#include "UserLibrary.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtDebug>

#include <algorithm>
#include <limits>

UserLibrary::UserLibrary(const QString &path, QObject *parent)
    : QObject(parent), m_path(path)
{
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(400);
    connect(&m_saveTimer, &QTimer::timeout, this, &UserLibrary::saveNow);
    load();
}

UserLibrary::~UserLibrary()
{
    if (m_dirty) saveNow();
}

void UserLibrary::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : o.value(QLatin1String("recent")).toArray())
        if (!v.toString().isEmpty()) m_recent << v.toString();
    for (const QJsonValue &v : o.value(QLatin1String("lists")).toArray()) {
        const QJsonObject l = v.toObject();
        List list{l.value(QLatin1String("id")).toString(), l.value(QLatin1String("name")).toString(), {}};
        if (list.id.isEmpty() || list.name.isEmpty() || listRef(list.id)) continue;
        for (const QJsonValue &k : l.value(QLatin1String("keys")).toArray())
            if (!k.toString().isEmpty() && !list.keys.contains(k.toString())) list.keys << k.toString();
        m_lists.append(list);
    }
    {
        const QJsonObject ids = o.value(QLatin1String("guideIds")).toObject();
        for (auto it = ids.begin(); it != ids.end(); ++it) m_guideIds.insert(it.key(), it.value().toString());
        const QJsonObject offs = o.value(QLatin1String("guideOffsets")).toObject();
        for (auto it = offs.begin(); it != offs.end(); ++it) m_guideOffsets.insert(it.key(), it.value().toInt());
    }
    for (const QJsonValue &v : o.value(QLatin1String("favorites")).toArray()) {
        const QString key = v.toString();
        if (!key.isEmpty() && !m_favoriteSet.contains(key)) {
            m_favorites << key;
            m_favoriteSet.insert(key);
        }
    }
    for (const QJsonValue &v : o.value(QLatin1String("hiddenGroups")).toArray())
        m_hiddenGroups.insert(v.toString());
    const QJsonObject hidden = o.value(QLatin1String("hiddenChannels")).toObject();
    for (auto it = hidden.constBegin(); it != hidden.constEnd(); ++it)
        m_hiddenChannels.insert(it.key(), it.value().toString());
    auto readMap = [&](const char *name, QHash<QString, QString> *out) {
        const QJsonObject m = o.value(QLatin1String(name)).toObject();
        for (auto it = m.constBegin(); it != m.constEnd(); ++it) out->insert(it.key(), it.value().toString());
    };
    readMap("channelNames", &m_channelNames);
    readMap("groupNames", &m_groupNames);
    for (const QJsonValue &v : o.value(QLatin1String("groupOrder")).toArray())
        m_groupOrder << v.toString();
    const QJsonObject order = o.value(QLatin1String("channelOrder")).toObject();
    for (auto it = order.constBegin(); it != order.constEnd(); ++it) {
        QStringList keys;
        for (const QJsonValue &v : it.value().toArray()) keys << v.toString();
        m_channelOrder.insert(it.key(), keys);
    }
    for (const QJsonValue &v : o.value(QLatin1String("lockedChannels")).toArray()) m_lockedChannels.insert(v.toString());
    for (const QJsonValue &v : o.value(QLatin1String("lockedGroups")).toArray()) m_lockedGroups.insert(v.toString());
    for (const QJsonValue &v : o.value(QLatin1String("lockedVodCategories")).toArray()) m_lockedVod.insert(v.toString());
    const QJsonObject progress = o.value(QLatin1String("vodProgress")).toObject();
    for (auto it = progress.constBegin(); it != progress.constEnd(); ++it)
        m_vodProgress.insert(it.key(), it.value().toObject().toVariantMap());
    for (const QJsonValue &v : o.value(QLatin1String("vodFavorites")).toArray()) {
        const QJsonObject f = v.toObject();
        const QString key = f.value(QLatin1String("key")).toString();
        if (key.isEmpty() || m_vodFavoriteMeta.contains(key)) continue;
        m_vodFavorites << key;
        m_vodFavoriteMeta.insert(key, f.toVariantMap());
    }
    qInfo().noquote() << QStringLiteral("Library: %1 favorites, %2 hidden groups, %3 hidden channels")
                             .arg(m_favorites.size()).arg(m_hiddenGroups.size())
                             .arg(m_hiddenChannels.size());
}

void UserLibrary::saveNow()
{
    m_saveTimer.stop();
    if (!m_dirty) return;
    QJsonObject o;
    o.insert(QStringLiteral("version"), 1);
    o.insert(QStringLiteral("favorites"), QJsonArray::fromStringList(m_favorites));
    o.insert(QStringLiteral("recent"), QJsonArray::fromStringList(m_recent));
    {
        QJsonArray lists;
        for (const List &l : std::as_const(m_lists))
            lists.append(QJsonObject{{QStringLiteral("id"), l.id}, {QStringLiteral("name"), l.name},
                                     {QStringLiteral("keys"), QJsonArray::fromStringList(l.keys)}});
        o.insert(QStringLiteral("lists"), lists);
    }
    {
        QJsonObject ids, offs;
        for (auto it = m_guideIds.constBegin(); it != m_guideIds.constEnd(); ++it) ids.insert(it.key(), it.value());
        for (auto it = m_guideOffsets.constBegin(); it != m_guideOffsets.constEnd(); ++it) offs.insert(it.key(), it.value());
        o.insert(QStringLiteral("guideIds"), ids);
        o.insert(QStringLiteral("guideOffsets"), offs);
    }
    o.insert(QStringLiteral("hiddenGroups"), QJsonArray::fromStringList(hiddenGroups()));
    QJsonObject hidden;
    for (auto it = m_hiddenChannels.constBegin(); it != m_hiddenChannels.constEnd(); ++it)
        hidden.insert(it.key(), it.value());
    o.insert(QStringLiteral("hiddenChannels"), hidden);
    auto writeMap = [](const QHash<QString, QString> &in) {
        QJsonObject m;
        for (auto it = in.constBegin(); it != in.constEnd(); ++it) m.insert(it.key(), it.value());
        return m;
    };
    o.insert(QStringLiteral("channelNames"), writeMap(m_channelNames));
    o.insert(QStringLiteral("groupNames"), writeMap(m_groupNames));
    o.insert(QStringLiteral("groupOrder"), QJsonArray::fromStringList(m_groupOrder));
    QJsonObject order;
    for (auto it = m_channelOrder.constBegin(); it != m_channelOrder.constEnd(); ++it)
        order.insert(it.key(), QJsonArray::fromStringList(it.value()));
    o.insert(QStringLiteral("channelOrder"), order);
    auto list = [](const QSet<QString> &set) {
        QStringList l(set.cbegin(), set.cend());
        l.sort();
        return QJsonArray::fromStringList(l);
    };
    o.insert(QStringLiteral("lockedChannels"), list(m_lockedChannels));
    o.insert(QStringLiteral("lockedGroups"), list(m_lockedGroups));
    o.insert(QStringLiteral("lockedVodCategories"), list(m_lockedVod));
    QJsonObject progress;
    for (auto it = m_vodProgress.constBegin(); it != m_vodProgress.constEnd(); ++it)
        progress.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
    o.insert(QStringLiteral("vodProgress"), progress);
    QJsonArray vodFavs;
    for (const QString &key : std::as_const(m_vodFavorites))
        vodFavs.append(QJsonObject::fromVariantMap(m_vodFavoriteMeta.value(key)));
    o.insert(QStringLiteral("vodFavorites"), vodFavs);

    QSaveFile f(m_path);   // atomic: never leaves a half-written file
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        if (f.commit()) m_dirty = false;
    }
    if (m_dirty) qWarning().noquote() << "Library: couldn't save" << m_path;
}

void UserLibrary::scheduleSave()
{
    m_dirty = true;
    m_saveTimer.start();
    emit changed();
}

void UserLibrary::setFavorite(const QString &key, bool favorite)
{
    if (key.isEmpty() || favorite == m_favoriteSet.contains(key)) return;
    if (favorite) {
        m_favorites << key;
        m_favoriteSet.insert(key);
    } else {
        m_favorites.removeAll(key);
        m_favoriteSet.remove(key);
    }
    scheduleSave();
}

void UserLibrary::addRecent(const QString &key)
{
    if (key.isEmpty() || m_recent.value(0) == key) return;
    m_recent.removeAll(key);
    m_recent.prepend(key);
    while (m_recent.size() > 30) m_recent.removeLast();
    scheduleSave();
}

void UserLibrary::setGuideId(const QString &key, const QString &id)
{
    if (key.isEmpty() || m_guideIds.value(key) == id) return;
    if (id.isEmpty()) m_guideIds.remove(key);
    else m_guideIds.insert(key, id);
    scheduleSave();
}

void UserLibrary::setGuideOffset(const QString &key, int minutes)
{
    minutes = qBound(-720, minutes, 720);
    if (key.isEmpty() || m_guideOffsets.value(key) == minutes) return;
    if (minutes == 0) m_guideOffsets.remove(key);
    else m_guideOffsets.insert(key, minutes);
    scheduleSave();
}

// ---- extra favorites lists ----------------------------------------------------

UserLibrary::List *UserLibrary::listRef(const QString &id)
{
    for (List &l : m_lists)
        if (l.id == id) return &l;
    return nullptr;
}

const UserLibrary::List *UserLibrary::list(const QString &id) const
{
    return const_cast<UserLibrary *>(this)->listRef(id);
}

QString UserLibrary::createList(const QString &name)
{
    const QString n = name.trimmed();
    if (n.isEmpty()) return {};
    int number = 1;
    QString id;
    do id = QStringLiteral("\x1Flist:%1").arg(number++); while (listRef(id));
    m_lists.append(List{id, n, {}});
    scheduleSave();
    return id;
}

void UserLibrary::renameList(const QString &id, const QString &name)
{
    List *l = listRef(id);
    const QString n = name.trimmed();
    if (!l || n.isEmpty() || l->name == n) return;
    l->name = n;
    scheduleSave();
}

void UserLibrary::removeList(const QString &id)
{
    for (int i = 0; i < m_lists.size(); ++i)
        if (m_lists[i].id == id) { m_lists.removeAt(i); scheduleSave(); return; }
}

void UserLibrary::moveList(const QString &id, int delta)
{
    for (int i = 0; i < m_lists.size(); ++i) {
        if (m_lists[i].id != id) continue;
        const int to = i + (delta > 0 ? 1 : -1);
        if (to < 0 || to >= m_lists.size()) return;
        m_lists.move(i, to);
        scheduleSave();
        return;
    }
}

void UserLibrary::setInList(const QString &id, const QString &key, bool in)
{
    List *l = listRef(id);
    if (!l || key.isEmpty() || l->keys.contains(key) == in) return;
    if (in) l->keys.append(key);
    else l->keys.removeAll(key);
    scheduleSave();
}

void UserLibrary::moveInList(const QString &id, int from, int to)
{
    List *l = listRef(id);
    if (!l || from < 0 || from >= l->keys.size() || to < 0 || to >= l->keys.size() || from == to) return;
    l->keys.move(from, to);
    scheduleSave();
}

void UserLibrary::moveFavorite(int from, int to)
{
    if (from < 0 || from >= m_favorites.size() || to < 0 || to >= m_favorites.size() || from == to)
        return;
    m_favorites.move(from, to);
    scheduleSave();
}

void UserLibrary::setChannelName(const QString &key, const QString &name)
{
    const QString n = name.trimmed();
    if (n == m_channelNames.value(key)) return;
    if (n.isEmpty()) m_channelNames.remove(key);
    else m_channelNames.insert(key, n);
    scheduleSave();
}

void UserLibrary::setGroupName(const QString &group, const QString &name)
{
    const QString n = name.trimmed();
    if (n == m_groupNames.value(group)) return;
    if (n.isEmpty() || n == group) m_groupNames.remove(group);
    else m_groupNames.insert(group, n);
    scheduleSave();
}

void UserLibrary::setGroupOrder(const QStringList &order)
{
    if (order == m_groupOrder) return;
    m_groupOrder = order;
    scheduleSave();
}

void UserLibrary::setChannelOrder(const QString &group, const QStringList &keys)
{
    if (keys == m_channelOrder.value(group)) return;
    if (keys.isEmpty()) m_channelOrder.remove(group);
    else m_channelOrder.insert(group, keys);
    scheduleSave();
}

void UserLibrary::setGroupHidden(const QString &group, bool hidden)
{
    if (hidden == m_hiddenGroups.contains(group)) return;
    if (hidden) m_hiddenGroups.insert(group);
    else m_hiddenGroups.remove(group);
    scheduleSave();
}

QStringList UserLibrary::hiddenGroups() const
{
    QStringList list(m_hiddenGroups.cbegin(), m_hiddenGroups.cend());
    std::sort(list.begin(), list.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return list;
}

void UserLibrary::setChannelHidden(const QString &key, bool hidden, const QString &label)
{
    if (key.isEmpty() || hidden == m_hiddenChannels.contains(key)) return;
    if (hidden) {
        m_hiddenChannels.insert(key, label);
        if (m_favoriteSet.contains(key)) {   // hiding wins over favorite
            m_favorites.removeAll(key);
            m_favoriteSet.remove(key);
        }
    } else {
        m_hiddenChannels.remove(key);
    }
    scheduleSave();
}

void UserLibrary::setVodProgress(const QString &key, const QVariantMap &entry)
{
    if (key.isEmpty()) return;
    m_vodProgress.insert(key, entry);
    // Keep the file small: remember the 300 most recent items.
    if (m_vodProgress.size() > 300) {
        QString oldest;
        qint64 oldestT = std::numeric_limits<qint64>::max();
        for (auto it = m_vodProgress.cbegin(); it != m_vodProgress.cend(); ++it) {
            const qint64 t = it.value().value(QStringLiteral("t")).toLongLong();
            if (t < oldestT) { oldestT = t; oldest = it.key(); }
        }
        m_vodProgress.remove(oldest);
    }
    scheduleSave();
}

void UserLibrary::removeVodProgress(const QString &key)
{
    if (m_vodProgress.remove(key)) scheduleSave();
}

void UserLibrary::setVodFavorite(const QString &key, bool favorite, const QVariantMap &meta)
{
    if (key.isEmpty() || favorite == m_vodFavoriteMeta.contains(key)) return;
    if (favorite) {
        QVariantMap m = meta;
        m.insert(QStringLiteral("key"), key);
        m_vodFavorites.prepend(key);
        m_vodFavoriteMeta.insert(key, m);
    } else {
        m_vodFavorites.removeAll(key);
        m_vodFavoriteMeta.remove(key);
    }
    scheduleSave();
}

static bool setIn(QSet<QString> &set, const QString &key, bool on)
{
    if (key.isEmpty() || on == set.contains(key)) return false;
    if (on) set.insert(key); else set.remove(key);
    return true;
}

void UserLibrary::setChannelLocked(const QString &key, bool locked)
{
    if (setIn(m_lockedChannels, key, locked)) scheduleSave();
}

void UserLibrary::setGroupLocked(const QString &group, bool locked)
{
    if (setIn(m_lockedGroups, group, locked)) scheduleSave();
}

void UserLibrary::setVodCategoryLocked(const QString &key, bool locked)
{
    if (setIn(m_lockedVod, key, locked)) scheduleSave();
}

void UserLibrary::setPath(const QString &path)
{
    if (path == m_path) return;
    saveNow();
    m_favorites.clear();
    m_recent.clear();
    m_lists.clear();
    m_guideIds.clear();
    m_guideOffsets.clear();
    m_favoriteSet.clear();
    m_hiddenGroups.clear();
    m_hiddenChannels.clear();
    m_channelNames.clear();
    m_groupNames.clear();
    m_groupOrder.clear();
    m_channelOrder.clear();
    m_vodProgress.clear();
    m_vodFavorites.clear();
    m_vodFavoriteMeta.clear();
    m_lockedChannels.clear();
    m_lockedGroups.clear();
    m_lockedVod.clear();
    m_dirty = false;
    m_path = path;
    load();
    emit changed();
}
