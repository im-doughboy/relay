#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Trakt (trakt.tv): what's watched in Relay goes to the user's Trakt
// history, and what's marked watched on Trakt shows as watched in Relay.
//
// Needs the user's own Trakt app (developer.trakt.tv → Create an App, named
// "Relay"): its Client ID. Apps made since Trakt's 2026 changes have no
// Client Secret (Trakt asks desktop apps not to send one); an older app's
// secret is still sent if the user enters it. Signing in uses Trakt's
// device codes: Relay shows a short code, the user enters it at
// trakt.tv/activate.
//
// The secret and the sign-in are kept in the system credential store where
// there is one.
class TraktService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool configured READ configured NOTIFY changed)       // Client ID entered
    Q_PROPERTY(bool hasSecret READ hasSecret NOTIFY changed)         // an older app's Client Secret saved
    Q_PROPERTY(bool connected READ connected NOTIFY changed)         // signed in
    Q_PROPERTY(QString user READ user NOTIFY changed)
    Q_PROPERTY(QString clientId READ clientId NOTIFY changed)
    // While signing in: the code to enter, and where.
    Q_PROPERTY(QString userCode READ userCode NOTIFY changed)
    Q_PROPERTY(QString verifyUrl READ verifyUrl NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool sendWatching READ scrobbleOn WRITE setScrobbleOn NOTIFY changed)
    Q_PROPERTY(QString lastSync READ lastSync NOTIFY changed)

public:
    explicit TraktService(QObject *parent = nullptr);

    bool configured() const { return !m_clientId.isEmpty(); }
    bool hasSecret() const { return !m_secret.isEmpty(); }
    bool connected() const { return !m_access.isEmpty(); }
    QString user() const { return m_user; }
    QString clientId() const { return m_clientId; }
    QString userCode() const { return m_userCode; }
    QString verifyUrl() const { return m_verifyUrl; }
    QString status() const { return m_status; }
    bool scrobbleOn() const { return m_scrobble; }
    void setScrobbleOn(bool on);
    QString lastSync() const;

    // Saves the app's Client ID (and Client Secret, for an older app that
    // has one), and starts signing in. A new Client ID drops the old secret.
    Q_INVOKABLE void signIn(const QString &clientId, const QString &secret);
    Q_INVOKABLE void cancelSignIn();
    Q_INVOKABLE void signOut();
    // Fetches what's watched on Trakt (watchedFetched follows).
    Q_INVOKABLE void syncNow();

    // Playback of a movie or an episode (Player.vod), progress in percent:
    // "start", "pause" or "stop". Trakt counts it as watched when it stops
    // past 80 %.
    Q_INVOKABLE void scrobble(const QString &action, const QVariantMap &descriptor, double progress);
    // Marked (or unmarked) watched by hand in Relay.
    void markWatched(const QVariantMap &descriptor, bool watched);

    // Matching titles across services: folded title without provider tags.
    static QString titleKey(const QString &title);
    // Trakt's watched lists -> {"movies": [{title, year}], "episodes": [{title,
    // year, season, episode}]}.
    static QVariantMap parseWatched(const QJsonArray &movies, const QJsonArray &shows);

    static QString apiBase();   // RELAY_TRAKT_URL overrides (tests)

signals:
    void changed();
    void message(const QString &text);
    // What's watched on Trakt (see parseWatched()).
    void watchedFetched(const QVariantMap &watched);

private:
    QNetworkReply *call(const QByteArray &verb, const QString &path, const QJsonObject &body = {}, bool auth = true);
    // A sign-in request's body with the client_secret added, only when there is one.
    QJsonObject withSecret(QJsonObject body) const;
    // An authorised call; refreshes the sign-in once on 401.
    void api(const QByteArray &verb, const QString &path, const QJsonObject &body,
             std::function<void(int http, const QByteArray &)> done, bool retried = false);
    void poll();
    void tokensFrom(const QJsonObject &o);
    void refreshTokens(std::function<void(bool)> done);
    void fetchUser();
    void saveTokens();
    void clearTokens();
    void setStatus(const QString &s);
    // {"movie": {"ids": …}} / {"show": {"ids": …}, "episode": {season, number}}
    // for a descriptor; searches Trakt for the title once and remembers it.
    void identify(const QVariantMap &descriptor, std::function<void(const QJsonObject &)> done);

    QNetworkAccessManager *m_net;
    QString m_clientId, m_secret;
    QString m_access, m_refresh, m_user;
    qint64 m_expires = 0;
    QString m_deviceCode, m_userCode, m_verifyUrl, m_status;
    int m_interval = 5;
    qint64 m_codeExpires = 0;
    QTimer m_pollTimer, m_syncTimer;
    bool m_scrobble = true;
    QHash<QString, QJsonObject> m_ids;          // "movie|title|year" / "show|title|year" -> ids
    QString m_scrobbling;                       // key of what's being scrobbled now
};
