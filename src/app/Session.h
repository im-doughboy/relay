#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>

class AppSettings;
class ChannelModel;
class PlayerController;
class PlaylistService;
class EpgService;
class VodService;
class CatchupService;
class UserLibrary;
class ScheduleService;
struct PlaylistResult;

// Application-level coordinator: adds/reloads/removes the playlist, saves it
// only after it loads successfully, and hands channels to the model/player.
class Session : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    // e.g. "3,912 live channels · 98,839 movies and episodes hidden"
    Q_PROPERTY(QString summary READ summary NOTIFY summaryChanged)

public:
    Session(AppSettings *settings, PlaylistService *playlists, ChannelModel *channels,
            PlayerController *player, EpgService *epg, VodService *vod, CatchupService *catchup,
            QObject *parent = nullptr);

    QString lastError() const { return m_lastError; }
    bool ready() const { return m_ready; }
    QString summary() const { return m_summary; }

    // Each playlist keeps its own files (library.json, guide.sqlite) in
    // <data>/playlists/<id>/. Call before creating the library and guide.
    static QString storageDir(const QString &dataDir, const QString &id);
    static void prepareStorage(const QString &dataDir, const QString &activeId);
    void setStorage(UserLibrary *library, ScheduleService *schedule, const QString &dataDir);

    // Change the active playlist's source (saved once it loads).
    Q_INVOKABLE void addM3u(const QString &location);
    Q_INVOKABLE void addXtream(const QString &server, const QString &user,
                               const QString &password, const QString &output);
    // Add another playlist; it becomes the active one once it loads.
    Q_INVOKABLE void addNewM3u(const QString &name, const QString &location, const QString &guideUrl);
    Q_INVOKABLE void addNewXtream(const QString &name, const QString &server, const QString &user,
                                  const QString &password, const QString &output, const QString &guideUrl);
    Q_INVOKABLE void switchPlaylist(const QString &id);
    // Switch, then tune the channel with this key once it's loaded.
    Q_INVOKABLE void switchPlaylistAndTune(const QString &id, const QString &channelKey);
    Q_INVOKABLE void reload();          // re-download the saved playlist
    Q_INVOKABLE void removePlaylist();  // the active one
    Q_INVOKABLE void removePlaylistById(const QString &id);
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void refreshGuide();   // re-download the guide now
    // Automatic playlist refresh: true when the playlist was loaded longer
    // ago than the setting allows; reloadQuietly() then re-downloads it
    // without interrupting what's playing (and without error screens).
    Q_INVOKABLE bool playlistDue() const;
    Q_INVOKABLE void reloadQuietly();
    // Listings chosen by hand for a channel: a guide channel id, "-" for
    // none, "" for automatic; and minutes to move that channel's listings.
    Q_INVOKABLE QString guideChoice(int globalIndex) const;
    Q_INVOKABLE void setGuideChoice(int globalIndex, const QString &guideId);
    Q_INVOKABLE int guideOffset(int globalIndex) const;
    Q_INVOKABLE void setGuideOffset(int globalIndex, int minutes);
    // The user agent typed for a playlist that's being added ("" = none).
    Q_INVOKABLE void setPendingUserAgent(const QString &ua);
    Q_INVOKABLE void applyGuideSettings();   // after the guide settings change

signals:
    void lastErrorChanged();
    void readyChanged();
    void summaryChanged();

private:
    enum class Pending { None, M3u, Xtream };

    void onLoaded(const PlaylistResult &result);
    void onFailed(const QString &message);
    void setError(const QString &e);
    void applyGuideMapping();
    QStringList guideUrls() const;
    QSet<QString> chosenGuideIds() const;
    void refreshGuideIfNeeded(bool force);
    void switchStorage(const QString &id);   // empty state, other playlist's files

    AppSettings *m_settings;
    PlaylistService *m_playlists;
    ChannelModel *m_channels;
    PlayerController *m_player;
    EpgService *m_epg;
    VodService *m_vod;
    CatchupService *m_catchup;
    QString m_playlistGuideUrl;   // url-tvg / xmltv.php from the loaded playlist

    Pending m_pending = Pending::None;
    QString m_pendingA, m_pendingB, m_pendingC, m_pendingD;
    bool m_pendingNew = false;
    QString m_pendingName, m_pendingGuide, m_pendingTuneKey, m_pendingUserAgent;
    UserLibrary *m_library = nullptr;
    ScheduleService *m_schedule = nullptr;
    QString m_dataDir;
    QString m_lastError;
    bool m_ready = false;
    bool m_quiet = false;            // the load under way is an automatic refresh
    QDateTime m_loadedAt;
    QString m_summary;
};
