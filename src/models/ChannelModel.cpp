#include "ChannelModel.h"

#include "app/UserLibrary.h"
#include "epg/EpgService.h"

#include <QDateTime>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <climits>

ChannelModel::ChannelModel(QObject *parent)
    : QAbstractListModel(parent)
{
    m_groupNames << tr("Favorites") << tr("Recently watched") << tr("All channels");
    m_members.resize(2);
    m_guideTick.setInterval(30000);
    connect(&m_guideTick, &QTimer::timeout, this, &ChannelModel::refreshGuideRoles);
    m_guideTick.start();
}

QString ChannelModel::channelKey(const Channel &c)
{
    QString last = QUrl(c.url).path().section(QLatin1Char('/'), -1);
    const int dot = last.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) last.truncate(dot);
    bool numeric = !last.isEmpty();
    for (const QChar ch : std::as_const(last))
        if (!ch.isDigit()) { numeric = false; break; }
    return numeric ? QStringLiteral("s:") + last : QStringLiteral("u:") + c.url;
}

QString ChannelModel::fold(const QString &text)
{
    const QString d = text.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d)
        if (c.category() != QChar::Mark_NonSpacing) out.append(c);
    return out.toCaseFolded().simplified();
}

void ChannelModel::applyNames()
{
    m_folded.resize(m_all.size());
    for (int i = 0; i < m_all.size(); ++i) {
        const QString custom = m_library ? m_library->channelName(m_keys[i]) : QString();
        m_all[i].name = custom.isEmpty() ? m_original[i].name : custom;
        m_folded[i] = fold(m_all[i].name);
    }
}

void ChannelModel::setEpgIds(const QVector<QString> &ids)
{
    for (int i = 0; i < m_all.size() && i < ids.size(); ++i) {
        m_all[i].epgId = ids[i];
        m_original[i].epgId = ids[i];
    }
    refreshGuideRoles();
}

void ChannelModel::setCatchupAccount(const Catchup::Account &account)
{
    m_catchupAccount = account;
    const int n = rowCount();
    if (n > 0) emit dataChanged(index(0), index(n - 1), {CatchupDaysRole});
}

int ChannelModel::applyArchiveDays(const QHash<QString, int> &days)
{
    int applied = 0;
    for (int i = 0; i < m_all.size(); ++i) {
        if (!m_all[i].catchupType.isEmpty() && m_all[i].catchupType != QLatin1String("xc")) continue;
        const int d = days.value(Catchup::streamIdFromUrl(m_all[i].url));
        for (Channel *c : {&m_all[i], &m_original[i]}) {
            c->catchupType = d > 0 ? QStringLiteral("xc") : QString();
            c->catchupDays = d;
        }
        if (d > 0) ++applied;
    }
    const int n = rowCount();
    if (n > 0) emit dataChanged(index(0), index(n - 1), {CatchupDaysRole});
    return applied;
}

int ChannelModel::catchupChannelCount() const
{
    int n = 0;
    for (const Channel &c : m_all)
        if (Catchup::supported(c, m_catchupAccount)) ++n;
    return n;
}

void ChannelModel::refreshGuideRoles()
{
    const int n = rowCount();
    if (n > 0) emit dataChanged(index(0), index(n - 1), {NowTitleRole, NowProgressRole, EpgIdRole});
}

void ChannelModel::setPlaylist(const PlaylistResult &result)
{
    m_all = result.channels;
    m_original = result.channels;
    m_keys.clear();
    m_keys.reserve(m_all.size());
    m_firstByKey.clear();
    m_playlistGroups.clear();
    m_byGroup.clear();

    // Group order: provider's order first, then any remaining groups in the
    // order they first appear.
    QSet<QString> used;
    for (const Channel &c : std::as_const(m_all)) used.insert(c.group);
    for (const QString &g : result.groupOrder)
        if (used.contains(g) && !m_byGroup.contains(g)) {
            m_playlistGroups << g;
            m_byGroup.insert(g, {});
        }

    for (int i = 0; i < m_all.size(); ++i) {
        const QString key = channelKey(m_all[i]);
        m_keys << key;
        if (!m_firstByKey.contains(key)) m_firstByKey.insert(key, i);
        const QString &g = m_all[i].group;
        auto it = m_byGroup.find(g);
        if (it == m_byGroup.end()) {
            m_playlistGroups << g;
            it = m_byGroup.insert(g, {});
        }
        it->append(i);
    }
    applyNames();
    m_recentKeys = m_library ? m_library->recent() : QStringList();

    beginResetModel();
    m_currentGroup = AllGroup;
    computeGroups(&m_groupNames, &m_members);
    endResetModel();
    emit groupsChanged();
    emit countsChanged();
    emit currentGroupChanged();
    emit countChanged();
    emit hiddenChanged();
}

void ChannelModel::computeGroups(QStringList *namesOut, QVector<QVector<int>> *membersOut) const
{
    QStringList names{tr("Favorites"), tr("Recently watched"), tr("All channels")};
    QVector<QVector<int>> members(3);

    // Recently watched, newest first.
    for (const QString &key : m_recentKeys) {
        const auto it = m_firstByKey.constFind(key);
        if (it != m_firstByKey.constEnd() && !isHidden(it.value()))
            members[RecentGroup].append(it.value());
    }

    // Favorites, in the order the user added them.
    if (m_library) {
        for (const QString &key : m_library->favorites()) {
            const auto it = m_firstByKey.constFind(key);
            if (it != m_firstByKey.constEnd() && !m_library->isChannelHidden(key))
                members[FavoritesGroup].append(it.value());
        }
    }

    // The user's own lists, in their order (shown even when empty).
    if (m_library) {
        for (const UserLibrary::List &l : m_library->lists()) {
            QVector<int> list;
            for (const QString &key : l.keys) {
                const auto it = m_firstByKey.constFind(key);
                if (it != m_firstByKey.constEnd() && !m_library->isChannelHidden(key)) list.append(it.value());
            }
            names << l.id;
            members.append(list);
        }
    }

    for (const QString &g : orderedGroupIds()) {
        if (m_library && m_library->isGroupHidden(g)) continue;
        QVector<int> list;
        for (int i : orderedMembers(g))
            if (!m_library || !m_library->isChannelHidden(m_keys[i])) list.append(i);
        if (list.isEmpty()) continue;
        names << g;
        members.append(list);
    }
    // All channels keeps playlist order.
    members[AllGroup].reserve(m_all.size());
    for (int i = 0; i < m_all.size(); ++i)
        if (!isHidden(i)) members[AllGroup].append(i);

    *namesOut = names;
    *membersOut = members;
}

void ChannelModel::noteWatched(int globalIndex, bool keepOrder)
{
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return;
    m_library->addRecent(m_keys[globalIndex]);
    if (!keepOrder) refreshRecent();
}

void ChannelModel::refreshRecent()
{
    if (!m_library || m_recentKeys == m_library->recent()) return;
    m_recentKeys = m_library->recent();
    rebuild();
}

QStringList ChannelModel::orderedGroupIds() const
{
    if (!m_library || m_library->groupOrder().isEmpty()) return m_playlistGroups;
    QStringList out;
    QSet<QString> seen;
    for (const QString &g : m_library->groupOrder())
        if (m_byGroup.contains(g) && !seen.contains(g)) { out << g; seen.insert(g); }
    for (const QString &g : m_playlistGroups)
        if (!seen.contains(g)) out << g;
    return out;
}

QVector<int> ChannelModel::orderedMembers(const QString &group) const
{
    QVector<int> list = m_byGroup.value(group);
    const QStringList order = m_library ? m_library->channelOrder(group) : QStringList();
    if (order.isEmpty()) return list;
    QHash<QString, int> pos;
    for (int i = 0; i < order.size(); ++i)
        if (!pos.contains(order[i])) pos.insert(order[i], i);
    std::stable_sort(list.begin(), list.end(), [&](int a, int b) {
        return pos.value(m_keys[a], INT_MAX) < pos.value(m_keys[b], INT_MAX);
    });
    return list;
}

bool ChannelModel::isHidden(int g) const
{
    if (!m_library) return false;
    return m_library->isChannelHidden(m_keys[g]) || m_library->isGroupHidden(m_all[g].group);
}

void ChannelModel::rebuild()
{
    const QString currentName = m_groupNames.value(m_currentGroup);
    const QVector<int> oldVisible = m_members.value(m_currentGroup);
    const QStringList oldNames = m_groupNames;
    QStringList oldLabels;
    for (int i = 0; i < m_groupNames.size(); ++i) oldLabels << groupLabel(i);

    QStringList names;
    QVector<QVector<int>> members;
    computeGroups(&names, &members);

    int newCurrent = names.indexOf(currentName);
    if (newCurrent < 0) newCurrent = AllGroup;

    const bool viewChanged = newCurrent != m_currentGroup || members[newCurrent] != oldVisible;
    if (viewChanged) beginResetModel();
    m_groupNames = names;
    m_members = members;
    const bool groupChanged = newCurrent != m_currentGroup;
    m_currentGroup = newCurrent;
    if (viewChanged) endResetModel();

    QStringList newLabels;
    for (int i = 0; i < m_groupNames.size(); ++i) newLabels << groupLabel(i);
    if (names != oldNames || newLabels != oldLabels) emit groupsChanged();
    emit countsChanged();
    if (groupChanged) emit currentGroupChanged();
    if (viewChanged) emit countChanged();
    emit hiddenChanged();
}

const Channel *ChannelModel::channel(int globalIndex) const
{
    if (globalIndex < 0 || globalIndex >= m_all.size()) return nullptr;
    return &m_all[globalIndex];
}

int ChannelModel::rowCount(const QModelIndex &parent) const
{
    return (parent.isValid() || m_members.isEmpty()) ? 0 : visible().size();
}

QVariant ChannelModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= visible().size()) return {};
    const int global = visible()[index.row()];
    const Channel &c = m_all[global];
    switch (role) {
    case NumberRole: return c.number;
    case NameRole: return c.name;
    case GroupRole: return labelForGroupId(c.group);
    case LogoRole: return c.logo;
    case GlobalIndexRole: return global;
    case EpgIdRole: return c.epgId;
    case FavoriteRole: return m_library && m_library->isFavorite(m_keys[global]);
    case LockedRole: return isLocked(global);
    case CatchupDaysRole: return Catchup::supported(c, m_catchupAccount) ? c.catchupDays : 0;
    case NowTitleRole:
    case NowProgressRole: {
        if (!m_epg || c.epgId.isEmpty() || (m_blocked && m_blocked(global)))
            return role == NowTitleRole ? QVariant(QString()) : QVariant(-1.0);
        const auto nn = m_epg->nowNext(c.epgId);
        if (!nn.hasNow) return role == NowTitleRole ? QVariant(QString()) : QVariant(-1.0);
        if (role == NowTitleRole) return nn.now.title;
        const double len = double(nn.now.stop - nn.now.start);
        const double done = double(QDateTime::currentSecsSinceEpoch() - nn.now.start);
        return len > 0 ? qBound(0.0, done / len, 1.0) : 0.0;
    }
    default: return {};
    }
}

QHash<int, QByteArray> ChannelModel::roleNames() const
{
    return {
        {NumberRole, "number"},
        {NameRole, "name"},
        {GroupRole, "group"},
        {LogoRole, "logo"},
        {GlobalIndexRole, "globalIndex"},
        {NowTitleRole, "nowTitle"},
        {NowProgressRole, "nowProgress"},
        {EpgIdRole, "epgId"},
        {FavoriteRole, "favorite"},
        {CatchupDaysRole, "catchupDays"},
        {LockedRole, "locked"},
    };
}

QVariantList ChannelModel::groups() const
{
    QVariantList list;
    list.reserve(m_groupNames.size());
    for (int i = 0; i < m_groupNames.size(); ++i)
        list.append(QVariantMap{{QStringLiteral("name"), groupLabel(i)},
                                {QStringLiteral("locked"), isGroupLocked(i)},
                                {QStringLiteral("list"), isListId(m_groupNames[i])}});
    return list;
}

QVariantList ChannelModel::groupCounts() const
{
    QVariantList list;
    list.reserve(m_members.size());
    for (const auto &m : m_members) list.append(m.size());
    return list;
}

int ChannelModel::totalCount() const
{
    return m_members.size() > AllGroup ? m_members[AllGroup].size() : 0;
}

void ChannelModel::setCurrentGroup(int group)
{
    if (group < 0 || group >= m_members.size() || group == m_currentGroup) return;
    beginResetModel();
    m_currentGroup = group;
    endResetModel();
    emit currentGroupChanged();
    emit countChanged();
}

QString ChannelModel::groupName(int group) const { return m_groupNames.value(group); }

QString ChannelModel::groupLabel(int group) const
{
    return group > AllGroup ? labelForGroupId(m_groupNames.value(group)) : m_groupNames.value(group);
}

QString ChannelModel::labelForGroupId(const QString &id) const
{
    if (isListId(id)) {
        const UserLibrary::List *l = m_library ? m_library->list(id) : nullptr;
        return l ? l->name : QString();
    }
    const QString custom = m_library ? m_library->groupName(id) : QString();
    return custom.isEmpty() ? id : custom;
}
int ChannelModel::groupIndex(const QString &name) const { return m_groupNames.indexOf(name); }

int ChannelModel::globalIndexAt(int row) const
{
    return (row >= 0 && row < visible().size()) ? visible()[row] : -1;
}

int ChannelModel::rowOf(int globalIndex) const
{
    return m_members.isEmpty() ? -1 : visible().indexOf(globalIndex);
}

int ChannelModel::findByNumber(int number) const
{
    for (int i = 0; i < m_all.size(); ++i)
        if (m_all[i].number == number && !isHidden(i)) return i;
    return -1;
}

int ChannelModel::groupOfChannel(int globalIndex) const
{
    const Channel *c = channel(globalIndex);
    if (!c) return AllGroup;
    const int idx = m_groupNames.indexOf(c->group, AllGroup + 1);
    return idx < 0 ? AllGroup : idx;
}

int ChannelModel::step(int globalIndex, int delta, int group) const
{
    if (group < 0 || group >= m_members.size()) group = AllGroup;
    const QVector<int> &list = m_members[group];
    if (list.isEmpty()) return -1;
    // Locked channels are skipped while the parental lock is on.
    auto ok = [this](int gi) { return !m_blocked || !m_blocked(gi); };
    const int pos = list.indexOf(globalIndex);
    if (pos < 0) {
        for (int gi : list) if (ok(gi)) return gi;
        return -1;
    }
    if (delta == 0) return list[pos];
    const int n = list.size();
    for (int k = 1; k <= n; ++k) {
        const int p = ((pos + delta * k) % n + n) % n;
        if (ok(list[p])) return list[p];
    }
    return -1;
}

bool ChannelModel::isLocked(int globalIndex) const
{
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return false;
    return m_library->isChannelLocked(m_keys[globalIndex])
        || m_library->isGroupLocked(m_original[globalIndex].group);
}

bool ChannelModel::isGroupLocked(int group) const
{
    return m_library && group > AllGroup && group < m_groupNames.size()
        && m_library->isGroupLocked(groupName(group));
}

void ChannelModel::notifyLocksChanged()
{
    const int n = rowCount();
    if (n > 0) emit dataChanged(index(0), index(n - 1), {LockedRole, NowTitleRole, NowProgressRole});
    emit groupsChanged();
}

// ---- user choices ----------------------------------------------------------------

bool ChannelModel::isFavorite(int globalIndex) const
{
    return m_library && globalIndex >= 0 && globalIndex < m_keys.size()
        && m_library->isFavorite(m_keys[globalIndex]);
}

bool ChannelModel::toggleFavorite(int globalIndex)
{
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return false;
    const QString &key = m_keys[globalIndex];
    const bool fav = !m_library->isFavorite(key);
    m_library->setFavorite(key, fav);
    rebuild();
    // Update the star on every visible row showing this stream.
    for (int r = 0; r < visible().size(); ++r)
        if (m_keys[visible()[r]] == key) emit dataChanged(index(r), index(r), {FavoriteRole});
    return fav;
}

// ---- the user's own favorites lists ------------------------------------------------

bool ChannelModel::isListGroup(int group) const
{
    return group > AllGroup && group < m_groupNames.size() && isListId(m_groupNames[group]);
}

int ChannelModel::createList(const QString &name)
{
    if (!m_library) return -1;
    const QString id = m_library->createList(name);
    if (id.isEmpty()) return -1;
    rebuild();
    return groupIndex(id);
}

void ChannelModel::deleteList(int group)
{
    if (!m_library || !isListGroup(group)) return;
    m_library->removeList(m_groupNames[group]);
    rebuild();
}

QVariantList ChannelModel::listsFor(int globalIndex) const
{
    QVariantList out;
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return out;
    for (const UserLibrary::List &l : m_library->lists())
        out.append(QVariantMap{{QStringLiteral("id"), l.id}, {QStringLiteral("name"), l.name},
                               {QStringLiteral("member"), l.keys.contains(m_keys[globalIndex])}});
    return out;
}

void ChannelModel::setInList(int globalIndex, const QString &listId, bool in)
{
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return;
    m_library->setInList(listId, m_keys[globalIndex], in);
    rebuild();
}

void ChannelModel::hideChannel(int globalIndex)
{
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return;
    const Channel &c = m_all[globalIndex];
    m_library->setChannelHidden(m_keys[globalIndex], true,
                                QStringLiteral("%1 · %2").arg(c.name, c.group));
    rebuild();
}

void ChannelModel::unhideChannel(const QString &key)
{
    if (!m_library) return;
    m_library->setChannelHidden(key, false);
    rebuild();
}

void ChannelModel::hideGroup(const QString &name)
{
    if (!m_library || name.isEmpty() || !m_byGroup.contains(name)) return;
    m_library->setGroupHidden(name, true);
    rebuild();
}

void ChannelModel::unhideGroup(const QString &name)
{
    if (!m_library) return;
    m_library->setGroupHidden(name, false);
    rebuild();
}

QVariantList ChannelModel::hiddenGroupsList() const
{
    QVariantList list;
    if (!m_library) return list;
    for (const QString &g : m_library->hiddenGroups())
        list.append(QVariantMap{{QStringLiteral("name"), g},
                                {QStringLiteral("count"), m_byGroup.value(g).size()}});
    return list;
}

QVariantList ChannelModel::hiddenChannelsList() const
{
    QVariantList list;
    if (!m_library) return list;
    const auto &hidden = m_library->hiddenChannels();
    QStringList keys = hidden.keys();
    std::sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
        return hidden.value(a).compare(hidden.value(b), Qt::CaseInsensitive) < 0;
    });
    for (const QString &k : keys)
        list.append(QVariantMap{{QStringLiteral("key"), k},
                                {QStringLiteral("name"), hidden.value(k)}});
    return list;
}

// ---- renaming ---------------------------------------------------------------------

QString ChannelModel::originalChannelName(int globalIndex) const
{
    return (globalIndex >= 0 && globalIndex < m_original.size()) ? m_original[globalIndex].name : QString();
}

void ChannelModel::renameChannel(int globalIndex, const QString &name)
{
    if (!m_library || globalIndex < 0 || globalIndex >= m_keys.size()) return;
    const QString key = m_keys[globalIndex];
    const QString n = name.trimmed();
    m_library->setChannelName(key, n == m_original[globalIndex].name ? QString() : n);
    applyNames();
    for (int r = 0; r < visible().size(); ++r)
        if (m_keys[visible()[r]] == key) emit dataChanged(index(r), index(r), {NameRole});
    for (int i = 0; i < m_keys.size(); ++i)
        if (m_keys[i] == key) emit channelRenamed(i);
}

void ChannelModel::renameGroup(int group, const QString &name)
{
    if (!m_library || !canEditGroup(group)) return;
    if (isListGroup(group)) m_library->renameList(m_groupNames[group], name);
    else m_library->setGroupName(m_groupNames[group], name);
    rebuild();
    const int n = rowCount();
    if (n > 0) emit dataChanged(index(0), index(n - 1), {GroupRole});
}

// ---- reordering ---------------------------------------------------------------------

namespace {
// Moves `item` so it sits where `neighbor` is (before it when moving up,
// after it when moving down), keeping everything else in place.
void moveNextTo(QStringList &list, const QString &item, const QString &neighbor, int delta)
{
    list.removeAll(item);
    int at = list.indexOf(neighbor);
    if (at < 0) { list.append(item); return; }
    list.insert(delta > 0 ? at + 1 : at, item);
}
} // namespace

int ChannelModel::moveGroup(int group, int delta)
{
    if (!m_library || !canEditGroup(group) || delta == 0) return group;
    const int target = group + (delta > 0 ? 1 : -1);
    if (!canEditGroup(target) || isListGroup(target) != isListGroup(group)) return group;
    const QString id = m_groupNames[group];
    if (isListGroup(group)) {
        m_library->moveList(id, delta);
        rebuild();
        return groupIndex(id);
    }
    QStringList order = orderedGroupIds();
    moveNextTo(order, id, m_groupNames[target], delta);
    m_library->setGroupOrder(order);
    rebuild();
    return groupIndex(id);
}

int ChannelModel::moveChannel(int row, int delta)
{
    if (!m_library || !canMoveChannels() || delta == 0) return row;
    const QVector<int> &rows = visible();
    const int target = row + (delta > 0 ? 1 : -1);
    if (row < 0 || row >= rows.size() || target < 0 || target >= rows.size()) return row;
    const QString key = m_keys[rows[row]];
    const QString neighbor = m_keys[rows[target]];

    if (m_currentGroup == FavoritesGroup) {
        const QStringList &favs = m_library->favorites();
        m_library->moveFavorite(favs.indexOf(key), favs.indexOf(neighbor));
    } else if (isListGroup(m_currentGroup)) {
        const QString id = m_groupNames[m_currentGroup];
        if (const UserLibrary::List *l = m_library->list(id))
            m_library->moveInList(id, l->keys.indexOf(key), l->keys.indexOf(neighbor));
    } else {
        const QString id = m_groupNames[m_currentGroup];
        QStringList order;
        for (int i : orderedMembers(id)) order << m_keys[i];
        order.removeDuplicates();
        moveNextTo(order, key, neighbor, delta);
        m_library->setChannelOrder(id, order);
    }
    rebuild();
    for (int r = 0; r < visible().size(); ++r)
        if (m_keys[visible()[r]] == key) return r;
    return row;
}

void ChannelModel::sortGroup(int group)
{
    if (!m_library || !canEditGroup(group) || isListGroup(group)) return;
    const QString id = m_groupNames[group];
    QVector<int> list = m_byGroup.value(id);
    std::stable_sort(list.begin(), list.end(), [&](int a, int b) {
        return QString::localeAwareCompare(m_folded[a], m_folded[b]) < 0;
    });
    QStringList keys;
    for (int i : list) keys << m_keys[i];
    keys.removeDuplicates();
    m_library->setChannelOrder(id, keys);
    rebuild();
}

void ChannelModel::resetGroupOrder(int group)
{
    if (!m_library || !canEditGroup(group) || isListGroup(group)) return;
    m_library->setChannelOrder(m_groupNames[group], {});
    rebuild();
}

// ---- search ----------------------------------------------------------------------------

QVariantList ChannelModel::searchChannels(const QString &query, int limit) const
{
    QVariantList out;
    const QString q = fold(query);
    if (q.isEmpty() || m_members.size() <= AllGroup) return out;
    const QStringList words = q.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool isNumber = false;
    const int number = query.trimmed().toInt(&isNumber);

    struct Hit { int score; int order; int index; };
    QVector<Hit> hits;
    const QVector<int> &all = m_members[AllGroup];
    for (int pos = 0; pos < all.size(); ++pos) {
        const int i = all[pos];
        if (m_blocked && m_blocked(i)) continue;   // locked: not found while locked
        if (isNumber && m_all[i].number == number) { hits.append({0, pos, i}); continue; }
        const QString &name = m_folded[i];
        bool match = true;
        for (const QString &w : words)
            if (!name.contains(w)) { match = false; break; }
        if (!match) continue;
        hits.append({name.startsWith(q) ? 1 : 2, pos, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
        return a.score != b.score ? a.score < b.score : a.order < b.order;
    });
    for (int k = 0; k < hits.size() && k < limit; ++k) {
        const Channel &c = m_all[hits[k].index];
        QString now;
        if (m_epg && !c.epgId.isEmpty()) {
            const auto nn = m_epg->nowNext(c.epgId);
            if (nn.hasNow) now = nn.now.title;
        }
        out.append(QVariantMap{
            {QStringLiteral("globalIndex"), hits[k].index},
            {QStringLiteral("number"), c.number},
            {QStringLiteral("name"), c.name},
            {QStringLiteral("group"), labelForGroupId(c.group)},
            {QStringLiteral("logo"), c.logo},
            {QStringLiteral("nowTitle"), now},
        });
    }
    return out;
}

QVariantList ChannelModel::searchProgrammes(const QString &query, int limit) const
{
    QVariantList out;
    if (!m_epg || query.trimmed().size() < 2 || m_members.size() <= AllGroup) return out;

    // Guide channel -> first visible playlist channel using it.
    QHash<QString, int> byEpg;
    for (int i : m_members[AllGroup])
        if (!m_all[i].epgId.isEmpty() && !byEpg.contains(EpgService::baseId(m_all[i].epgId)) && !(m_blocked && m_blocked(i)))
            byEpg.insert(EpgService::baseId(m_all[i].epgId), i);

    const QVector<Programme> found = m_epg->search(query.trimmed(), limit * 3);
    for (Programme p : found) {
        const auto it = byEpg.constFind(p.channel);
        if (it == byEpg.constEnd()) continue;
        const Channel &c = m_all[it.value()];
        // search() applied the playlist's offset; add the channel's own.
        const qint64 own = m_epg->offsetSecs(c.epgId) - m_epg->offsetSecs(QString());
        p.start += own;
        p.stop += own;
        out.append(QVariantMap{
            {QStringLiteral("globalIndex"), it.value()},
            {QStringLiteral("number"), c.number},
            {QStringLiteral("channelName"), c.name},
            {QStringLiteral("logo"), c.logo},
            {QStringLiteral("title"), p.title},
            {QStringLiteral("episode"), p.episode},
            {QStringLiteral("start"), double(p.start) * 1000.0},
            {QStringLiteral("stop"), double(p.stop) * 1000.0},
        });
        if (out.size() >= limit) break;
    }
    return out;
}

QString ChannelModel::keyAt(int globalIndex) const
{
    return globalIndex >= 0 && globalIndex < m_keys.size() ? m_keys[globalIndex] : QString();
}

int ChannelModel::indexOfKey(const QString &key) const
{
    return m_firstByKey.value(key, -1);
}

QVariantMap ChannelModel::info(int globalIndex) const
{
    const Channel *c = channel(globalIndex);
    if (!c) return {};
    QString now;
    if (m_epg && !c->epgId.isEmpty()) {
        const auto nn = m_epg->nowNext(c->epgId);
        if (nn.hasNow) now = nn.now.title;
    }
    return {
        {QStringLiteral("globalIndex"), globalIndex},
        {QStringLiteral("number"), c->number},
        {QStringLiteral("name"), c->name},
        {QStringLiteral("group"), labelForGroupId(c->group)},
        {QStringLiteral("logo"), c->logo},
        {QStringLiteral("nowTitle"), now},
        {QStringLiteral("url"), c->url},
        {QStringLiteral("userAgent"), c->userAgent},
        {QStringLiteral("referrer"), c->referrer},
        {QStringLiteral("key"), keyAt(globalIndex)},
    };
}

QVariantList ChannelModel::listGroup(int group, int limit) const
{
    QVariantList out;
    if (group < 0 || group >= m_members.size()) return out;
    const QVector<int> &members = m_members[group];
    for (int k = 0; k < members.size() && k < limit; ++k) out.append(info(members[k]));
    return out;
}
