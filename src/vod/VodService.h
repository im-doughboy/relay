#pragma once

#include "VodParser.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class UserLibrary;
class TmdbClient;

// The posters shown in the movies/series grid for the current section,
// category, filter and sort.
class VodModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1, KindRole, TitleRole, PosterRole, YearRole, RatingRole,
        SubtitleRole, ProgressRole, WatchedRole, FavoriteRole,
    };
    struct Row {
        VodItem item;
        QString kind;         // "movie" or "series"
        QString subtitle;     // e.g. "S2 E5 · 23 min left" in Continue watching
        QString progressKey;  // library key whose progress the poster shows
    };

    explicit VodModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    void setLibrary(UserLibrary *library) { m_library = library; }
    void setRows(QVector<Row> rows);
    void refreshMarks();   // progress / favorite changed
    // Poster, year or rating of one item filled in (TMDB).
    void updateItem(const QString &kind, const VodItem &item);
    const Row *row(int i) const { return i >= 0 && i < m_rows.size() ? &m_rows[i] : nullptr; }
    int count() const { return int(m_rows.size()); }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    QVector<Row> m_rows;
    UserLibrary *m_library = nullptr;
};

// Movies and series ("video on demand"). With an Xtream login (typed in, or
// found in a get.php playlist link) the catalogue comes from the server's
// API, cached on disk for a day; otherwise it is built from the movie and
// episode entries in the M3U playlist.
class VodService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(int movieCount READ movieCount NOTIFY stateChanged)
    Q_PROPERTY(int seriesCount READ seriesCount NOTIFY stateChanged)
    // 0 = Movies, 1 = Series
    Q_PROPERTY(int section READ section WRITE setSection NOTIFY sectionChanged)
    // [{id, label, count}]; ids "continue", "favorites", "all", "c:<id>"
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY categoryChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // 0 server order, 1 recently added, 2 A–Z, 3 rating, 4 year
    Q_PROPERTY(int sort READ sort WRITE setSort NOTIFY sortChanged)
    Q_PROPERTY(QStringList sortNames READ sortNames CONSTANT)
    Q_PROPERTY(VodModel *items READ items CONSTANT)

public:
    VodService(UserLibrary *library, const QString &cacheDir, QObject *parent = nullptr);

    bool available() const { return m_mode != Mode::None; }
    bool loading() const { return m_loading; }
    bool loaded() const { return m_loaded; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    int movieCount() const { return int(m_movies.size()); }
    int seriesCount() const { return int(m_series.size()); }
    int section() const { return m_section; }
    void setSection(int s);
    QVariantList categories() const { return m_categoryList; }
    QString category() const { return m_category; }
    void setCategory(const QString &id);
    QString filter() const { return m_filter; }
    void setFilter(const QString &f);
    int sort() const { return m_sort; }
    void setSort(int s);
    QStringList sortNames() const;
    VodModel *items() const { return m_items; }

    // Parental lock: categories that are locked right now (section, id).
    // Their titles are left out of All, search and Continue watching.
    void setBlockedCategoryCheck(std::function<bool(int, const QString &)> check) { m_catBlocked = std::move(check); }
    Q_INVOKABLE void refresh();   // locks changed

    // From the loaded playlist. Called by Session.
    void setSource(const struct PlaylistResult &result);
    void clear();

    Q_INVOKABLE void ensureLoaded();       // load on first use (cache or network)
    Q_INVOKABLE void reload();             // fetch again from the server
    // Fetch details for an item in the grid (row) or by kind/id; answers
    // with detailsReady(details). See VodParser for the keys; added here:
    // kind, id, key, favorite, pos, dur, watched, resumeIndex (series),
    // and per episode key, pos, dur, watched.
    Q_INVOKABLE void requestDetails(const QString &kind, const QString &id);
    Q_INVOKABLE QVariantMap rowAt(int row) const;
    // The browse home's rows for the current section: [{id, label, count, items:
    // [{id, kind, title, poster, year, rating, progress, watched, subtitle}]}],
    // Continue watching, Favorites, Recently added, Top rated, then the
    // provider's categories. id is a category for "See all" (recent/top: all, sorted).
    Q_INVOKABLE QVariantList shelves(int perRow = 24) const;

    // Descriptor for Player.playVod(). `episodeIndex` picks an episode of
    // a series. fromStart ignores saved progress.
    Q_INVOKABLE QVariantMap playback(const QVariantMap &details, int episodeIndex = -1,
                                     bool fromStart = false) const;
    // The episode after the one in `descriptor`, or {} at the end.
    Q_INVOKABLE QVariantMap nextEpisode(const QVariantMap &descriptor) const;
    Q_INVOKABLE void saveProgress(const QVariantMap &descriptor, double position, double duration);
    Q_INVOKABLE void forgetProgress(const QString &key);   // remove from Continue watching
    Q_INVOKABLE void setWatched(const QVariantMap &descriptor, bool watched);
    Q_INVOKABLE bool toggleFavorite(const QString &kind, const QString &id);
    // [{kind, id, title, poster, year}] matching `text`, movies then series.
    Q_INVOKABLE QVariantList search(const QString &text, int limit) const;

    // TMDB (posters and details for titles that come without them).
    void setTmdbKey(const QString &key);
    // Tries a key; answers with tmdbChecked(ok, message).
    Q_INVOKABLE void checkTmdbKey(const QString &key);

    // Trakt: what's watched there shows as watched here (TraktService::parseWatched()).
    void applyTraktWatched(const QVariantMap &watched);
    // Title as compared across services (provider tags removed, folded).
    static QString matchKey(const QString &title);

signals:
    void availableChanged();
    void stateChanged();
    void sectionChanged();
    void categoriesChanged();
    void categoryChanged();
    void filterChanged();
    void sortChanged();
    void detailsReady(const QVariantMap &details);
    void detailsFailed(const QString &kind, const QString &id, const QString &message);
    void tmdbChecked(bool ok, const QString &message);
    void tmdbKeyRejected();
    // Marked watched / not watched by hand (for Trakt).
    void watchedByHand(const QVariantMap &descriptor, bool watched);

private:
    enum class Mode { None, Xtream, M3u, Portal };
    struct Catalogue {
        QVector<VodItem> movies, series;
        QVector<VodParser::Category> movieCats, seriesCats;
        QStringList movieFolded, seriesFolded;
        QHash<QString, QVariantList> m3uEpisodes;
        QHash<QString, QVariantMap> portalInfo;
        QString error;
        bool fromCache = false;
    };

    void loadFromCacheOrNetwork(bool forceNetwork);
    void fetchFromServer(bool quiet);
    void applyCatalogue(Catalogue cat);
    void useM3uFallback(const QString &reason);
    // Stalker / MAC portal: every category's pages, a few at a time.
    void fetchFromPortal(bool quiet);
    static Catalogue portalCatalogueFromJson(const QByteArray &json);
    static QByteArray portalCatalogueToJson(const Catalogue &cat);
    void portalSeriesDetails(const VodItem &item, std::function<void(const QVariantMap &, const QString &)> done);
    void setStatus(const QString &status);
    void setError(const QString &error);
    void setLoading(bool loading);
    void rebuildCategories();
    void rebuild();
    QVector<VodModel::Row> continueRows() const;
    QVector<VodModel::Row> favoriteRows() const;
    const VodItem *find(const QString &kind, const QString &id) const;
    QVariantMap decorate(QVariantMap details, const QString &kind, const VodItem &item) const;
    QString cachePath(const QString &name) const;
    // Details out to the info screen, with TMDB's filled in where missing.
    void publish(const QVariantMap &details);
    // Posters for the grid from TMDB, shown rows first.
    void fillPosters();
    void markTraktMovies();
    bool traktMovieWatched(const VodItem &item) const;
    QHash<QString, QList<int>> m_traktMovies;            // title key -> years
    QHash<QString, QSet<QString>> m_traktEpisodes;       // title key -> "season:episode"
    QNetworkReply *get(const QString &action, const QString &extra = QString());

    UserLibrary *m_library;
    QString m_cacheRoot;
    QNetworkAccessManager *m_net;
    VodModel *m_items;
    TmdbClient *m_tmdb;

    Mode m_mode = Mode::None;
    XtreamParser::Credentials m_creds;
    QString m_account;                 // cache folder for this login
    QString m_portal;                  // PortalClient account (Mode::Portal)
    QVariantMap shelfItem(const VodModel::Row &r) const;
    QHash<QString, std::pair<qint64, QVariantMap>> m_recentDetails;   // "kind:id" -> (when, details)
    QHash<QString, QVariantMap> m_portalInfo;   // "movie:<id>" / "series:<id>" -> details
    QVector<Channel> m_m3uMovies, m_m3uEpisodes;
    quint64 m_generation = 0;

    bool m_loading = false;
    bool m_loaded = false;
    QString m_status, m_error;

    QVector<VodItem> m_movies, m_series;
    QVector<VodParser::Category> m_movieCats, m_seriesCats;
    QStringList m_movieFolded, m_seriesFolded;
    QHash<QString, int> m_movieIndex, m_seriesIndex;
    QHash<QString, QVariantList> m_m3uEpisodeMap;
    QHash<QString, QVariantMap> m_seriesDetails;   // cache, by series id

    int m_section = 0;
    QString m_category = QStringLiteral("all");
    QString m_filter;
    int m_sort = 0;
    QVariantList m_categoryList;
    std::function<bool(int, const QString &)> m_catBlocked;
    bool blockedItem(int section, const VodItem &item) const {
        return m_catBlocked && !item.categoryId.isEmpty() && m_catBlocked(section, item.categoryId);
    }
};
