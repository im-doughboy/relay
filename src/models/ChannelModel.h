#pragma once

#include "sources/Catchup.h"
#include "sources/Channel.h"

#include <QAbstractListModel>
#include <functional>
#include <QHash>
#include <QTimer>
#include <QVariantList>

class EpgService;
class UserLibrary;

// Holds every channel of the active playlist and exposes the channels of the
// currently selected group to QML.
//
// Groups, in order: Favorites (index 0), Recently watched (1), All channels (2), the
// user's own favorites lists, then the playlist's groups that aren't hidden. Hidden channels and channels in
// hidden groups are left out everywhere except Favorites (where a channel
// only appears if the user put it there).
//
// "Global index" = position in the full playlist; "row" = position in the
// currently shown group.
class ChannelModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)          // [{name}] (display names)
    Q_PROPERTY(QVariantList groupCounts READ groupCounts NOTIFY countsChanged) // [int], same order
    Q_PROPERTY(int currentGroup READ currentGroup WRITE setCurrentGroup NOTIFY currentGroupChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countsChanged)
    Q_PROPERTY(int favoritesGroup READ favoritesGroup CONSTANT)
    Q_PROPERTY(int allGroup READ allGroup CONSTANT)
    Q_PROPERTY(int recentGroup READ recentGroup CONSTANT)
    Q_PROPERTY(QVariantList hiddenGroups READ hiddenGroupsList NOTIFY hiddenChanged)     // [{name, count}]
    Q_PROPERTY(QVariantList hiddenChannels READ hiddenChannelsList NOTIFY hiddenChanged) // [{key, name}]

public:
    enum Roles {
        NumberRole = Qt::UserRole + 1,
        NameRole,
        GroupRole,
        LogoRole,
        GlobalIndexRole,
        NowTitleRole,       // current programme title ("" without guide)
        NowProgressRole,    // 0..1 through the current programme, -1 without guide
        EpgIdRole,          // guide channel id ("" = no guide)
        FavoriteRole,
        CatchupDaysRole,    // days of archive for catch-up, 0 = none
        LockedRole,         // parental lock on the channel or its group
    };

    static constexpr int FavoritesGroup = 0;
    static constexpr int RecentGroup = 1;
    static constexpr int AllGroup = 2;

    explicit ChannelModel(QObject *parent = nullptr);

    // Stable identity for favorites/hiding across playlist refreshes:
    // Xtream-style URLs end in a numeric stream id ("s:12345"); anything
    // else uses the full URL.
    static QString channelKey(const Channel &channel);

    void setLibrary(UserLibrary *library) { m_library = library; }
    void setPlaylist(const PlaylistResult &result);
    const Channel *channel(int globalIndex) const;
    const QVector<Channel> &channels() const { return m_all; }            // with custom names
    const QVector<Channel> &originalChannels() const { return m_original; } // as the playlist had them (for guide matching)

    void setEpg(EpgService *epg) { m_epg = epg; }
    // Guide channel ids, one per channel (from EpgService::resolve).
    void setEpgIds(const QVector<QString> &ids);
    // Catch-up: the Xtream account (for "xc" archives) and archive days by
    // stream id, for playlists whose entries don't say (get.php links).
    void setCatchupAccount(const Catchup::Account &account);
    const Catchup::Account &catchupAccount() const { return m_catchupAccount; }
    int applyArchiveDays(const QHash<QString, int> &daysByStreamId);
    int catchupChannelCount() const;
    void refreshGuideRoles();

    // QAbstractListModel
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QVariantList groups() const;
    QVariantList groupCounts() const;
    int currentGroup() const { return m_currentGroup; }
    void setCurrentGroup(int group);
    int count() const { return rowCount(); }
    int totalCount() const;                   // visible channels (All channels)
    int favoritesGroup() const { return FavoritesGroup; }
    int allGroup() const { return AllGroup; }
    int recentGroup() const { return RecentGroup; }
    // Recently watched: remember a channel as watched. With keepOrder the
    // list on screen stays as it is (zapping inside Recently watched
    // shouldn't reshuffle it) until refreshRecent().
    void noteWatched(int globalIndex, bool keepOrder);
    Q_INVOKABLE void refreshRecent();

    // Groups have an id (the playlist's original name, used for hiding,
    // ordering and zapping) and a label (custom name if the user set one).
    Q_INVOKABLE QString groupName(int group) const;          // id
    Q_INVOKABLE QString groupLabel(int group) const;         // what to show
    QString labelForGroupId(const QString &id) const;
    Q_INVOKABLE int groupIndex(const QString &name) const;   // by id; -1 if not shown
    Q_INVOKABLE int globalIndexAt(int row) const;
    Q_INVOKABLE QString keyAt(int globalIndex) const;          // stable channel key
    // Parental locks (marked, whether or not the PIN has been entered).
    Q_INVOKABLE bool isLocked(int globalIndex) const;
    Q_INVOKABLE bool isGroupLocked(int group) const;
    // True for channels that may not play right now (locked, PIN not entered):
    // zapping skips them and search leaves them out.
    void setBlockedCheck(std::function<bool(int)> blocked) { m_blocked = std::move(blocked); }
    void notifyLocksChanged();
    UserLibrary *library() const { return m_library; }
    Q_INVOKABLE int indexOfKey(const QString &key) const;      // -1 if not in the playlist
    Q_INVOKABLE int rowOf(int globalIndex) const;            // -1 if not in current group
    Q_INVOKABLE int findByNumber(int number) const;          // visible channels only; -1 if none
    Q_INVOKABLE int groupOfChannel(int globalIndex) const;   // its own group, or All channels

    // ---- user choices (persisted through UserLibrary) ----
    Q_INVOKABLE bool isFavorite(int globalIndex) const;
    Q_INVOKABLE bool toggleFavorite(int globalIndex);        // returns the new state
    Q_INVOKABLE void hideChannel(int globalIndex);
    Q_INVOKABLE void unhideChannel(const QString &key);
    Q_INVOKABLE void hideGroup(const QString &name);
    Q_INVOKABLE void unhideGroup(const QString &name);

    // The user's own favorites lists ("Sports", "Kids"…): groups right after
    // All channels. They can be renamed, moved among themselves and deleted.
    Q_INVOKABLE bool isListGroup(int group) const;
    Q_INVOKABLE int createList(const QString &name);          // its group index, -1 if no name
    Q_INVOKABLE void deleteList(int group);
    // [{id, name, member}] for one channel, in list order.
    Q_INVOKABLE QVariantList listsFor(int globalIndex) const;
    Q_INVOKABLE void setInList(int globalIndex, const QString &listId, bool in);
    static bool isListId(const QString &id) { return id.startsWith(QLatin1String("\x1Flist:")); }

    // Renaming: an empty name (or the original) restores the playlist's name.
    Q_INVOKABLE void renameChannel(int globalIndex, const QString &name);
    Q_INVOKABLE void renameGroup(int group, const QString &name);
    Q_INVOKABLE QString originalChannelName(int globalIndex) const;
    Q_INVOKABLE bool canEditGroup(int group) const { return group > AllGroup && group < m_members.size(); }

    // Reordering. Returns the item's new position. Channels can be moved
    // in Favorites and in playlist groups (All channels keeps playlist order).
    Q_INVOKABLE int moveGroup(int group, int delta);
    Q_INVOKABLE int moveChannel(int row, int delta);
    Q_INVOKABLE bool canMoveChannels() const { return m_currentGroup != AllGroup && m_currentGroup != RecentGroup; }
    Q_INVOKABLE void sortGroup(int group);          // A–Z by name
    Q_INVOKABLE void resetGroupOrder(int group);    // back to playlist order

    // Search. Channels: every word must appear in the name (accents and case
    // ignored); an exact channel number ranks first. Programmes: titles
    // airing now or later, airing-now first. Lists of maps for QML.
    Q_INVOKABLE QVariantList searchChannels(const QString &query, int limit = 50) const;
    // One channel as a map (globalIndex, number, name, group, logo, nowTitle,
    // url, userAgent, referrer); {} if out of range.
    Q_INVOKABLE QVariantMap info(int globalIndex) const;
    // The channels of a shown group, in its order, as maps like info().
    Q_INVOKABLE QVariantList listGroup(int group, int limit = 500) const;
    Q_INVOKABLE QVariantList searchProgrammes(const QString &query, int limit = 50) const;
    static QString fold(const QString &text);

    // Next/previous channel within `group`, wrapping around. Used for zapping.
    Q_INVOKABLE int step(int globalIndex, int delta, int group) const;

    QVariantList hiddenGroupsList() const;
    QVariantList hiddenChannelsList() const;

signals:
    void groupsChanged();
    void countsChanged();
    void currentGroupChanged();
    void countChanged();
    void hiddenChanged();
    void channelRenamed(int globalIndex);

private:
    const QVector<int> &visible() const { return m_members[m_currentGroup]; }
    bool isHidden(int globalIndex) const;
    // Recomputes which channels/groups are shown. Keeps the current group
    // (by name) and only resets the view if its contents changed.
    void rebuild();
    void computeGroups(QStringList *names, QVector<QVector<int>> *members) const;
    QStringList orderedGroupIds() const;              // all playlist groups, user order first
    QVector<int> orderedMembers(const QString &group) const;
    void applyNames();

    QVector<Channel> m_all;                  // display copy (custom names applied)
    QVector<Channel> m_original;             // as loaded
    QVector<QString> m_folded;               // fold(name) per channel, for search
    QVector<QString> m_keys;                 // channelKey per global index
    QHash<QString, int> m_firstByKey;        // key -> first global index
    QStringList m_playlistGroups;            // every playlist group, in order
    QHash<QString, QVector<int>> m_byGroup;  // playlist group -> global indexes

    QStringList m_recentKeys;                // Recently watched as shown
    QStringList m_groupNames;                // shown: Favorites, All, groups...
    QVector<QVector<int>> m_members;         // per shown group
    int m_currentGroup = AllGroup;

    UserLibrary *m_library = nullptr;
    EpgService *m_epg = nullptr;
    QTimer m_guideTick;   // keeps "now playing" text and progress current
    Catchup::Account m_catchupAccount;
    std::function<bool(int)> m_blocked;
};
