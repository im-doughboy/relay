#include "Session.h"

#include "app/AppSettings.h"
#include "app/UserLibrary.h"
#include "record/ScheduleService.h"
#include "epg/EpgService.h"
#include "models/ChannelModel.h"
#include "player/PlayerController.h"
#include "sources/PlaylistService.h"
#include "vod/VodService.h"
#include "sources/CatchupService.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QTimer>

#include <algorithm>

Session::Session(AppSettings *settings, PlaylistService *playlists, ChannelModel *channels,
                 PlayerController *player, EpgService *epg, VodService *vod, CatchupService *catchup,
                 QObject *parent)
    : QObject(parent), m_settings(settings), m_playlists(playlists),
      m_channels(channels), m_player(player), m_epg(epg), m_vod(vod), m_catchup(catchup)
{
    connect(m_playlists, &PlaylistService::loaded, this, &Session::onLoaded);
    connect(m_playlists, &PlaylistService::failed, this, &Session::onFailed);
    connect(m_epg, &EpgService::guideUpdated, this, &Session::applyGuideMapping);

    // Twice-daily guide refresh happens inside refresh(); just poll hourly.
    auto *timer = new QTimer(this);
    timer->setInterval(3600 * 1000);
    connect(timer, &QTimer::timeout, this, [this] {
        if (m_ready) refreshGuideIfNeeded(false);
    });
    timer->start();
    applyGuideSettings();
    connect(m_settings, &AppSettings::optionsChanged, this, &Session::applyGuideSettings);
    connect(m_settings, &AppSettings::sourceChanged, this, &Session::applyGuideSettings);
}

void Session::applyGuideSettings()
{
    m_epg->setRefreshHours(m_settings->guideRefreshHours());
    m_epg->setDaysAhead(m_settings->guideDaysAhead());
    m_epg->setOffsetMinutes(m_settings->guideOffsetMinutes());
}

QSet<QString> Session::chosenGuideIds() const
{
    QSet<QString> ids;
    if (!m_library) return ids;
    for (const QString &id : m_library->guideIds())
        if (!id.isEmpty() && id != QLatin1String("-")) ids.insert(id);
    return ids;
}

void Session::refreshGuideIfNeeded(bool force)
{
    m_epg->refresh(m_channels->originalChannels(), guideUrls(), force, chosenGuideIds());
}

QString Session::guideChoice(int globalIndex) const
{
    return m_library ? m_library->guideId(m_channels->keyAt(globalIndex)) : QString();
}

void Session::setGuideChoice(int globalIndex, const QString &guideId)
{
    const QString key = m_channels->keyAt(globalIndex);
    if (!m_library || key.isEmpty()) return;
    m_library->setGuideId(key, guideId);
    applyGuideMapping();
    // Listings for a guide channel that wasn't kept need another import.
    if (!guideId.isEmpty() && guideId != QLatin1String("-") && !m_epg->hasChannel(guideId))
        refreshGuideIfNeeded(false);
}

int Session::guideOffset(int globalIndex) const
{
    const QString key = m_channels->keyAt(globalIndex);
    if (m_library && m_library->guideOffsets().contains(key)) return m_library->guideOffset(key);
    const Channel *c = m_channels->channel(globalIndex);
    return c ? c->tvgShiftMinutes : 0;
}

void Session::setGuideOffset(int globalIndex, int minutes)
{
    const QString key = m_channels->keyAt(globalIndex);
    if (!m_library || key.isEmpty()) return;
    m_library->setGuideOffset(key, minutes);
    applyGuideMapping();
}

void Session::setPendingUserAgent(const QString &ua)
{
    m_pendingUserAgent = ua.trimmed();
    m_settings->setTransientUserAgent(m_pendingUserAgent);
}

bool Session::playlistDue() const
{
    const int hours = m_settings->playlistRefreshHours();
    return hours > 0 && m_ready && !m_playlists->busy() && m_loadedAt.isValid()
        && m_loadedAt.secsTo(QDateTime::currentDateTimeUtc()) >= qint64(hours) * 3600;
}

void Session::reloadQuietly()
{
    if (!m_ready || m_playlists->busy() || m_pending != Pending::None) return;
    qInfo() << "Playlist: automatic refresh";
    m_loadedAt = QDateTime::currentDateTimeUtc();   // a failed try waits for the next interval
    m_quiet = true;
    if (m_settings->sourceType() == QLatin1String("m3u"))
        m_playlists->loadM3u(m_settings->m3uLocation());
    else if (m_settings->sourceType() == QLatin1String("xtream"))
        m_playlists->loadXtream(m_settings->xtreamServer(), m_settings->xtreamUser(),
                                m_settings->xtreamPassword(), m_settings->xtreamOutput());
    else m_quiet = false;
}

// The addresses typed in Settings (several allowed), then the playlist's own
// when there are none or "also use the playlist's guide" is on.
QStringList Session::guideUrls() const
{
    QStringList urls = AppSettings::splitAddresses(m_settings->epgOverride());
    if (urls.isEmpty() || m_settings->guideKeepOwn())
        for (const QString &u : AppSettings::splitAddresses(m_playlistGuideUrl))
            if (!urls.contains(u)) urls << u;
    return urls;
}

void Session::applyGuideMapping()
{
    // Listings chosen by hand and per-channel time offsets, by position.
    QHash<int, QString> chosen;
    QHash<int, int> offsets;
    if (m_library) {
        for (auto it = m_library->guideIds().constBegin(); it != m_library->guideIds().constEnd(); ++it) {
            const int i = m_channels->indexOfKey(it.key());
            if (i >= 0) chosen.insert(i, it.value());
        }
        for (auto it = m_library->guideOffsets().constBegin(); it != m_library->guideOffsets().constEnd(); ++it) {
            const int i = m_channels->indexOfKey(it.key());
            if (i >= 0) offsets.insert(i, it.value());
        }
    }
    const QVector<QString> ids = m_epg->resolve(m_channels->originalChannels(), chosen, offsets);
    m_channels->setEpgIds(ids);
    m_player->refreshProgram();
    if (m_epg->hasGuide()) {
        const int matched = int(std::count_if(ids.cbegin(), ids.cend(),
                                              [](const QString &id) { return !id.isEmpty(); }));
        qInfo().noquote() << QStringLiteral("Guide: matched %1 of %2 channels")
                                 .arg(matched).arg(ids.size());
    }
}

void Session::refreshGuide()
{
    if (!m_ready) return;
    refreshGuideIfNeeded(true);
}

QString Session::storageDir(const QString &dataDir, const QString &id)
{
    return dataDir + QStringLiteral("/playlists/") + (id.isEmpty() ? QStringLiteral("new") : id);
}

void Session::prepareStorage(const QString &dataDir, const QString &activeId)
{
    const QString dir = storageDir(dataDir, activeId);
    QDir().mkpath(dir);
    // Before multiple playlists, the files sat in the data folder itself.
    if (activeId.isEmpty() || QFile::exists(dir + QStringLiteral("/library.json"))) return;
    bool moved = false;
    for (const char *name : {"library.json", "guide.sqlite", "guide.sqlite-wal", "guide.sqlite-shm"}) {
        const QString from = dataDir + QLatin1Char('/') + QLatin1String(name);
        if (QFile::exists(from) && QFile::rename(from, dir + QLatin1Char('/') + QLatin1String(name))) moved = true;
    }
    if (moved) qInfo().noquote() << "Playlists: moved this playlist's favorites and guide into" << QDir::toNativeSeparators(dir);
}

void Session::setStorage(UserLibrary *library, ScheduleService *schedule, const QString &dataDir)
{
    m_library = library;
    m_schedule = schedule;
    m_dataDir = dataDir;
}

void Session::switchStorage(const QString &id)
{
    m_playlists->cancel();
    m_epg->cancel();
    m_player->stopVod();
    m_player->reset();
    m_vod->clear();
    m_catchup->clear();
    m_channels->setPlaylist(PlaylistResult{});
    m_playlistGuideUrl.clear();
    m_summary.clear();
    emit summaryChanged();
    if (m_ready) { m_ready = false; emit readyChanged(); }
    m_settings->setActive(id);
    const QString dir = storageDir(m_dataDir, id);
    QDir().mkpath(dir);
    if (m_library) m_library->setPath(dir + QStringLiteral("/library.json"));
    m_epg->setDatabase(dir + QStringLiteral("/guide.sqlite"));
    if (m_schedule) m_schedule->setPlaylist(id);
    qInfo().noquote() << "Playlists: now using" << m_settings->playlistName(id);
}

void Session::switchPlaylist(const QString &id)
{
    if (!m_settings->playlistIds().contains(id)) return;
    if (id == m_settings->activeId() && (m_ready || m_playlists->busy())) return;
    clearError();
    m_pending = Pending::None;
    m_pendingNew = false;
    switchStorage(id);
    reload();
}

void Session::switchPlaylistAndTune(const QString &id, const QString &channelKey)
{
    if (id == m_settings->activeId() && m_ready) {
        const int idx = m_channels->indexOfKey(channelKey);
        if (idx >= 0) m_player->tune(idx);
        return;
    }
    switchPlaylist(id);
    m_pendingTuneKey = channelKey;
}

void Session::addNewM3u(const QString &name, const QString &location, const QString &guideUrl)
{
    clearError();
    m_pending = Pending::M3u;
    m_pendingNew = true;
    m_pendingName = name.trimmed().isEmpty() ? AppSettings::suggestName(location) : name.trimmed();
    m_pendingGuide = guideUrl.trimmed();
    m_pendingA = location;
    m_playlists->loadM3u(location);
}

void Session::addNewXtream(const QString &name, const QString &server, const QString &user,
                           const QString &password, const QString &output, const QString &guideUrl)
{
    clearError();
    m_pending = Pending::Xtream;
    m_pendingNew = true;
    m_pendingName = name.trimmed().isEmpty() ? AppSettings::suggestName(server) : name.trimmed();
    m_pendingGuide = guideUrl.trimmed();
    m_pendingA = server;
    m_pendingB = user;
    m_pendingC = password;
    m_pendingD = output;
    m_playlists->loadXtream(server, user, password, output);
}

void Session::addM3u(const QString &location)
{
    clearError();
    if (m_settings->activeId().isEmpty()) { addNewM3u(QString(), location, m_settings->epgOverride()); return; }
    m_pendingNew = false;
    m_pending = Pending::M3u;
    m_pendingA = location;
    m_playlists->loadM3u(location);
}

void Session::addXtream(const QString &server, const QString &user,
                        const QString &password, const QString &output)
{
    clearError();
    if (m_settings->activeId().isEmpty()) { addNewXtream(QString(), server, user, password, output, QString()); return; }
    m_pendingNew = false;
    m_pending = Pending::Xtream;
    m_pendingA = server;
    m_pendingB = user;
    // An empty password on the settings screen means "keep the saved one".
    const bool keepSaved = password.isEmpty()
        && m_settings->sourceType() == QLatin1String("xtream");
    m_pendingC = keepSaved ? m_settings->xtreamPassword() : password;
    m_pendingD = output;
    m_playlists->loadXtream(server, user, m_pendingC, output);
}

void Session::reload()
{
    clearError();
    m_pending = Pending::None;
    if (m_settings->sourceType() == QLatin1String("m3u"))
        m_playlists->loadM3u(m_settings->m3uLocation());
    else if (m_settings->sourceType() == QLatin1String("xtream"))
        m_playlists->loadXtream(m_settings->xtreamServer(), m_settings->xtreamUser(),
                                m_settings->xtreamPassword(), m_settings->xtreamOutput());
}

void Session::removePlaylist()
{
    removePlaylistById(m_settings->activeId());
}

void Session::removePlaylistById(const QString &id)
{
    if (id.isEmpty()) return;
    const bool active = id == m_settings->activeId();
    if (active) {
        m_playlists->cancel();
        m_epg->cancel();
        m_channels->setPlaylist(PlaylistResult{});
        m_playlistGuideUrl.clear();
        m_summary.clear();
        emit summaryChanged();
        m_player->stopVod();
        m_player->reset();
        m_vod->clear();
        m_catchup->clear();
        if (m_ready) { m_ready = false; emit readyChanged(); }
        // Let go of the files before deleting them.
        const QString temp = storageDir(m_dataDir, QString());
        QDir().mkpath(temp);
        if (m_library) m_library->setPath(temp + QStringLiteral("/library.json"));
        m_epg->setDatabase(temp + QStringLiteral("/guide.sqlite"));
    }
    m_settings->removePlaylist(id);
    QDir(storageDir(m_dataDir, id)).removeRecursively();
    qInfo().noquote() << "Playlists: removed" << id;
    // Carry on with another playlist, if there is one.
    if (active && !m_settings->playlistIds().isEmpty()) switchPlaylist(m_settings->playlistIds().first());
}

void Session::clearError() { setError(QString()); }

void Session::setError(const QString &e)
{
    if (m_lastError == e) return;
    m_lastError = e;
    emit lastErrorChanged();
}

void Session::onLoaded(const PlaylistResult &result)
{
    m_loadedAt = QDateTime::currentDateTimeUtc();
    if (m_quiet) {
        // Automatic refresh: swap the channels in underneath what's playing.
        m_quiet = false;
        const QString current = m_channels->keyAt(m_player->currentIndex());
        const QString previous = m_channels->keyAt(m_player->previousIndex());
        const Channel *was = m_channels->channel(m_player->currentIndex());
        const QString url = was ? was->url : QString();
        const QString group = m_channels->groupName(m_channels->currentGroup());
        m_channels->setPlaylist(result);
        const int g = m_channels->groupIndex(group);
        if (g >= 0) m_channels->setCurrentGroup(g);
        m_catchup->setSource(result);
        m_playlistGuideUrl = result.epgUrl;
        applyGuideMapping();
        m_player->rebindAfterReload(current, previous, url);
        refreshGuideIfNeeded(result.guideIsNew);
        qInfo().noquote() << QStringLiteral("Playlist: refreshed, %1 channels").arg(result.channels.size());
        return;
    }
    // Only remember a new playlist once it has actually worked.
    if (m_pendingNew && m_pending != Pending::None) {
        const QString id = m_settings->createPlaylist(m_pendingName);
        switchStorage(id);
        if (!m_pendingGuide.isEmpty()) m_settings->setEpgOverride(m_pendingGuide);
        m_settings->setUserAgent(m_pendingUserAgent);
        m_pendingUserAgent.clear();
        m_pendingNew = false;
    }
    if (m_pending == Pending::M3u) m_settings->saveM3u(m_pendingA);
    else if (m_pending == Pending::Xtream)
        m_settings->saveXtream(m_pendingA, m_pendingB, m_pendingC, m_pendingD);
    m_pending = Pending::None;
    m_pendingC.clear();

    m_channels->setPlaylist(result);
    m_catchup->setSource(result);
    m_playlistGuideUrl = result.epgUrl;

    const QLocale locale;
    m_summary = tr("%1 live channels").arg(locale.toString(result.channels.size()));
    if (!result.xtreamServer.isEmpty())
        m_summary += tr(" · movies and series from the server");
    else if (result.skippedMovies + result.skippedSeries > 0)
        m_summary += tr(" · %1 movies and %2 episodes")
                         .arg(locale.toString(result.skippedMovies), locale.toString(result.skippedSeries));
    emit summaryChanged();
    applyGuideMapping();   // the stored guide shows up instantly
    if (!m_ready) { m_ready = true; emit readyChanged(); }
    // A channel asked for while switching (a reminder or multi-view tile
    // from this playlist) opens instead of the last one.
    if (!m_pendingTuneKey.isEmpty() && m_channels->indexOfKey(m_pendingTuneKey) >= 0)
        m_settings->setLastChannelUrl(m_pendingTuneKey);
    m_pendingTuneKey.clear();
    m_player->restoreAfterLoad();
    m_vod->setSource(result);
    refreshGuideIfNeeded(result.guideIsNew);
}

void Session::onFailed(const QString &message)
{
    if (m_quiet) {      // an automatic refresh that didn't work: carry on with what we have
        m_quiet = false;
        qWarning().noquote() << "Playlist: automatic refresh failed:" << message;
        return;
    }
    m_pending = Pending::None;
    m_pendingNew = false;
    m_pendingTuneKey.clear();
    m_pendingC.clear();
    setError(message);
}
