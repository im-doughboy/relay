#include "ScheduleService.h"

#include "Recorder.h"
#include "sources/PortalClient.h"
#include "epg/EpgService.h"
#include "models/ChannelModel.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <QtDebug>

#include <algorithm>

namespace {
constexpr qint64 kReminderLead = 60;     // seconds before the show
constexpr qint64 kRecordBefore = 60;
constexpr qint64 kRecordAfter = 120;

qint64 nowSecs() { return QDateTime::currentSecsSinceEpoch(); }
}

ScheduleService::ScheduleService(ChannelModel *channels, EpgService *epg, const QString &dataDir,
                                 const QString &defaultFolder, const QString &playlistId, QObject *parent)
    : QObject(parent), m_channels(channels), m_epg(epg),
      m_path(dataDir + QStringLiteral("/schedule.json")), m_folder(defaultFolder),
      m_net(new QNetworkAccessManager(this)), m_playlist(playlistId)
{
    load();
    // New guide listings: schedule what the series rules ask for (after the
    // channels have been given their guide ids).
    if (m_epg)
        connect(m_epg, &EpgService::guideUpdated, this, [this] { QTimer::singleShot(1500, this, &ScheduleService::applySeries); });
    m_timer.setInterval(5000);
    connect(&m_timer, &QTimer::timeout, this, &ScheduleService::tick);
    m_timer.start();
}

ScheduleService::~ScheduleService()
{
    shutdown();
}

void ScheduleService::setFolder(const QString &folder)
{
    QString f = folder;
    if (f.startsWith(QLatin1String("file:"))) f = QUrl(f).toLocalFile();
    if (f.isEmpty() || f == m_folder) return;
    m_folder = f;
    save();
    emit folderChanged();
}

void ScheduleService::setPlaylist(const QString &id)
{
    if (id == m_playlist) return;
    m_playlist = id;
    m_lastApply = 0;
    ++m_revision;
    emit changed();
}

void ScheduleService::touch()
{
    ++m_revision;
    save();
    emit changed();
}

// ---- persistence ----------------------------------------------------------------------

void ScheduleService::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QString folder = root.value(QLatin1String("folder")).toString();
    if (!folder.isEmpty()) m_folder = folder;
    for (const QJsonValue &v : root.value(QLatin1String("items")).toArray()) {
        const QJsonObject o = v.toObject();
        Item it;
        it.id = o.value(QLatin1String("id")).toString();
        it.type = o.value(QLatin1String("type")).toString();
        it.status = o.value(QLatin1String("status")).toString();
        it.playlistId = o.value(QLatin1String("playlistId")).toString();
        if (it.playlistId.isEmpty()) it.playlistId = m_playlist;   // from before multiple playlists
        it.channelKey = o.value(QLatin1String("channelKey")).toString();
        it.channelName = o.value(QLatin1String("channelName")).toString();
        it.url = o.value(QLatin1String("url")).toString();
        it.userAgent = o.value(QLatin1String("userAgent")).toString();
        it.referrer = o.value(QLatin1String("referrer")).toString();
        it.title = o.value(QLatin1String("title")).toString();
        it.file = o.value(QLatin1String("file")).toString();
        it.error = o.value(QLatin1String("error")).toString();
        it.start = qint64(o.value(QLatin1String("start")).toDouble());
        it.stop = qint64(o.value(QLatin1String("stop")).toDouble());
        it.bytes = qint64(o.value(QLatin1String("bytes")).toDouble());
        it.seriesId = o.value(QLatin1String("seriesId")).toString();
        it.episodeKey = o.value(QLatin1String("episodeKey")).toString();
        if (it.id.isEmpty()) continue;
        // Relay was closed while recording: keep what was saved.
        if (it.status == QLatin1String("recording")) {
            const qint64 size = QFileInfo(it.file).size();
            it.bytes = size;
            it.status = size > 0 ? QStringLiteral("done") : QStringLiteral("failed");
            if (size > 0) it.error = tr("Stopped early because Relay was closed.");
        }
        // A recording whose file was deleted outside Relay.
        if (it.type == QLatin1String("recording") && it.status == QLatin1String("done")
            && !QFileInfo::exists(it.file))
            continue;
        m_items.append(it);
    }
    for (const QJsonValue &v : root.value(QLatin1String("series")).toArray()) {
        const QJsonObject o = v.toObject();
        Series sr;
        sr.id = o.value(QLatin1String("id")).toString();
        sr.playlistId = o.value(QLatin1String("playlistId")).toString();
        sr.channelKey = o.value(QLatin1String("channelKey")).toString();
        sr.channelName = o.value(QLatin1String("channelName")).toString();
        sr.title = o.value(QLatin1String("title")).toString();
        for (const QJsonValue &e : o.value(QLatin1String("seen")).toArray()) sr.seen << e.toString();
        for (const QJsonValue &e : o.value(QLatin1String("skipped")).toArray()) sr.skipped << qint64(e.toDouble());
        if (!sr.id.isEmpty() && !sr.title.isEmpty()) m_series.append(sr);
    }
}

void ScheduleService::save()
{
    QJsonArray arr;
    for (const Item &it : std::as_const(m_items)) {
        arr.append(QJsonObject{
            {"id", it.id}, {"type", it.type}, {"status", it.status}, {"playlistId", it.playlistId},
            {"channelKey", it.channelKey}, {"channelName", it.channelName},
            // No stream address: it usually contains the login. The channel is
            // looked up by its key when the recording starts.
            {"userAgent", it.userAgent}, {"referrer", it.referrer},
            {"title", it.title}, {"file", it.file}, {"error", it.error},
            {"start", double(it.start)}, {"stop", double(it.stop)}, {"bytes", double(it.bytes)},
            {"seriesId", it.seriesId}, {"episodeKey", it.episodeKey},
        });
    }
    QJsonArray series;
    const qint64 old = nowSecs() - 3 * 86400;
    for (const Series &sr : std::as_const(m_series)) {
        QJsonArray skipped;
        for (qint64 t : sr.skipped)
            if (t > old) skipped.append(double(t));
        series.append(QJsonObject{
            {"id", sr.id}, {"playlistId", sr.playlistId}, {"channelKey", sr.channelKey},
            {"channelName", sr.channelName}, {"title", sr.title},
            {"seen", QJsonArray::fromStringList(sr.seen.mid(qMax(0, int(sr.seen.size()) - 500)))},
            {"skipped", skipped},
        });
    }
    QSaveFile f(m_path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject{{"version", 1}, {"folder", m_folder}, {"items", arr}, {"series", series}})
                    .toJson(QJsonDocument::Indented));
        f.commit();
    }
}

// ---- queries ------------------------------------------------------------------------------

QVariantMap ScheduleService::toMap(const Item &it) const
{
    return {
        {"id", it.id}, {"type", it.type}, {"status", it.status},
        {"channelKey", it.channelKey}, {"channelName", it.channelName},
        {"playlistId", it.playlistId},
        {"globalIndex", it.playlistId == m_playlist ? m_channels->indexOfKey(it.channelKey) : -1},
        {"title", it.title}, {"file", it.file}, {"error", it.error},
        {"start", double(it.start) * 1000.0}, {"stop", double(it.stop) * 1000.0},
        {"bytes", double(m_recorders.contains(it.id) ? m_recorders.value(it.id)->bytes() : it.bytes)},
        {"series", !it.seriesId.isEmpty()},
        {"retrying", m_recorders.contains(it.id)
                         && m_recorders.value(it.id)->state() == Recorder::State::Retrying},
    };
}

QVariantList ScheduleService::items() const
{
    QVector<const Item *> sorted;
    for (const Item &it : m_items) sorted.append(&it);
    std::sort(sorted.begin(), sorted.end(), [](const Item *a, const Item *b) { return a->start > b->start; });
    QVariantList out;
    for (const Item *it : sorted) out.append(toMap(*it));
    return out;
}

int ScheduleService::recordingCount() const
{
    return int(m_recorders.size());
}

QVariantMap ScheduleService::item(const QString &id) const
{
    for (const Item &it : m_items)
        if (it.id == id) return toMap(it);
    return {};
}

ScheduleService::Item *ScheduleService::find(const QString &id)
{
    for (Item &it : m_items)
        if (it.id == id) return &it;
    return nullptr;
}

QString ScheduleService::idFor(int globalIndex, double startMs) const
{
    const QString key = m_channels->keyAt(globalIndex);
    const qint64 start = qint64(startMs / 1000);
    for (const Item &it : m_items)
        if (it.playlistId == m_playlist && it.channelKey == key && it.start == start
            && (it.status == QLatin1String("scheduled") || it.status == QLatin1String("recording")))
            return it.id;
    return QString();
}

QString ScheduleService::markFor(int globalIndex, double startMs) const
{
    const QString key = m_channels->keyAt(globalIndex);
    if (key.isEmpty()) return QString();
    const qint64 start = qint64(startMs / 1000);
    for (const Item &it : m_items) {
        if (it.playlistId != m_playlist || it.channelKey != key || it.start != start) continue;
        if (it.status == QLatin1String("scheduled") || it.status == QLatin1String("recording")) return it.type;
    }
    return QString();
}

// ---- adding -------------------------------------------------------------------------------

QString ScheduleService::add(const QString &type, int globalIndex, qint64 start, qint64 stop, const QString &title)
{
    const Channel *c = m_channels->channel(globalIndex);
    if (!c || stop <= start) return QString();
    const QString key = m_channels->keyAt(globalIndex);
    for (const Item &it : std::as_const(m_items))   // already there
        if (it.type == type && it.playlistId == m_playlist && it.channelKey == key && it.start == start
            && (it.status == QLatin1String("scheduled") || it.status == QLatin1String("recording")))
            return it.id;
    Item it;
    it.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    it.type = type;
    it.status = QStringLiteral("scheduled");
    it.playlistId = m_playlist;
    it.channelKey = key;
    it.channelName = c->name;
    it.url = c->url;
    it.userAgent = c->userAgent;
    it.referrer = c->referrer;
    it.title = title.isEmpty() ? c->name : title;
    it.start = start;
    it.stop = stop;
    m_items.append(it);
    qInfo().noquote() << QStringLiteral("Schedule: %1 \"%2\" on %3 at %4")
                             .arg(type, it.title, it.channelName,
                                  QDateTime::fromSecsSinceEpoch(start).toString(QStringLiteral("ddd HH:mm")));
    touch();
    tick();   // a show that's already on starts recording at once
    return it.id;
}

QString ScheduleService::addReminder(int globalIndex, const QVariantMap &p)
{
    const qint64 start = qint64(p.value(QStringLiteral("start")).toDouble() / 1000);
    const qint64 stop = qint64(p.value(QStringLiteral("stop")).toDouble() / 1000);
    if (start <= nowSecs()) return QString();   // only for shows that haven't started
    return add(QStringLiteral("reminder"), globalIndex, start, stop, p.value(QStringLiteral("title")).toString());
}

QString ScheduleService::addRecording(int globalIndex, const QVariantMap &p)
{
    const qint64 start = qint64(p.value(QStringLiteral("start")).toDouble() / 1000);
    const qint64 stop = qint64(p.value(QStringLiteral("stop")).toDouble() / 1000);
    if (stop <= nowSecs()) return QString();    // already over: that's catch-up, not recording
    return add(QStringLiteral("recording"), globalIndex, start, stop, p.value(QStringLiteral("title")).toString());
}

QString ScheduleService::recordNow(int globalIndex)
{
    const Channel *c = m_channels->channel(globalIndex);
    if (!c) return QString();
    // Until the end of the show that's on, with its title; otherwise an hour.
    if (m_epg && !c->epgId.isEmpty()) {
        const auto nn = m_epg->nowNext(c->epgId);
        if (nn.hasNow && nn.now.stop > nowSecs())
            return add(QStringLiteral("recording"), globalIndex, nn.now.start, nn.now.stop, nn.now.title);
    }
    const qint64 now = nowSecs();
    return add(QStringLiteral("recording"), globalIndex, now, now + 3600, QString());
}

// ---- record every episode -------------------------------------------------------------

ScheduleService::Series *ScheduleService::findSeries(const QString &id)
{
    for (Series &sr : m_series)
        if (sr.id == id) return &sr;
    return nullptr;
}

QString ScheduleService::seriesFor(int globalIndex, const QString &title) const
{
    const QString key = m_channels->keyAt(globalIndex);
    const QString folded = ChannelModel::fold(title);
    if (key.isEmpty() || folded.isEmpty()) return QString();
    for (const Series &sr : m_series)
        if (sr.playlistId == m_playlist && sr.channelKey == key && ChannelModel::fold(sr.title) == folded)
            return sr.id;
    return QString();
}

QString ScheduleService::addSeries(int globalIndex, const QVariantMap &p)
{
    const Channel *c = m_channels->channel(globalIndex);
    const QString title = p.value(QStringLiteral("title")).toString().trimmed();
    if (!c || title.isEmpty()) return QString();
    const QString existing = seriesFor(globalIndex, title);
    if (!existing.isEmpty()) return existing;
    Series sr;
    sr.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    sr.playlistId = m_playlist;
    sr.channelKey = m_channels->keyAt(globalIndex);
    sr.channelName = c->name;
    sr.title = title;
    m_series.append(sr);
    qInfo().noquote() << QStringLiteral("Schedule: every episode of \"%1\" on %2").arg(title, c->name);
    applySeries();
    touch();
    return sr.id;
}

void ScheduleService::removeSeries(const QString &id)
{
    for (int i = 0; i < m_series.size(); ++i) {
        if (m_series[i].id != id) continue;
        m_series.removeAt(i);
        for (int k = m_items.size() - 1; k >= 0; --k)
            if (m_items[k].seriesId == id && m_items[k].status == QLatin1String("scheduled")) m_items.removeAt(k);
        touch();
        return;
    }
}

QVariantList ScheduleService::series() const
{
    QVariantList out;
    for (const Series &sr : m_series) {
        int upcoming = 0;
        qint64 next = 0;
        for (const Item &it : m_items) {
            if (it.seriesId != sr.id || it.status != QLatin1String("scheduled")) continue;
            ++upcoming;
            if (next == 0 || it.start < next) next = it.start;
        }
        out.append(QVariantMap{
            {"id", sr.id}, {"title", sr.title}, {"channelName", sr.channelName},
            {"playlistId", sr.playlistId}, {"upcoming", upcoming}, {"next", double(next) * 1000.0},
        });
    }
    return out;
}

void ScheduleService::applySeries()
{
    m_lastApply = nowSecs();
    if (!m_epg || m_series.isEmpty()) return;
    if (m_channels->channels().isEmpty()) { m_lastApply -= 3570; return; }   // still loading: try again shortly
    const qint64 now = nowSecs();
    bool added = false;
    for (Series &sr : m_series) {
        if (sr.playlistId != m_playlist) continue;
        const int idx = m_channels->indexOfKey(sr.channelKey);
        const Channel *c = m_channels->channel(idx);
        if (!c || c->epgId.isEmpty()) continue;
        const QString folded = ChannelModel::fold(sr.title);
        // Episodes this rule already has (recorded, recording or scheduled).
        QSet<QString> have(sr.seen.cbegin(), sr.seen.cend());
        for (const Item &it : std::as_const(m_items))
            if (it.seriesId == sr.id && !it.episodeKey.isEmpty() && it.status != QLatin1String("failed")
                && it.status != QLatin1String("missed"))
                have.insert(it.episodeKey);
        const QVector<Programme> list = m_epg->range(c->epgId, now, now + 9 * 86400);
        for (const Programme &p : list) {
            if (p.stop <= now + 60 || ChannelModel::fold(p.title) != folded) continue;
            if (sr.skipped.contains(p.start)) continue;
            const QString episode = ChannelModel::fold(p.episode.isEmpty() ? p.subTitle : p.episode);
            if (!episode.isEmpty() && have.contains(episode)) continue;
            bool exists = false;
            for (const Item &it : std::as_const(m_items))
                if (it.type == QLatin1String("recording") && it.playlistId == m_playlist
                    && it.channelKey == sr.channelKey && it.start == p.start) { exists = true; break; }
            if (exists) continue;
            Item it;
            it.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            it.type = QStringLiteral("recording");
            it.status = QStringLiteral("scheduled");
            it.playlistId = m_playlist;
            it.channelKey = sr.channelKey;
            it.channelName = c->name;
            it.url = c->url;
            it.userAgent = c->userAgent;
            it.referrer = c->referrer;
            it.title = p.episode.isEmpty() ? p.title : p.title + QStringLiteral(" ") + p.episode;
            it.start = p.start;
            it.stop = p.stop;
            it.seriesId = sr.id;
            it.episodeKey = episode;
            m_items.append(it);
            if (!episode.isEmpty()) have.insert(episode);
            added = true;
            qInfo().noquote() << QStringLiteral("Schedule: series recording \"%1\" on %2 at %3")
                                     .arg(it.title, it.channelName,
                                          QDateTime::fromSecsSinceEpoch(it.start).toString(QStringLiteral("ddd HH:mm")));
        }
    }
    if (added) touch();
}

// ---- running --------------------------------------------------------------------------------

QString ScheduleService::fileNameFor(const Item &it) const
{
    static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    QString name = QStringLiteral("%1 - %2 - %3")
                       .arg(it.title, it.channelName,
                            QDateTime::fromSecsSinceEpoch(qMax(it.start, nowSecs()))
                                .toString(QStringLiteral("yyyy-MM-dd HHmm")));
    name.replace(bad, QStringLiteral("_"));
    name = name.simplified().left(150);
    QString path = m_folder + QLatin1Char('/') + name + QStringLiteral(".ts");
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = m_folder + QLatin1Char('/') + name + QStringLiteral(" (%1).ts").arg(n);
    return path;
}

void ScheduleService::startRecording(Item &it)
{
    // Use the channel's current address (logins in addresses can change).
    const int idx = m_channels->indexOfKey(it.channelKey);
    if (const Channel *c = m_channels->channel(idx)) {
        it.url = c->url;
        it.userAgent = c->userAgent;
        it.referrer = c->referrer;
    }
    if (it.url.isEmpty()) {
        it.status = QStringLiteral("failed");
        it.error = tr("The channel isn't in the playlist any more.");
        emit message(tr("Recording failed: %1").arg(it.title));
        touch();
        return;
    }
    QDir().mkpath(m_folder);
    it.file = fileNameFor(it);
    it.status = QStringLiteral("recording");
    it.error.clear();
    const QString id = it.id;
    auto begin = [this, id](const QString &address) {
        Item *item = find(id);
        if (!item || item->status != QLatin1String("recording") || m_recorders.contains(id)) return;
        auto *rec = new Recorder(m_net, QUrl(address), item->userAgent, item->referrer, item->file, this);
        m_recorders.insert(id, rec);
        connect(rec, &Recorder::changed, this, [this] { ++m_revision; emit changed(); });
        connect(rec, &Recorder::stopped, this, [this, id] { finishRecording(id); });
        qInfo().noquote() << QStringLiteral("Recording: started \"%1\" on %2 -> %3")
                                 .arg(item->title, item->channelName, QDir::toNativeSeparators(item->file));
        rec->start();
        touch();
    };
    emit message(tr("Recording %1").arg(it.title));
    if (PortalClient::isPortalUrl(it.url)) {
        // A portal channel: the portal gives out the address when asked.
        PortalClient::instance()->resolve(it.url, this, [this, id, begin](const QString &real, const QString &error) {
            if (!real.isEmpty()) { begin(real); return; }
            if (Item *item = find(id)) {
                if (item->status != QLatin1String("recording")) return;
                item->status = QStringLiteral("failed");
                item->error = error;
                emit message(tr("Recording failed: %1").arg(item->title));
                touch();
            }
        });
        touch();
        return;
    }
    begin(it.url);
}

void ScheduleService::finishRecording(const QString &id)
{
    Recorder *rec = m_recorders.take(id);
    if (!rec) return;
    if (Item *it = find(id)) {
        it->bytes = rec->bytes();
        it->status = rec->state() == Recorder::State::Failed ? QStringLiteral("failed") : QStringLiteral("done");
        it->error = rec->error();
        if (it->status == QLatin1String("failed") && it->bytes == 0) QFile::remove(it->file);
        if (it->status == QLatin1String("done") && !it->episodeKey.isEmpty())
            if (Series *sr = findSeries(it->seriesId))
                if (!sr->seen.contains(it->episodeKey)) sr->seen.append(it->episodeKey);
        qInfo().noquote() << QStringLiteral("Recording: %1 \"%2\" (%3 MB)%4")
                                 .arg(it->status == QLatin1String("done") ? QStringLiteral("finished") : QStringLiteral("failed"),
                                      it->title)
                                 .arg(it->bytes / 1048576.0, 0, 'f', 1)
                                 .arg(it->error.isEmpty() ? QString() : QStringLiteral(" · ") + it->error);
        emit message(it->status == QLatin1String("done") ? tr("Recording finished: %1").arg(it->title)
                                                         : tr("Recording failed: %1").arg(it->title));
    }
    rec->deleteLater();
    touch();
}

void ScheduleService::tick()
{
    const qint64 now = nowSecs();
    bool dirty = false;
    QVector<QString> toStop;
    for (int i = 0; i < m_items.size(); ++i) {
        Item &it = m_items[i];
        if (it.type == QLatin1String("reminder")) {
            if (it.status != QLatin1String("scheduled")) continue;
            if (now >= it.stop) {                       // missed (Relay wasn't running)
                m_items.removeAt(i--);
                dirty = true;
            } else if (now >= it.start - kReminderLead) {
                const QVariantMap m = toMap(it);
                m_items.removeAt(i--);
                dirty = true;
                emit reminderDue(m);
            }
            continue;
        }
        if (it.status == QLatin1String("scheduled")) {
            if (now >= it.stop + kRecordAfter) { it.status = QStringLiteral("missed"); dirty = true; }
            else if (now >= it.start - kRecordBefore) {
                if (it.playlistId != m_playlist) {
                    // Its channels aren't loaded: another playlist is open.
                    it.status = QStringLiteral("missed");
                    it.error = tr("Another playlist was open.");
                    emit message(tr("Couldn't record %1: its playlist isn't the open one").arg(it.title));
                    dirty = true;
                } else {
                    startRecording(it);
                }
            }
        } else if (it.status == QLatin1String("recording")) {
            if (now >= it.stop + kRecordAfter) toStop.append(it.id);
        }
    }
    for (const QString &id : toStop)
        if (Recorder *r = m_recorders.value(id)) r->stop();
    if (dirty) touch();
    if (now - m_lastApply >= 3600) applySeries();   // listings slide into the window
}

void ScheduleService::stopRecording(const QString &id)
{
    if (Recorder *r = m_recorders.value(id)) {
        if (Item *it = find(id)) it->stop = qMin(it->stop, nowSecs());
        r->stop();
    }
}

void ScheduleService::remove(const QString &id, bool deleteFile)
{
    if (Recorder *r = m_recorders.value(id)) r->stop();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].id != id) continue;
        if (deleteFile && !m_items[i].file.isEmpty()) QFile::remove(m_items[i].file);
        // A cancelled airing of a series stays cancelled.
        if (m_items[i].status == QLatin1String("scheduled"))
            if (Series *sr = findSeries(m_items[i].seriesId)) sr->skipped.append(m_items[i].start);
        m_items.removeAt(i);
        touch();
        return;
    }
}

void ScheduleService::shutdown()
{
    m_timer.stop();
    const QList<QString> ids = m_recorders.keys();
    for (const QString &id : ids)
        if (Recorder *r = m_recorders.value(id)) {
            if (Item *it = find(id)) it->error = tr("Stopped early because Relay was closed.");
            r->stop();
        }
    save();
}
