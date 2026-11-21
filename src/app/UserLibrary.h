#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

// The user's own choices about the channel list: favorites (in the order
// added), hidden groups and hidden channels. Stored as JSON in the app data
// folder and keyed by stable channel keys (see ChannelModel::channelKey), so
// they survive playlist refreshes.
class UserLibrary : public QObject
{
    Q_OBJECT

public:
    explicit UserLibrary(const QString &path, QObject *parent = nullptr);
    ~UserLibrary() override;

    const QStringList &favorites() const { return m_favorites; }
    bool isFavorite(const QString &key) const { return m_favoriteSet.contains(key); }
    void setFavorite(const QString &key, bool favorite);

    bool isGroupHidden(const QString &group) const { return m_hiddenGroups.contains(group); }
    void setGroupHidden(const QString &group, bool hidden);
    QStringList hiddenGroups() const;

    bool isChannelHidden(const QString &key) const { return m_hiddenChannels.contains(key); }
    // `label` is remembered so Settings can list the channel even if it
    // later disappears from the playlist.
    void setChannelHidden(const QString &key, bool hidden, const QString &label = QString());
    const QHash<QString, QString> &hiddenChannels() const { return m_hiddenChannels; }

    // Channels watched lately, newest first (at most 30).
    const QStringList &recent() const { return m_recent; }
    void addRecent(const QString &key);

    // Guide listings chosen by hand for a channel (a guide channel id; empty
    // removes the choice) and minutes to move that channel's listings by.
    QString guideId(const QString &key) const { return m_guideIds.value(key); }
    void setGuideId(const QString &key, const QString &id);
    const QHash<QString, QString> &guideIds() const { return m_guideIds; }
    int guideOffset(const QString &key) const { return m_guideOffsets.value(key); }
    void setGuideOffset(const QString &key, int minutes);
    const QHash<QString, int> &guideOffsets() const { return m_guideOffsets; }

    // Favorites order (positions in favorites()).
    void moveFavorite(int from, int to);

    // Extra favorites lists the user made ("Sports", "Kids"…), in order.
    struct List { QString id; QString name; QStringList keys; };
    const QVector<List> &lists() const { return m_lists; }
    const List *list(const QString &id) const;
    QString createList(const QString &name);              // returns its id
    void renameList(const QString &id, const QString &name);
    void removeList(const QString &id);
    void moveList(const QString &id, int delta);
    void setInList(const QString &id, const QString &key, bool in);
    void moveInList(const QString &id, int from, int to);

    // Custom names. An empty name removes the override.
    QString channelName(const QString &key) const { return m_channelNames.value(key); }
    void setChannelName(const QString &key, const QString &name);
    QString groupName(const QString &group) const { return m_groupNames.value(group); }
    void setGroupName(const QString &group, const QString &name);

    // Custom order of the playlist's groups (by original name), and of the
    // channels inside a group (by channel key). Empty = playlist order.
    const QStringList &groupOrder() const { return m_groupOrder; }
    void setGroupOrder(const QStringList &order);
    QStringList channelOrder(const QString &group) const { return m_channelOrder.value(group); }
    void setChannelOrder(const QString &group, const QStringList &keys);

    // ---- Movies and series ----
    // Watch progress keyed "movie:<id>" / "episode:<id>". Each entry holds
    // pos and dur (seconds), t (last watched, ms since epoch) and what's
    // needed to show and resume it without the catalogue: kind, id, title,
    // poster, url, and for episodes seriesId, seriesTitle, season, episode.
    QVariantMap vodProgress(const QString &key) const { return m_vodProgress.value(key); }
    const QHash<QString, QVariantMap> &allVodProgress() const { return m_vodProgress; }
    void setVodProgress(const QString &key, const QVariantMap &entry);
    void removeVodProgress(const QString &key);

    // Favorites keyed "movie:<id>" / "series:<id>", newest first, each with
    // title and poster so the list shows even before the catalogue loads.
    const QStringList &vodFavorites() const { return m_vodFavorites; }
    bool isVodFavorite(const QString &key) const { return m_vodFavoriteMeta.contains(key); }
    QVariantMap vodFavorite(const QString &key) const { return m_vodFavoriteMeta.value(key); }
    void setVodFavorite(const QString &key, bool favorite, const QVariantMap &meta = {});

    // Parental locks: channel keys, playlist group names (original), and
    // movie/series categories ("0:<id>" movies, "1:<id>" series).
    bool isChannelLocked(const QString &key) const { return m_lockedChannels.contains(key); }
    bool isGroupLocked(const QString &group) const { return m_lockedGroups.contains(group); }
    bool isVodCategoryLocked(const QString &key) const { return m_lockedVod.contains(key); }
    void setChannelLocked(const QString &key, bool locked);
    void setGroupLocked(const QString &group, bool locked);
    void setVodCategoryLocked(const QString &key, bool locked);
    int lockCount() const { return int(m_lockedChannels.size() + m_lockedGroups.size() + m_lockedVod.size()); }

    void saveNow();
    // Switch to another playlist's library file (saves the current one first).
    void setPath(const QString &path);
    QString path() const { return m_path; }

signals:
    void changed();

private:
    void load();
    void scheduleSave();

    QString m_path;
    QStringList m_favorites;
    QStringList m_recent;
    QVector<List> m_lists;
    List *listRef(const QString &id);
    QHash<QString, QString> m_guideIds;
    QHash<QString, int> m_guideOffsets;
    QSet<QString> m_favoriteSet;
    QSet<QString> m_hiddenGroups;
    QHash<QString, QString> m_hiddenChannels;   // key -> label
    QHash<QString, QString> m_channelNames;     // key -> custom name
    QHash<QString, QString> m_groupNames;       // original group -> custom name
    QStringList m_groupOrder;
    QHash<QString, QStringList> m_channelOrder; // original group -> keys
    QHash<QString, QVariantMap> m_vodProgress;
    QStringList m_vodFavorites;
    QHash<QString, QVariantMap> m_vodFavoriteMeta;
    QSet<QString> m_lockedChannels, m_lockedGroups, m_lockedVod;
    QTimer m_saveTimer;
    bool m_dirty = false;
};
