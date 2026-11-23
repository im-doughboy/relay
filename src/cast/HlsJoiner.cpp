#include "HlsJoiner.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QtDebug>

namespace {
constexpr qint64 kHighWater = 4 * 1024 * 1024;   // don't get further ahead of the TV than this
constexpr int kStartBehind = 3;                   // start this many segments from the live edge
constexpr int kMaxFailures = 6;

QByteArray attribute(const QByteArray &line, const QByteArray &name)
{
    const int at = line.indexOf(name + '=');
    if (at < 0) return {};
    int from = at + name.size() + 1;
    if (from < line.size() && line.at(from) == '"') {
        const int end = line.indexOf('"', from + 1);
        return line.mid(from + 1, end < 0 ? -1 : end - from - 1);
    }
    const int end = line.indexOf(',', from);
    return line.mid(from, end < 0 ? -1 : end - from).trimmed();
}
} // namespace

HlsJoiner::Playlist HlsJoiner::parse(const QByteArray &text, const QUrl &base)
{
    Playlist p;
    qint64 pendingBandwidth = -1;
    for (QByteArray line : text.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith("#EXT-X-STREAM-INF")) {
            p.master = true;
            pendingBandwidth = attribute(line, "BANDWIDTH").toLongLong();
        } else if (line.startsWith("#EXT-X-MEDIA-SEQUENCE:")) {
            p.sequence = line.mid(22).trimmed().toLongLong();
        } else if (line.startsWith("#EXT-X-TARGETDURATION:")) {
            p.targetDuration = qMax(1.0, line.mid(22).trimmed().toDouble());
        } else if (line.startsWith("#EXT-X-ENDLIST")) {
            p.ended = true;
        } else if (line.startsWith("#EXT-X-KEY")) {
            const QByteArray method = attribute(line, "METHOD");
            p.encrypted = !method.isEmpty() && method != "NONE";
        } else if (line.startsWith("#EXT-X-MAP")) {
            p.fmp4 = true;
        } else if (!line.startsWith('#')) {
            const QUrl url = base.resolved(QUrl(QString::fromUtf8(line)));
            if (pendingBandwidth >= 0) { p.variants.append({url, pendingBandwidth}); pendingBandwidth = -1; }
            else if (!p.master) p.segments.append(url);
        }
    }
    return p;
}

QUrl HlsJoiner::pickVariant(const Playlist &master)
{
    QUrl best;
    qint64 bandwidth = -1;
    for (const auto &v : master.variants)
        if (v.second > bandwidth) { bandwidth = v.second; best = v.first; }
    return best;
}

HlsJoiner::HlsJoiner(QNetworkAccessManager *net, const QUrl &url, const QString &userAgent, const QString &referrer,
                     QTcpSocket *socket, QObject *parent)
    : QObject(parent), m_net(net), m_url(url), m_userAgent(userAgent), m_referrer(referrer), m_socket(socket)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &HlsJoiner::fetchPlaylist);
    connect(socket, &QTcpSocket::disconnected, this, [this] { stop(QStringLiteral("the TV closed the stream")); });
    connect(socket, &QTcpSocket::bytesWritten, this, [this] {
        // Caught up enough: carry on with the segments that were waiting.
        if (!m_reply && !m_queue.isEmpty() && m_socket && m_socket->bytesToWrite() < kHighWater / 2) nextSegment();
    });
}

void HlsJoiner::start() { fetchPlaylist(); }

QNetworkReply *HlsJoiner::get(const QUrl &url)
{
    QNetworkRequest req(url);
    if (!m_userAgent.isEmpty()) req.setRawHeader("User-Agent", m_userAgent.toUtf8());
    if (!m_referrer.isEmpty()) req.setRawHeader("Referer", m_referrer.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20000);
    return m_net->get(req);
}

void HlsJoiner::fetchPlaylist()
{
    if (m_stopped || m_reply) return;
    QNetworkReply *reply = get(m_url);
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_reply = nullptr;
        if (m_stopped) return;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << "Cast: the channel's playlist failed:" << reply->errorString();
            if (++m_failures > kMaxFailures) { stop(QStringLiteral("the channel stopped answering")); return; }
            schedulePlaylist(qMin(1.0 * m_failures, 5.0));
            return;
        }
        onPlaylist(reply->readAll(), reply->url());
    });
}

void HlsJoiner::onPlaylist(const QByteArray &body, const QUrl &url)
{
    const Playlist p = parse(body, url);
    if (p.master) {
        const QUrl variant = pickVariant(p);
        if (!variant.isValid() || ++m_masterHops > 3) { stop(QStringLiteral("the channel's playlist lists no streams")); return; }
        m_url = variant;
        fetchPlaylist();
        return;
    }
    if (p.encrypted || p.fmp4) {
        stop(p.encrypted ? QStringLiteral("the channel is encrypted (it can't be passed on as one stream)")
                         : QStringLiteral("the channel isn't MPEG-TS inside (fMP4)"));
        return;
    }
    m_target = p.targetDuration;
    if (p.segments.isEmpty()) { schedulePlaylist(m_target / 2); return; }
    const qint64 last = p.sequence + p.segments.size() - 1;
    if (m_next < 0) m_next = p.ended ? p.sequence : qMax(p.sequence, last - kStartBehind + 1);
    if (m_next < p.sequence) {
        qInfo().noquote() << "Cast: fell behind the channel by" << (p.sequence - m_next) << "segments; skipping ahead";
        m_next = p.sequence;
    }
    // Segments not yet queued or sent.
    const qint64 queuedUpTo = m_queue.isEmpty() ? m_next - 1 : m_queue.last().first;
    for (int i = 0; i < p.segments.size(); ++i) {
        const qint64 n = p.sequence + i;
        if (n > queuedUpTo) m_queue.append({n, p.segments[i]});
    }
    m_failures = 0;
    if (!m_queue.isEmpty()) nextSegment();
    else if (p.ended) stop(QString());
    else schedulePlaylist(m_target / 2);
}

void HlsJoiner::nextSegment()
{
    if (m_stopped || m_reply || !m_socket) return;
    if (m_queue.isEmpty()) { schedulePlaylist(m_target / 2); return; }
    if (m_socket->bytesToWrite() > kHighWater) return;   // bytesWritten brings us back
    const auto [number, url] = m_queue.takeFirst();
    QNetworkReply *reply = get(url);
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        if (m_socket) { const QByteArray data = reply->readAll(); m_sent += data.size(); m_socket->write(data); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, number] {
        reply->deleteLater();
        m_reply = nullptr;
        if (m_stopped) return;
        if (reply->error() != QNetworkReply::NoError)
            qWarning().noquote() << "Cast: a segment failed, skipping it:" << reply->errorString();
        else if (m_socket) { const QByteArray rest = reply->readAll(); m_sent += rest.size(); m_socket->write(rest); }
        m_next = number + 1;
        if (!m_queue.isEmpty()) nextSegment();
        else schedulePlaylist(m_target / 2);
    });
}

void HlsJoiner::schedulePlaylist(double seconds)
{
    if (m_stopped) return;
    m_timer.start(int(qBound(0.5, seconds, 10.0) * 1000));
}

void HlsJoiner::stop(const QString &why)
{
    if (m_stopped) return;
    m_stopped = true;
    m_timer.stop();
    if (m_reply) m_reply->abort();
    if (!why.isEmpty()) qInfo().noquote() << "Cast: stopped passing the channel on:" << why << QStringLiteral("(%1 MB sent)").arg(m_sent / 1048576.0, 0, 'f', 1);
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState) m_socket->disconnectFromHost();
    deleteLater();
}
