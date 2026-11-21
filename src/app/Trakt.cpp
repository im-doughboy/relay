#include "Trakt.h"

#include "SecretStore.h"
#include "vod/Tmdb.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QUrlQuery>
#include <QtDebug>

namespace {
const char kRedirect[] = "urn:ietf:wg:oauth:2.0:oob";
constexpr int kSyncHours = 6;

QString fold(const QString &s)
{
    QString out;
    for (const QChar c : s.normalized(QString::NormalizationForm_KD))
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}
} // namespace

TraktService::TraktService(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    QSettings s;
    m_clientId = s.value(QStringLiteral("trakt/clientId")).toString();
    m_scrobble = s.value(QStringLiteral("trakt/scrobble"), true).toBool();
    m_user = s.value(QStringLiteral("trakt/user")).toString();
    m_expires = s.value(QStringLiteral("trakt/expires")).toLongLong();
    // Secret and sign-in: credential store, or the settings file where there is none.
    auto secret = [&s](const char *name) {
        const QString key = QStringLiteral("trakt-") + QLatin1String(name);
        if (s.value(QStringLiteral("trakt/secure")).toBool()) return SecretStore::read(key);
        return s.value(QStringLiteral("trakt/") + QLatin1String(name)).toString();
    };
    m_secret = secret("secret");
    m_access = secret("access");
    m_refresh = secret("refresh");

    m_pollTimer.setSingleShot(true);
    connect(&m_pollTimer, &QTimer::timeout, this, &TraktService::poll);
    m_syncTimer.setInterval(kSyncHours * 3600 * 1000);
    connect(&m_syncTimer, &QTimer::timeout, this, &TraktService::syncNow);
    if (connected()) {
        m_syncTimer.start();
        QTimer::singleShot(8000, this, &TraktService::syncNow);   // after the catalogue has loaded
    }
}

QString TraktService::apiBase()
{
    const QString env = qEnvironmentVariable("RELAY_TRAKT_URL");
    return env.isEmpty() ? QStringLiteral("https://api.trakt.tv") : env;
}

QString TraktService::lastSync() const
{
    const qint64 t = QSettings().value(QStringLiteral("trakt/lastSync")).toLongLong();
    return t > 0 ? QDateTime::fromSecsSinceEpoch(t).toString(QStringLiteral("ddd d MMM, HH:mm")) : QString();
}

void TraktService::setScrobbleOn(bool on)
{
    if (on == m_scrobble) return;
    m_scrobble = on;
    QSettings().setValue(QStringLiteral("trakt/scrobble"), on);
    emit changed();
}

void TraktService::setStatus(const QString &s)
{
    m_status = s;
    emit changed();
}

QString TraktService::titleKey(const QString &title)
{
    return fold(TmdbClient::clean(title).title);
}

// ---- requests --------------------------------------------------------------------------------

QNetworkReply *TraktService::call(const QByteArray &verb, const QString &path, const QJsonObject &body, bool auth)
{
    QNetworkRequest req(QUrl(apiBase() + path));
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("trakt-api-version", "2");
    req.setRawHeader("trakt-api-key", m_clientId.toUtf8());
    req.setRawHeader("User-Agent", "Relay");
    if (auth && !m_access.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_access.toUtf8());
    req.setTransferTimeout(20000);
    const QByteArray data = body.isEmpty() && verb == "GET" ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    return m_net->sendCustomRequest(req, verb, data);
}

void TraktService::api(const QByteArray &verb, const QString &path, const QJsonObject &body,
                       std::function<void(int, const QByteArray &)> done, bool retried)
{
    if (!connected()) { done(401, QByteArray()); return; }
    auto go = [=] {
        QNetworkReply *reply = call(verb, path, body);
        connect(reply, &QNetworkReply::finished, this, [=] {
            reply->deleteLater();
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = reply->readAll();
            if (http == 401 && !retried) {
                refreshTokens([=](bool ok) {
                    if (ok) api(verb, path, body, done, true);
                    else done(401, QByteArray());
                });
                return;
            }
            done(http, data);
        });
    };
    // Refresh a little before the sign-in runs out.
    if (m_expires > 0 && QDateTime::currentSecsSinceEpoch() > m_expires - 600 && !retried)
        refreshTokens([=](bool) { go(); });
    else
        go();
}

// ---- signing in --------------------------------------------------------------------------------

void TraktService::signIn(const QString &clientIdIn, const QString &secretIn)
{
    const QString clientId = clientIdIn.trimmed(), secret = secretIn.trimmed();
    if (!clientId.isEmpty()) m_clientId = clientId;
    if (!secret.isEmpty()) m_secret = secret;
    if (!configured()) { setStatus(tr("Enter your Trakt app's Client ID and Client Secret.")); return; }
    QSettings().setValue(QStringLiteral("trakt/clientId"), m_clientId);
    saveTokens();   // keeps the secret
    m_userCode.clear();
    setStatus(tr("Asking Trakt for a code…"));
    QNetworkReply *reply = call("POST", QStringLiteral("/oauth/device/code"), {{"client_id", m_clientId}}, false);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        if (http != 200 || o.value(QLatin1String("device_code")).toString().isEmpty()) {
            setStatus(http == 0 ? tr("Couldn't reach Trakt. Check the internet connection.")
                                : http == 401 || http == 403 ? tr("Trakt didn't accept this Client ID.")
                                                             : tr("Trakt answered with HTTP %1.").arg(http));
            return;
        }
        m_deviceCode = o.value(QLatin1String("device_code")).toString();
        m_userCode = o.value(QLatin1String("user_code")).toString();
        m_verifyUrl = o.value(QLatin1String("verification_url")).toString();
        m_interval = qMax(1, o.value(QLatin1String("interval")).toInt(5));
        m_codeExpires = QDateTime::currentSecsSinceEpoch() + o.value(QLatin1String("expires_in")).toInt(600);
        setStatus(tr("Waiting for you to enter the code on Trakt…"));
        m_pollTimer.start(m_interval * 1000);
    });
}

void TraktService::cancelSignIn()
{
    m_pollTimer.stop();
    m_deviceCode.clear();
    m_userCode.clear();
    setStatus(QString());
}

void TraktService::poll()
{
    if (m_deviceCode.isEmpty()) return;
    if (QDateTime::currentSecsSinceEpoch() > m_codeExpires) {
        cancelSignIn();
        setStatus(tr("The code ran out. Start again."));
        return;
    }
    QNetworkReply *reply = call("POST", QStringLiteral("/oauth/device/token"),
                                {{"code", m_deviceCode}, {"client_id", m_clientId}, {"client_secret", m_secret}}, false);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (m_deviceCode.isEmpty()) return;   // cancelled meanwhile
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        switch (http) {
        case 200: {
            m_deviceCode.clear();
            m_userCode.clear();
            tokensFrom(QJsonDocument::fromJson(reply->readAll()).object());
            setStatus(QString());
            qInfo().noquote() << "Trakt: signed in";
            fetchUser();
            m_syncTimer.start();
            syncNow();
            return;
        }
        case 400: break;                                       // not entered yet
        case 429: m_interval += 2; break;                      // asking too often
        case 418: cancelSignIn(); setStatus(tr("Signing in was refused on Trakt.")); return;
        case 401: cancelSignIn(); setStatus(tr("Trakt didn't accept the Client Secret.")); return;
        case 404: case 409: case 410:
            cancelSignIn(); setStatus(tr("The code ran out. Start again.")); return;
        default: break;                                        // try again
        }
        m_pollTimer.start(m_interval * 1000);
    });
}

void TraktService::tokensFrom(const QJsonObject &o)
{
    m_access = o.value(QLatin1String("access_token")).toString();
    m_refresh = o.value(QLatin1String("refresh_token")).toString();
    const qint64 created = qint64(o.value(QLatin1String("created_at")).toDouble());
    const qint64 lifetime = qint64(o.value(QLatin1String("expires_in")).toDouble());
    m_expires = lifetime > 0 ? (created > 0 ? created : QDateTime::currentSecsSinceEpoch()) + lifetime : 0;
    saveTokens();
    emit changed();
}

void TraktService::refreshTokens(std::function<void(bool)> done)
{
    if (m_refresh.isEmpty()) { done(false); return; }
    QNetworkReply *reply = call("POST", QStringLiteral("/oauth/token"),
                                {{"refresh_token", m_refresh}, {"client_id", m_clientId}, {"client_secret", m_secret},
                                 {"redirect_uri", QLatin1String(kRedirect)}, {"grant_type", "refresh_token"}}, false);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 200) {
            tokensFrom(QJsonDocument::fromJson(reply->readAll()).object());
            done(true);
            return;
        }
        if (http == 400 || http == 401) {
            // The sign-in was withdrawn on Trakt.
            qWarning().noquote() << "Trakt: the sign-in is no longer valid";
            clearTokens();
            emit message(tr("Trakt signed Relay out · sign in again in Settings"));
        }
        done(false);
    });
}

void TraktService::fetchUser()
{
    api("GET", QStringLiteral("/users/settings"), {}, [this](int http, const QByteArray &data) {
        if (http != 200) return;
        const QJsonObject u = QJsonDocument::fromJson(data).object().value(QLatin1String("user")).toObject();
        m_user = u.value(QLatin1String("username")).toString();
        QSettings().setValue(QStringLiteral("trakt/user"), m_user);
        emit changed();
    });
}

void TraktService::saveTokens()
{
    QSettings s;
    const bool secure = SecretStore::available();
    s.setValue(QStringLiteral("trakt/expires"), m_expires);
    for (const auto &[name, value] : {std::pair{"secret", m_secret}, std::pair{"access", m_access}, std::pair{"refresh", m_refresh}}) {
        const QString storeKey = QStringLiteral("trakt-") + QLatin1String(name);
        const QString fileKey = QStringLiteral("trakt/") + QLatin1String(name);
        if (secure) {
            if (value.isEmpty()) SecretStore::remove(storeKey);
            else SecretStore::write(storeKey, value);
            s.remove(fileKey);
        } else if (value.isEmpty()) {
            s.remove(fileKey);
        } else {
            s.setValue(fileKey, value);
        }
    }
    s.setValue(QStringLiteral("trakt/secure"), secure);
}

void TraktService::clearTokens()
{
    m_access.clear();
    m_refresh.clear();
    m_user.clear();
    m_expires = 0;
    m_syncTimer.stop();
    QSettings s;
    s.remove(QStringLiteral("trakt/user"));
    s.remove(QStringLiteral("trakt/lastSync"));
    saveTokens();
    emit changed();
}

void TraktService::signOut()
{
    cancelSignIn();
    if (!m_access.isEmpty()) {
        QNetworkReply *reply = call("POST", QStringLiteral("/oauth/revoke"),
                                    {{"token", m_access}, {"client_id", m_clientId}, {"client_secret", m_secret}}, false);
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    clearTokens();
    qInfo().noquote() << "Trakt: signed out";
}

// ---- matching titles -----------------------------------------------------------------------------

void TraktService::identify(const QVariantMap &d, std::function<void(const QJsonObject &)> done)
{
    const bool episode = d.value(QStringLiteral("kind")).toString() == QLatin1String("episode");
    const QString rawTitle = episode ? d.value(QStringLiteral("seriesTitle")).toString() : d.value(QStringLiteral("title")).toString();
    const TmdbClient::Query q = TmdbClient::clean(rawTitle, d.value(QStringLiteral("year")).toString());
    if (q.title.isEmpty()) { done({}); return; }
    const QString type = episode ? QStringLiteral("show") : QStringLiteral("movie");
    const QString key = type + QLatin1Char('|') + fold(q.title) + QLatin1Char('|') + q.year;

    auto wrap = [=](const QJsonObject &ids) {
        if (ids.isEmpty()) return QJsonObject();
        if (!episode) return QJsonObject{{"movie", QJsonObject{{"ids", ids}}}};
        return QJsonObject{{"show", QJsonObject{{"ids", ids}}},
                           {"episode", QJsonObject{{"season", d.value(QStringLiteral("season")).toInt()},
                                                   {"number", d.value(QStringLiteral("episode")).toInt()}}}};
    };
    const auto known = m_ids.constFind(key);
    if (known != m_ids.constEnd()) { done(wrap(known.value())); return; }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), q.title);
    if (!q.year.isEmpty()) query.addQueryItem(QStringLiteral("years"), q.year);
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("title"));
    api("GET", QStringLiteral("/search/%1?%2").arg(type, query.toString(QUrl::FullyEncoded)), {},
        [=](int http, const QByteArray &data) {
            QJsonObject ids;
            if (http == 200) {
                const QJsonArray results = QJsonDocument::fromJson(data).array();
                const QString want = fold(q.title);
                // Same title first, else the top result.
                for (const QJsonValue &v : results) {
                    const QJsonObject item = v.toObject().value(type).toObject();
                    if (fold(item.value(QLatin1String("title")).toString()) == want) { ids = item.value(QLatin1String("ids")).toObject(); break; }
                }
                if (ids.isEmpty() && !results.isEmpty())
                    ids = results.first().toObject().value(type).toObject().value(QLatin1String("ids")).toObject();
                m_ids.insert(key, ids);   // remembered even when nothing matched
            }
            if (ids.isEmpty())
                qInfo().noquote() << QStringLiteral("Trakt: no match for \"%1\" %2").arg(q.title, q.year);
            done(wrap(ids));
        });
}

// ---- watching ---------------------------------------------------------------------------------------

void TraktService::scrobble(const QString &action, const QVariantMap &d, double progress)
{
    if (!connected() || !m_scrobble) return;
    const QString kind = d.value(QStringLiteral("kind")).toString();
    if (kind != QLatin1String("movie") && kind != QLatin1String("episode")) return;
    const QString key = d.value(QStringLiteral("key")).toString();
    if (action == QLatin1String("start")) m_scrobbling = key;
    else if (action == QLatin1String("stop") && m_scrobbling != key && progress < 80) return;   // never started
    else if (action == QLatin1String("pause") && m_scrobbling != key) return;
    if (action == QLatin1String("stop")) m_scrobbling.clear();
    const double pct = qBound(0.0, progress, 100.0);
    identify(d, [=](const QJsonObject &what) {
        if (what.isEmpty()) return;
        QJsonObject body = what;
        body.insert(QStringLiteral("progress"), pct);
        api("POST", QStringLiteral("/scrobble/") + action, body, [=](int http, const QByteArray &) {
            if (http == 201 || http == 409)   // 409: already scrobbled just now
                qInfo().noquote() << QStringLiteral("Trakt: %1 \"%2\" at %3%").arg(action, d.value(QStringLiteral("title")).toString())
                                         .arg(pct, 0, 'f', 0);
            else
                qWarning().noquote() << QStringLiteral("Trakt: %1 failed (HTTP %2)").arg(action).arg(http);
            if (action == QLatin1String("stop") && pct >= 80 && http == 201)
                emit message(tr("Added to your Trakt history"));
        });
    });
}

void TraktService::markWatched(const QVariantMap &d, bool watched)
{
    if (!connected()) return;
    identify(d, [=](const QJsonObject &what) {
        if (what.isEmpty()) return;
        QJsonObject body;
        if (what.contains(QLatin1String("movie"))) {
            body.insert(QStringLiteral("movies"), QJsonArray{what.value(QLatin1String("movie"))});
        } else {
            QJsonObject show = what.value(QLatin1String("show")).toObject();
            const QJsonObject ep = what.value(QLatin1String("episode")).toObject();
            show.insert(QStringLiteral("seasons"), QJsonArray{QJsonObject{
                {"number", ep.value(QLatin1String("season"))},
                {"episodes", QJsonArray{QJsonObject{{"number", ep.value(QLatin1String("number"))}}}}}});
            body.insert(QStringLiteral("shows"), QJsonArray{show});
        }
        api("POST", watched ? QStringLiteral("/sync/history") : QStringLiteral("/sync/history/remove"), body,
            [=](int http, const QByteArray &) {
                qInfo().noquote() << QStringLiteral("Trakt: %1 \"%2\" (HTTP %3)")
                                         .arg(watched ? QStringLiteral("marked watched") : QStringLiteral("unmarked"),
                                              d.value(QStringLiteral("title")).toString())
                                         .arg(http);
            });
    });
}

QVariantMap TraktService::parseWatched(const QJsonArray &movies, const QJsonArray &shows)
{
    QVariantList outMovies, outEpisodes;
    for (const QJsonValue &v : movies) {
        const QJsonObject m = v.toObject().value(QLatin1String("movie")).toObject();
        const QString title = m.value(QLatin1String("title")).toString();
        if (title.isEmpty()) continue;
        outMovies.append(QVariantMap{{QStringLiteral("title"), title}, {QStringLiteral("year"), m.value(QLatin1String("year")).toInt()}});
    }
    for (const QJsonValue &v : shows) {
        const QJsonObject entry = v.toObject();
        const QJsonObject show = entry.value(QLatin1String("show")).toObject();
        const QString title = show.value(QLatin1String("title")).toString();
        if (title.isEmpty()) continue;
        for (const QJsonValue &s : entry.value(QLatin1String("seasons")).toArray()) {
            const int season = s.toObject().value(QLatin1String("number")).toInt();
            for (const QJsonValue &e : s.toObject().value(QLatin1String("episodes")).toArray())
                outEpisodes.append(QVariantMap{{QStringLiteral("title"), title},
                                               {QStringLiteral("year"), show.value(QLatin1String("year")).toInt()},
                                               {QStringLiteral("season"), season},
                                               {QStringLiteral("episode"), e.toObject().value(QLatin1String("number")).toInt()}});
        }
    }
    return {{QStringLiteral("movies"), outMovies}, {QStringLiteral("episodes"), outEpisodes}};
}

void TraktService::syncNow()
{
    if (!connected()) return;
    auto movies = std::make_shared<QJsonArray>();
    api("GET", QStringLiteral("/sync/watched/movies"), {}, [=](int http, const QByteArray &data) {
        if (http != 200) { qWarning().noquote() << "Trakt: couldn't fetch watched movies, HTTP" << http; return; }
        *movies = QJsonDocument::fromJson(data).array();
        api("GET", QStringLiteral("/sync/watched/shows"), {}, [=](int http2, const QByteArray &data2) {
            if (http2 != 200) { qWarning().noquote() << "Trakt: couldn't fetch watched shows, HTTP" << http2; return; }
            const QVariantMap watched = parseWatched(*movies, QJsonDocument::fromJson(data2).array());
            QSettings().setValue(QStringLiteral("trakt/lastSync"), QDateTime::currentSecsSinceEpoch());
            qInfo().noquote() << QStringLiteral("Trakt: %1 movies and %2 episodes watched on Trakt")
                                     .arg(watched.value(QStringLiteral("movies")).toList().size())
                                     .arg(watched.value(QStringLiteral("episodes")).toList().size());
            emit watchedFetched(watched);
            emit changed();
        });
    });
}
