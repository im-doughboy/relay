#pragma once

#include <QHash>
#include <QObject>
#include <QVariantMap>
#include <QSettings>
#include <QStringList>
#include <QVariantList>

// Settings, including the list of playlists. One playlist is active; the
// source, guide address, last channel and multi-view tiles below are the
// active playlist's. Logins (the playlist link, the Xtream password) live in
// the system credential store where there is one (see SecretStore).
class AppSettings : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasSource READ hasSource NOTIFY sourceChanged)
    Q_PROPERTY(QString sourceType READ sourceType NOTIFY sourceChanged)
    Q_PROPERTY(QString m3uLocation READ m3uLocation NOTIFY sourceChanged)
    Q_PROPERTY(QString xtreamServer READ xtreamServer NOTIFY sourceChanged)
    Q_PROPERTY(QString xtreamUser READ xtreamUser NOTIFY sourceChanged)
    Q_PROPERTY(QString xtreamOutput READ xtreamOutput NOTIFY sourceChanged)
    Q_PROPERTY(QString epgOverride READ epgOverride WRITE setEpgOverride NOTIFY sourceChanged)
    // Pause live TV: disk space for the live buffer in MB (0 = off).
    Q_PROPERTY(int timeshiftMb READ timeshiftMb WRITE setTimeshiftMb NOTIFY timeshiftMbChanged)
    // Channel keys of the multi-view tiles, remembered for next time.
    Q_PROPERTY(QStringList multiviewKeys READ multiviewKeys WRITE setMultiviewKeys NOTIFY multiviewKeysChanged)
    // Multi-view layout: "auto", "stack", "row", "grid" or "focus".
    Q_PROPERTY(QString multiviewLayout READ multiviewLayout WRITE setMultiviewLayout NOTIFY multiviewLayoutChanged)
    // Picture-in-picture corner: 0 top right, 1 bottom right, 2 bottom left, 3 top left.
    Q_PROPERTY(int pipCorner READ pipCorner WRITE setPipCorner NOTIFY pipCornerChanged)
    // Picture and sound (global). pictureShape: "auto", "16:9", "4:3", "fill"
    // (crop to fill the window) or "stretch"; deinterlace: "auto", "on", "off".
    Q_PROPERTY(QString pictureShape READ pictureShape WRITE setPictureShape NOTIFY optionsChanged)
    Q_PROPERTY(QString deinterlace READ deinterlace WRITE setDeinterlace NOTIFY optionsChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY optionsChanged)
    Q_PROPERTY(int audioDelayMs READ audioDelayMs WRITE setAudioDelayMs NOTIFY optionsChanged)
    // Guide and playlist upkeep (global): hours between updates (playlist: 0 =
    // only when Relay starts) and how many days of listings to keep ahead.
    Q_PROPERTY(int guideRefreshHours READ guideRefreshHours WRITE setGuideRefreshHours NOTIFY optionsChanged)
    Q_PROPERTY(int guideDaysAhead READ guideDaysAhead WRITE setGuideDaysAhead NOTIFY optionsChanged)
    Q_PROPERTY(int playlistRefreshHours READ playlistRefreshHours WRITE setPlaylistRefreshHours NOTIFY optionsChanged)
    // Interface size in percent (read before the window is made: needs a restart).
    Q_PROPERTY(int uiScale READ uiScale WRITE setUiScale NOTIFY optionsChanged)
    // Another player's program file ("" = none), for "Open in external player".
    Q_PROPERTY(QString externalPlayer READ externalPlayer WRITE setExternalPlayer NOTIFY optionsChanged)
    // Keys the user changed: {action id: Qt key code}.
    Q_PROPERTY(QVariantMap keys READ keys NOTIFY keysChanged)
    // The user's TMDB key (posters and details); in the credential store where there is one.
    Q_PROPERTY(QString tmdbKey READ tmdbKey NOTIFY tmdbKeyChanged)
    // The active playlist's: what Relay calls itself to the provider (empty =
    // the default), and minutes to move every guide listing by.
    Q_PROPERTY(QString userAgent READ userAgent WRITE setUserAgent NOTIFY sourceChanged)
    Q_PROPERTY(int guideOffsetMinutes READ guideOffsetMinutes WRITE setGuideOffsetMinutes NOTIFY sourceChanged)
    // [{id, name, type, detail, active}] in the user's order.
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    Q_PROPERTY(QString activeId READ activeId NOTIFY sourceChanged)
    Q_PROPERTY(QString activeName READ activeName NOTIFY playlistsChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);

    // ---- the active playlist ----
    bool hasSource() const;
    QString sourceType() const;
    QString m3uLocation() const;
    QString xtreamServer() const;
    QString xtreamUser() const;
    QString xtreamPassword() const;
    QString xtreamOutput() const;
    // Guide (XMLTV) address the user typed; empty = use the playlist's own.
    QString epgOverride() const;
    void setEpgOverride(const QString &url);
    QStringList multiviewKeys() const;
    QString multiviewLayout() const;
    void setMultiviewLayout(const QString &layout);
    int pipCorner() const;
    void setPipCorner(int corner);
    void setMultiviewKeys(const QStringList &keys);
    QString lastChannelUrl() const;              // a channel key (older versions: an address)
    void setLastChannelUrl(const QString &key);

    // Saves into the active playlist (creating one if there's none).
    Q_INVOKABLE void saveM3u(const QString &location);
    Q_INVOKABLE void saveXtream(const QString &server, const QString &user,
                                const QString &password, const QString &output);
    Q_INVOKABLE void clearSource();              // removes the active playlist

    // Any playlist's source (for loading a playlist that isn't the active one).
    struct Source { QString type, m3u, server, user, password, output; };
    Source source(const QString &id) const;

    // ---- the list ----
    QVariantList playlists() const;
    QStringList playlistIds() const;
    QString activeId() const { return m_active; }
    QString activeName() const { return playlistName(m_active); }
    QString playlistName(const QString &id) const;
    QString createPlaylist(const QString &name);  // returns its id; not made active
    void setActive(const QString &id);
    void removePlaylist(const QString &id);
    Q_INVOKABLE void renamePlaylist(const QString &id, const QString &name);
    Q_INVOKABLE void movePlaylist(const QString &id, int delta);
    // A name from the address, e.g. "provider.example".
    static QString suggestName(const QString &location);

    // ---- global ----
    int timeshiftMb() const;
    void setTimeshiftMb(int mb);
    QString pictureShape() const;
    void setPictureShape(QString v);
    QString deinterlace() const;
    void setDeinterlace(QString v);
    bool hardwareDecoding() const;
    void setHardwareDecoding(bool v);
    int audioDelayMs() const;
    void setAudioDelayMs(int v);
    int guideRefreshHours() const;
    void setGuideRefreshHours(int v);
    int guideDaysAhead() const;
    void setGuideDaysAhead(int v);
    int playlistRefreshHours() const;
    int uiScale() const;
    void setUiScale(int percent);
    QString externalPlayer() const;
    void setExternalPlayer(QString path);
    QVariantMap keys() const;
    Q_INVOKABLE void setKey(const QString &action, int key);   // 0 = back to the usual key
    Q_INVOKABLE void resetKeys();
    QString tmdbKey() const;
    Q_INVOKABLE void setTmdbKey(const QString &key);   // "" removes it
    void setPlaylistRefreshHours(int v);
    QString userAgent() const { return userAgentFor(m_active); }
    QString userAgentFor(const QString &id) const;
    void setUserAgent(const QString &v);
    int guideOffsetMinutes() const;
    void setGuideOffsetMinutes(int v);
    // Guide addresses the user typed (epgOverride split on new lines, commas
    // and semicolons).
    static QStringList splitAddresses(const QString &text);
    // What Relay's own requests (playlist, guide, movies, recordings) call
    // themselves: the active playlist's user agent, or "Relay/<version>".
    static QString effectiveUserAgent();
    // For a playlist that's being added and isn't saved yet ("" = back to
    // the active playlist's).
    Q_INVOKABLE void setTransientUserAgent(const QString &ua);
    // Also use the playlist's own guide when guide addresses are typed in.
    Q_PROPERTY(bool guideKeepOwn READ guideKeepOwn WRITE setGuideKeepOwn NOTIFY sourceChanged)
    bool guideKeepOwn() const;
    void setGuideKeepOwn(bool v);
    QSettings &store() { return m_s; }   // backup / restore

signals:
    void timeshiftMbChanged();
    void optionsChanged();
    void keysChanged();
    void tmdbKeyChanged();
    void multiviewKeysChanged();
    void multiviewLayoutChanged();
    void pipCornerChanged();
    void sourceChanged();
    void playlistsChanged();

private:
    QString key(const QString &id, const char *name) const;
    QString key(const char *name) const { return key(m_active, name); }
    QString secret(const char *plainKey, const char *flag, const char *name) const;
    QString secretFor(const QString &id, const char *plainKey, const char *flag, const char *name) const;
    void setSecret(const char *plainKey, const char *flag, const char *name, const QString &value);
    void removeSecrets(const QString &id);
    QString secretName(const QString &id, const char *name) const;
    void syncUserAgent();
    void migrateSinglePlaylist();
    void moveSecretsToStore();
    QString ensureActive();

    QSettings m_s;
    QString m_active;
    mutable QHash<QString, QString> m_secretCache;
};
