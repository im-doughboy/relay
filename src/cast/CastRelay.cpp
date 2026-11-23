#include "CastRelay.h"

#include "TsSegmenter.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QtDebug>

namespace {
constexpr quint16 kFirstPort = 8790;
constexpr qint64 kSocketHighWater = 4 * 1024 * 1024;
constexpr qint64 kWaitForSegmentsMs = 25000;

QString newToken()
{
    QString t;
    for (int i = 0; i < 4; ++i) t += QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, QLatin1Char('0'));
    return t;
}

QByteArray statusText(int status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 404: return "Not Found";
    case 416: return "Range Not Satisfiable";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

// Lets receivers that fetch with JavaScript (Chromecast) read the answers, and
// tells DLNA TVs it's a stream they can play as it comes.
QByteArray commonHeaders(const QHash<QByteArray, QByteArray> &request, const QString &features = QString())
{
    QByteArray h = "Access-Control-Allow-Origin: *\r\n"
                   "Access-Control-Allow-Headers: *\r\n"
                   "Access-Control-Expose-Headers: Content-Length, Content-Range\r\n"
                   "Server: Relay\r\n";
    if (request.contains("getcontentfeatures.dlna.org"))
        h += "transferMode.dlna.org: Streaming\r\n"
             "contentFeatures.dlna.org: "
             + (features.isEmpty() ? QByteArray("DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000")
                                   : features.toLatin1()) + "\r\n";
    return h;
}

bool looksLikePlaylist(const QByteArray &type, const QUrl &url)
{
    const QByteArray t = type.toLower();
    return t.contains("mpegurl") || url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}
} // namespace

CastRelay::CastRelay(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this)), m_net(new QNetworkAccessManager(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &CastRelay::onConnection);
    m_waitTimer.setInterval(500);
    connect(&m_waitTimer, &QTimer::timeout, this, &CastRelay::answerWaiting);
}

CastRelay::~CastRelay() { clear(); }

bool CastRelay::ensureListening()
{
    if (m_server->isListening()) return true;
    // A fixed port when free, so a firewall rule keeps working.
    for (quint16 p = kFirstPort; p < kFirstPort + 10; ++p)
        if (m_server->listen(QHostAddress::AnyIPv4, p)) break;
    if (!m_server->isListening() && !m_server->listen(QHostAddress::AnyIPv4, 0)) {
        qWarning().noquote() << "Cast: couldn't start the stream relay:" << m_server->errorString();
        return false;
    }
    qInfo().noquote() << "Cast: stream relay on port" << m_server->serverPort();
    return true;
}

quint16 CastRelay::port() const { return m_server->serverPort(); }

QString CastRelay::add(const Source &source)
{
    if (!ensureListening()) return QString();
    const QString token = newToken();
    Entry e;
    e.source = source;
    if (source.asHls) e.segmenter = std::make_shared<TsSegmenter>();
    m_entries.insert(token, e);
    if (source.asHls) {
        startFeed(token);
        return QStringLiteral("/h/%1/live.m3u8").arg(token);
    }
    return QStringLiteral("/s/%1/stream").arg(token);
}

void CastRelay::remove(const QString &path)
{
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2) return;
    auto it = m_entries.find(parts[1]);
    if (it == m_entries.end()) return;
    if (it->feed) it->feed->abort();
    m_entries.erase(it);
}

void CastRelay::clear()
{
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
        if (it->feed) it->feed->abort();
    m_entries.clear();
    m_waiting.clear();
    m_waitTimer.stop();
}

QHostAddress CastRelay::localAddressFacing(const QHostAddress &peer)
{
    if (peer.isLoopback()) return QHostAddress(QHostAddress::LocalHost);
    QHostAddress fallback;
    for (const QNetworkInterface &nif : QNetworkInterface::allInterfaces()) {
        if (!(nif.flags() & QNetworkInterface::IsUp) || (nif.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const QNetworkAddressEntry &a : nif.addressEntries()) {
            if (a.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            if (fallback.isNull()) fallback = a.ip();
            if (a.prefixLength() > 0 && peer.isInSubnet(a.ip(), a.prefixLength())) return a.ip();
        }
    }
    return fallback.isNull() ? QHostAddress(QHostAddress::LocalHost) : fallback;
}

QString CastRelay::urlFor(const QString &path, const QHostAddress &peer) const
{
    return QStringLiteral("http://%1:%2%3").arg(localAddressFacing(peer).toString()).arg(m_server->serverPort()).arg(path);
}

QByteArray CastRelay::rewritePlaylist(const QByteArray &playlist, const QUrl &base,
                                      const std::function<QString(const QString &)> &register_)
{
    QByteArray out;
    for (const QByteArray &rawLine : playlist.split('\n')) {
        QByteArray line = rawLine;
        if (line.endsWith('\r')) line.chop(1);
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty()) { out += '\n'; continue; }
        if (trimmed.startsWith('#')) {
            // Keys, init sections and alternate renditions: URI="…".
            const int at = trimmed.indexOf("URI=\"");
            if (at >= 0) {
                const int end = trimmed.indexOf('"', at + 5);
                if (end > at) {
                    const QString target = base.resolved(QUrl(QString::fromUtf8(trimmed.mid(at + 5, end - at - 5)))).toString();
                    line = trimmed.left(at + 5) + register_(target).toUtf8() + trimmed.mid(end);
                }
            }
            out += line + '\n';
            continue;
        }
        out += register_(base.resolved(QUrl(QString::fromUtf8(trimmed))).toString()).toUtf8() + '\n';
    }
    return out;
}

// ---- requests --------------------------------------------------------------------------------

void CastRelay::onConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection()) {
        auto head = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, head] {
            if (socket->property("handled").toBool()) { socket->readAll(); return; }
            head->append(socket->readAll());
            const int end = head->indexOf("\r\n\r\n");
            if (end < 0) {
                if (head->size() > 32 * 1024) socket->abort();
                return;
            }
            socket->setProperty("handled", true);
            onRequest(socket, head->left(end));
        });
    }
}

void CastRelay::reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body, const QByteArray &method)
{
    if (!socket) return;
    QByteArray h = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText(status) + "\r\n";
    h += commonHeaders({});
    if (!type.isEmpty()) h += "Content-Type: " + type + "\r\n";
    h += "Content-Length: " + QByteArray::number(body.size()) + "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    socket->write(h);
    if (method != "HEAD") socket->write(body);
    socket->disconnectFromHost();
}

void CastRelay::onRequest(QTcpSocket *socket, const QByteArray &head)
{
    const QList<QByteArray> lines = head.split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    const QByteArray method = first.value(0);
    const QString path = QString::fromUtf8(first.value(1));
    QHash<QByteArray, QByteArray> headers;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines[i].indexOf(':');
        if (colon > 0) headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
    }
    // What the receiver asks for (says why a TV gives up), without the address.
    qInfo().noquote() << QStringLiteral("Cast: %1 %2 asks %3 /%4%5%6").arg(socket->peerAddress().toString(),
                             QString::fromLatin1(headers.value("user-agent").left(60)), QString::fromLatin1(method),
                             path.section(QLatin1Char('/'), 1, 1),
                             headers.contains("range") ? QStringLiteral(" range ") + QString::fromLatin1(headers.value("range")) : QString(),
                             headers.contains("getcontentfeatures.dlna.org") ? QStringLiteral(" +dlna features") : QString());
    if (method == "OPTIONS") { reply(socket, 200, QByteArray(), QByteArray()); return; }
    if (method != "GET" && method != "HEAD") { reply(socket, 404, "text/plain", "no"); return; }

    const QStringList parts = path.split(QLatin1Char('?')).first().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 3 || !m_entries.contains(parts[1])) { reply(socket, 404, "text/plain", "not found", method); return; }
    const QString token = parts[1];
    Entry &e = m_entries[token];

    if (parts[0] == QLatin1String("h")) {                       // HLS made here
        if (parts[2] == QLatin1String("live.m3u8")) {
            const QByteArray list = e.segmenter->playlist(QString(), 2);
            if (!list.isEmpty()) { reply(socket, 200, "application/vnd.apple.mpegurl", list, method); return; }
            m_waiting.append({socket, token, QDateTime::currentMSecsSinceEpoch()});
            m_waitTimer.start();
            return;
        }
        const QByteArray seg = e.segmenter->segment(parts[2].section(QLatin1Char('.'), 0, 0).toInt());
        if (seg.isEmpty()) reply(socket, 404, "text/plain", "gone", method);
        else reply(socket, 200, "video/mp2t", seg, method);
        return;
    }
    if (parts[0] == QLatin1String("s")) {                       // the stream itself
        pipe(socket, token, e.source.url, method, headers);
        return;
    }
    if (parts[0] == QLatin1String("u")) {                       // a part of an HLS playlist
        const int index = parts[2].section(QLatin1Char('.'), 0, 0).toInt();
        if (index < 0 || index >= e.parts.size()) { reply(socket, 404, "text/plain", "not found", method); return; }
        pipe(socket, token, e.parts[index], method, headers);
        return;
    }
    reply(socket, 404, "text/plain", "not found", method);
}

void CastRelay::pipe(QTcpSocket *socket, const QString &token, const QString &url, const QByteArray &method,
                     const QHash<QByteArray, QByteArray> &headers)
{
    const Source source = m_entries.value(token).source;
    QNetworkRequest req{QUrl(url)};
    if (!source.userAgent.isEmpty()) req.setRawHeader("User-Agent", source.userAgent.toUtf8());
    if (!source.referrer.isEmpty()) req.setRawHeader("Referer", source.referrer.toUtf8());
    if (headers.contains("range")) req.setRawHeader("Range", headers.value("range"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *up = method == "HEAD" ? m_net->head(req) : m_net->get(req);
    up->setReadBufferSize(2 * 1024 * 1024);
    QPointer<QTcpSocket> sock(socket);

    auto state = std::make_shared<int>(0);   // 0 waiting for headers, 1 streaming, 2 collecting a playlist
    auto playlistBody = std::make_shared<QByteArray>();
    auto pump = [up, sock, state] {
        if (!sock || *state != 1) return;
        while (up->bytesAvailable() > 0 && sock->bytesToWrite() < kSocketHighWater)
            sock->write(up->read(256 * 1024));
    };
    connect(up, &QNetworkReply::metaDataChanged, this, [=] {
        if (!sock || *state != 0) return;
        const int status = up->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 300 && status < 400) return;               // following a redirect
        QByteArray type = up->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        if (looksLikePlaylist(type, up->url())) { *state = 2; return; }
        if (type.isEmpty() || type.startsWith("application/octet-stream")) {
            const QString p = QUrl(url).path().toLower();
            type = !source.contentType.isEmpty() ? source.contentType.toUtf8()
                 : p.endsWith(QLatin1String(".mp4")) ? QByteArray("video/mp4")
                 : p.endsWith(QLatin1String(".mkv")) ? QByteArray("video/x-matroska")
                 : QByteArray("video/mp2t");
        }
        QByteArray h = "HTTP/1.1 " + QByteArray::number(status == 206 ? 206 : status >= 400 ? status : 200) + ' '
                     + statusText(status == 206 ? 206 : 200) + "\r\n";
        if (source.forceType && !source.contentType.isEmpty()) type = source.contentType.toUtf8();
        h += commonHeaders(headers, source.dlnaFeatures);
        h += "Content-Type: " + type + "\r\n";
        for (const QByteArray name : {QByteArray("Content-Length"), QByteArray("Content-Range"), QByteArray("Accept-Ranges")})
            if (up->hasRawHeader(name)) h += name + ": " + up->rawHeader(name) + "\r\n";
        h += "Connection: close\r\n\r\n";
        sock->write(h);
        *state = 1;
        qInfo().noquote() << QStringLiteral("Cast: answered %1 with %2, %3%4").arg(QString::fromLatin1(method))
                                 .arg(status == 206 ? 206 : status >= 400 ? status : 200).arg(QString::fromLatin1(type),
                                      up->hasRawHeader("Content-Length") ? QStringLiteral(", ") + QString::fromLatin1(up->rawHeader("Content-Length")) + QStringLiteral(" bytes") : QStringLiteral(", live"));
        if (method == "HEAD") { up->abort(); sock->disconnectFromHost(); }
    });
    connect(up, &QNetworkReply::readyRead, this, [=] {
        if (*state == 2) playlistBody->append(up->readAll());
        else pump();
    });
    if (socket) connect(socket, &QTcpSocket::bytesWritten, up, [pump] { pump(); });
    connect(socket, &QTcpSocket::disconnected, up, [up] { if (up->isRunning()) up->abort(); });
    connect(up, &QNetworkReply::finished, this, [=] {
        up->deleteLater();
        if (!sock) return;
        if (*state == 2) {
            playlistBody->append(up->readAll());
            auto it = m_entries.find(token);
            if (it == m_entries.end()) { reply(sock, 404, "text/plain", "gone"); return; }
            const QByteArray list = rewritePlaylist(*playlistBody, up->url(), [this, token](const QString &target) {
                Entry &e = m_entries[token];
                int index = e.parts.indexOf(target);
                if (index < 0) { e.parts.append(target); index = int(e.parts.size()) - 1; }
                const QString ext = QUrl(target).path().endsWith(QLatin1String(".m3u8")) ? QStringLiteral(".m3u8") : QStringLiteral(".ts");
                return QStringLiteral("/u/%1/%2%3").arg(token).arg(index).arg(ext);
            });
            reply(sock, 200, "application/vnd.apple.mpegurl", list, method);
            return;
        }
        if (*state == 0) {   // never got going
            const int status = up->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qWarning().noquote() << "Cast: the provider answered" << (status ? QString::number(status) : up->errorString());
            reply(sock, 502, "text/plain", "provider error");
            return;
        }
        // Whatever's left, then close once it has gone out.
        sock->write(up->readAll());
        sock->disconnectFromHost();
    });
}

// ---- live MPEG-TS -> HLS -----------------------------------------------------------------------

void CastRelay::startFeed(const QString &token)
{
    auto it = m_entries.find(token);
    if (it == m_entries.end()) return;
    QNetworkRequest req{QUrl(it->source.url)};
    if (!it->source.userAgent.isEmpty()) req.setRawHeader("User-Agent", it->source.userAgent.toUtf8());
    if (!it->source.referrer.isEmpty()) req.setRawHeader("Referer", it->source.referrer.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *feed = m_net->get(req);
    it->feed = feed;
    std::shared_ptr<TsSegmenter> seg = it->segmenter;
    connect(feed, &QNetworkReply::readyRead, this, [this, feed, seg, token] {
        seg->feed(feed->readAll());
        // Flowing again: a later drop gets the full number of retries.
        auto e = m_entries.find(token);
        if (e != m_entries.end() && e->feedFailures > 0 && seg->segments().size() > 1) e->feedFailures = 0;
        if (!m_waiting.isEmpty()) answerWaiting();
    });
    connect(feed, &QNetworkReply::finished, this, [this, feed, token] {
        feed->deleteLater();
        auto e = m_entries.find(token);
        if (e == m_entries.end() || e->feed != feed) return;   // stopped
        // Live streams shouldn't end: connect again, a few times.
        const bool failed = feed->error() != QNetworkReply::NoError;
        qWarning().noquote() << "Cast: the channel's stream" << (failed ? "failed: " + feed->errorString() : QStringLiteral("ended"));
        if (++e->feedFailures > 5) return;
        QTimer::singleShot(1000 * e->feedFailures, this, [this, token] { startFeed(token); });
    });
}

void CastRelay::answerWaiting()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_waiting.size() - 1; i >= 0; --i) {
        Waiting &w = m_waiting[i];
        if (!w.socket) { m_waiting.removeAt(i); continue; }
        const auto it = m_entries.constFind(w.token);
        const QByteArray list = it != m_entries.constEnd() ? it->segmenter->playlist(QString(), 2) : QByteArray();
        if (!list.isEmpty()) reply(w.socket, 200, "application/vnd.apple.mpegurl", list);
        else if (it == m_entries.constEnd() || now - w.since > kWaitForSegmentsMs) reply(w.socket, 503, "text/plain", "not ready");
        else continue;
        m_waiting.removeAt(i);
    }
    if (m_waiting.isEmpty()) m_waitTimer.stop();
}
