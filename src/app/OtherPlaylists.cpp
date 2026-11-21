#include "OtherPlaylists.h"

#include "AppSettings.h"
#include "Session.h"
#include "models/ChannelModel.h"
#include "sources/PlaylistService.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtDebug>

namespace {
QString fold(const QString &s)
{
    const QString d = s.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d)
        if (c.category() != QChar::Mark_NonSpacing) out.append(c.toLower());
    return out;
}
} // namespace

OtherPlaylists::OtherPlaylists(AppSettings *settings, const QString &dataDir, QObject *parent)
    : QObject(parent), m_settings(settings), m_dataDir(dataDir)
{
    // Playlists can only be edited while they're the active one, and then
    // the copy here may be out of date; removed ones go too.
    connect(m_settings, &AppSettings::sourceChanged, this, [this] { forget(m_settings->activeId()); });
    connect(m_settings, &AppSettings::playlistsChanged, this, [this] {
        const QStringList ids = m_settings->playlistIds();
        for (const QString &id : m_entries.keys())
            if (!ids.contains(id)) forget(id);
    });
}

void OtherPlaylists::forget(const QString &id)
{
    if (PlaylistService *l = m_loaders.take(id)) { l->cancel(); l->deleteLater(); }
    if (m_entries.remove(id)) { ++m_revision; emit changed(); }
}

void OtherPlaylists::readLibrary(const QString &id, Entry &e) const
{
    QFile f(Session::storageDir(m_dataDir, id) + QStringLiteral("/library.json"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QSet<QString> hiddenGroups, hiddenChannels;
    for (const QJsonValue &v : o.value(QLatin1String("hiddenGroups")).toArray()) hiddenGroups.insert(v.toString());
    const QJsonObject hidden = o.value(QLatin1String("hiddenChannels")).toObject();
    for (auto it = hidden.constBegin(); it != hidden.constEnd(); ++it) hiddenChannels.insert(it.key());
    for (const QJsonValue &v : o.value(QLatin1String("lockedChannels")).toArray()) e.lockedChannels.insert(v.toString());
    for (const QJsonValue &v : o.value(QLatin1String("lockedGroups")).toArray()) e.lockedGroups.insert(v.toString());
    const QJsonObject names = o.value(QLatin1String("channelNames")).toObject();

    // Leave out what the user hid in that playlist; use their channel names.
    QVector<Channel> kept;
    kept.reserve(e.channels.size());
    for (Channel c : std::as_const(e.channels)) {
        const QString k = ChannelModel::channelKey(c);
        if (hiddenGroups.contains(c.group) || hiddenChannels.contains(k)) continue;
        const QString custom = names.value(k).toString();
        if (!custom.isEmpty()) c.name = custom;
        kept.append(c);
    }
    e.channels = std::move(kept);
}

void OtherPlaylists::ensure(const QString &id)
{
    if (id.isEmpty() || id == m_settings->activeId()) return;
    const Entry existing = m_entries.value(id);
    if (existing.state == QLatin1String("ready") || existing.state == QLatin1String("loading")) return;

    const AppSettings::Source src = m_settings->source(id);
    if (src.type.isEmpty()) return;
    Entry &e = m_entries[id];
    e = Entry();
    e.state = QStringLiteral("loading");
    ++m_revision;
    emit changed();

    auto *loader = new PlaylistService(this);
    m_loaders.insert(id, loader);
    connect(loader, &PlaylistService::loaded, this, [this, id, loader](const PlaylistResult &r) {
        if (m_loaders.value(id) != loader) return;
        m_loaders.remove(id);
        loader->deleteLater();
        Entry &e = m_entries[id];
        e.channels = r.channels;
        readLibrary(id, e);
        e.byKey.clear();
        e.folded.clear();
        for (int i = 0; i < e.channels.size(); ++i) {
            e.byKey.insert(ChannelModel::channelKey(e.channels[i]), i);
            e.folded << fold(e.channels[i].name);
        }
        e.state = QStringLiteral("ready");
        qInfo().noquote() << QStringLiteral("Playlists: loaded %1 channels of \"%2\" for multi-view")
                                 .arg(e.channels.size()).arg(m_settings->playlistName(id));
        ++m_revision;
        emit changed();
    });
    connect(loader, &PlaylistService::failed, this, [this, id, loader](const QString &message) {
        if (m_loaders.value(id) != loader) return;
        m_loaders.remove(id);
        loader->deleteLater();
        Entry &e = m_entries[id];
        e.state = QStringLiteral("failed");
        e.error = message;
        ++m_revision;
        emit changed();
    });
    if (src.type == QLatin1String("xtream")) loader->loadXtream(src.server, src.user, src.password, src.output);
    else loader->loadM3u(src.m3u);
}

QString OtherPlaylists::state(const QString &id) const { return m_entries.value(id).state; }
QString OtherPlaylists::error(const QString &id) const { return m_entries.value(id).error; }

bool OtherPlaylists::isLocked(const QString &id, const QString &key) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd()) return false;
    if (it->lockedChannels.contains(key)) return true;
    const int i = it->byKey.value(key, -1);
    return i >= 0 && it->lockedGroups.contains(it->channels[i].group);
}

QVariantMap OtherPlaylists::toMap(const Entry &e, int i, bool withUrl) const
{
    const Channel &c = e.channels[i];
    const QString k = ChannelModel::channelKey(c);
    QVariantMap m{
        {QStringLiteral("key"), k},
        {QStringLiteral("number"), c.number},
        {QStringLiteral("name"), c.name},
        {QStringLiteral("group"), c.group},
        {QStringLiteral("logo"), c.logo},
        {QStringLiteral("locked"), e.lockedChannels.contains(k) || e.lockedGroups.contains(c.group)},
    };
    if (withUrl) {
        m.insert(QStringLiteral("url"), c.url);
        m.insert(QStringLiteral("userAgent"), c.userAgent);
        m.insert(QStringLiteral("referrer"), c.referrer);
    }
    return m;
}

QVariantList OtherPlaylists::channels(const QString &id, const QString &query, int limit) const
{
    QVariantList out;
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd() || it->state != QLatin1String("ready")) return out;
    const QString q = fold(query.trimmed());
    const QStringList words = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool isNumber = false;
    const int number = query.trimmed().toInt(&isNumber);
    QVector<int> exact, starts, rest;
    for (int i = 0; i < it->channels.size(); ++i) {
        if (words.isEmpty()) { rest.append(i); if (rest.size() >= limit) break; continue; }
        if (isNumber && it->channels[i].number == number) { exact.append(i); continue; }
        bool match = true;
        for (const QString &w : words)
            if (!it->folded[i].contains(w)) { match = false; break; }
        if (match) (it->folded[i].startsWith(q) ? starts : rest).append(i);
    }
    for (const QVector<int> *list : {&exact, &starts, &rest})
        for (int i : *list) {
            if (out.size() >= limit) return out;
            out.append(toMap(*it, i, false));
        }
    return out;
}

QVariantMap OtherPlaylists::info(const QString &id, const QString &key) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd()) return {};
    const int i = it->byKey.value(key, -1);
    return i >= 0 ? toMap(*it, i, true) : QVariantMap();
}

QString OtherPlaylists::step(const QString &id, const QString &key, int delta,
                             const QStringList &skip, bool skipLocked) const
{
    const auto it = m_entries.constFind(id);
    if (it == m_entries.constEnd() || it->channels.isEmpty()) return QString();
    const int n = int(it->channels.size());
    const int from = it->byKey.value(key, 0);
    for (int k = 1; k < n; ++k) {             // never back to where it started
        const int i = ((from + delta * k) % n + n) % n;
        const QString ck = ChannelModel::channelKey(it->channels[i]);
        if (skip.contains(ck)) continue;
        if (skipLocked && isLocked(id, ck)) continue;
        return ck;
    }
    return QString();
}
