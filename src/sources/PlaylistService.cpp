#include "app/AppSettings.h"
#include "PlaylistService.h"

#include "M3uParser.h"
#include "PortalClient.h"
#include "XtreamParser.h"
#include "app/Log.h"

#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtConcurrent>

namespace {
constexpr int kTimeoutMs = 45000;
}

PlaylistService::PlaylistService(QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    qRegisterMetaType<PlaylistResult>();
}

void PlaylistService::begin(const QString &status)
{
    ++m_generation;
    if (!m_busy) { m_busy = true; emit busyChanged(); }
    setStatus(status);
}

void PlaylistService::setStatus(const QString &status)
{
    if (m_status == status) return;
    m_status = status;
    emit statusChanged();
}

void PlaylistService::finishWith(const PlaylistResult &result)
{
    if (!result.error.isEmpty()) qWarning().noquote() << "Playlist: failed:" << result.error;
    else qInfo().noquote() << QStringLiteral("Playlist: %1 live channels (skipped %2 movies, "
                                             "%3 series episodes) · guide address: %4")
                                  .arg(result.channels.size())
                                  .arg(result.skippedMovies)
                                  .arg(result.skippedSeries)
                                  .arg(result.epgUrl.isEmpty() ? QStringLiteral("none")
                                                               : Log::redact(result.epgUrl));
    m_busy = false;
    emit busyChanged();
    setStatus(QString());
    if (!result.error.isEmpty()) emit failed(result.error);
    else emit loaded(result);
}

void PlaylistService::fail(const QString &message)
{
    PlaylistResult r;
    r.error = message;
    finishWith(r);
}

void PlaylistService::cancel()
{
    ++m_generation;
    if (m_busy) { m_busy = false; emit busyChanged(); }
    setStatus(QString());
}

QNetworkReply *PlaylistService::get(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  AppSettings::effectiveUserAgent());
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_net->get(req);
}

QString PlaylistService::describeError(QNetworkReply *reply)
{
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http >= 400) return QStringLiteral("The server responded with HTTP %1.").arg(http);
    switch (reply->error()) {
    case QNetworkReply::HostNotFoundError: return QStringLiteral("Server not found. Check the address.");
    case QNetworkReply::ConnectionRefusedError: return QStringLiteral("The server refused the connection.");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError: return QStringLiteral("The server took too long to respond.");
    default: return reply->errorString();
    }
}

// ---- M3U --------------------------------------------------------------------

void PlaylistService::loadM3u(const QString &location)
{
    const QString loc = location.trimmed();
    if (loc.isEmpty()) { fail(QStringLiteral("Enter a playlist URL or file.")); return; }

    begin(QStringLiteral("Downloading playlist…"));
    const quint64 gen = m_generation;

    const QUrl url = QUrl::fromUserInput(loc, QString(), QUrl::AssumeLocalFile);
    qInfo().noquote() << "Playlist: loading" << Log::redact(url.toString());

    auto parseAsync = [this, gen, url](QByteArray data) {
        setStatus(QStringLiteral("Reading channels…"));
        QtConcurrent::run([data, url] {
            PlaylistResult r = M3uParser::parse(data);
            if (r.epgUrl.isEmpty()) r.epgUrl = XtreamParser::guideUrlForPlaylist(url);
            XtreamParser::Credentials c;
            if (XtreamParser::credentialsFromPlaylistUrl(url, &c)) {
                r.xtreamServer = c.server;
                r.xtreamUser = c.username;
                r.xtreamPassword = c.password;
            }
            return r;
        })
            .then(this, [this, gen](PlaylistResult r) {
                if (gen == m_generation) finishWith(r);
            });
    };

    if (url.isLocalFile()) {
        const QString path = url.toLocalFile();
        if (!QFileInfo::exists(path)) { fail(QStringLiteral("File not found: %1").arg(path)); return; }
        QtConcurrent::run([path] {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        }).then(this, [parseAsync, gen, this](QByteArray data) {
            if (gen != m_generation) return;
            if (data.isEmpty()) fail(QStringLiteral("Couldn't read the playlist file."));
            else parseAsync(data);
        });
        return;
    }

    QNetworkReply *reply = get(url);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, gen](qint64 got, qint64) {
        if (gen == m_generation && got > 0)
            setStatus(QStringLiteral("Downloading playlist… %1 MB").arg(got / 1048576.0, 0, 'f', 1));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, parseAsync] {
        reply->deleteLater();
        if (gen != m_generation) return;
        if (reply->error() != QNetworkReply::NoError) { fail(describeError(reply)); return; }
        parseAsync(reply->readAll());
    });
}

// ---- Xtream Codes ---------------------------------------------------------------

void PlaylistService::loadXtream(const QString &server, const QString &username,
                                 const QString &password, const QString &output)
{
    if (output == QLatin1String("portal")) { loadPortal(server, username); return; }
    if (server.trimmed().isEmpty() || username.isEmpty()) {
        fail(QStringLiteral("Enter the server address, username and password."));
        return;
    }

    XtreamParser::Credentials c{server, username, password, output};
    begin(QStringLiteral("Signing in…"));
    const quint64 gen = m_generation;

    QNetworkReply *auth = get(XtreamParser::apiUrl(c));
    connect(auth, &QNetworkReply::finished, this, [=] {
        auth->deleteLater();
        if (gen != m_generation) return;
        if (auth->error() != QNetworkReply::NoError) { fail(describeError(auth)); return; }
        const auto a = XtreamParser::parseAuth(auth->readAll());
        if (!a.ok) { fail(a.message); return; }

        setStatus(QStringLiteral("Loading categories…"));
        QNetworkReply *cats = get(XtreamParser::apiUrl(c, QStringLiteral("get_live_categories")));
        connect(cats, &QNetworkReply::finished, this, [=] {
            cats->deleteLater();
            if (gen != m_generation) return;
            if (cats->error() != QNetworkReply::NoError) { fail(describeError(cats)); return; }
            QStringList order;
            const auto categories = XtreamParser::parseCategories(cats->readAll(), &order);

            setStatus(QStringLiteral("Loading channels…"));
            QNetworkReply *streams = get(XtreamParser::apiUrl(c, QStringLiteral("get_live_streams")));
            connect(streams, &QNetworkReply::finished, this, [=] {
                streams->deleteLater();
                if (gen != m_generation) return;
                if (streams->error() != QNetworkReply::NoError) { fail(describeError(streams)); return; }
                const QByteArray data = streams->readAll();
                QtConcurrent::run([data, categories, c, order] {
                    PlaylistResult r = XtreamParser::parseLiveStreams(data, categories, c, order);
                    r.xtreamServer = c.server;
                    r.xtreamUser = c.username;
                    r.xtreamPassword = c.password;
                    return r;
                }).then(this, [this, gen](PlaylistResult r) {
                    if (gen == m_generation) finishWith(r);
                });
            });
        });
    });
}

// ---- Stalker / MAC portal ---------------------------------------------------------

void PlaylistService::loadPortal(const QString &server, const QString &mac)
{
    begin(QStringLiteral("Signing in to the portal…"));
    const quint64 gen = m_generation;
    PortalClient::instance()->load(
        server, mac, this,
        [this, gen](const QString &status) { if (gen == m_generation) setStatus(status); },
        [this, gen](const PlaylistResult &result) { if (gen == m_generation) finishWith(result); });
}
