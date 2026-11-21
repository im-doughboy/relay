#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrlQuery>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;

// Posters and details from The Movie Database (themoviedb.org), for
// playlists whose movies and series come without them (plain M3U lists,
// and gaps in Xtream or portal catalogues). Needs the user's own TMDB key:
// the short "API key" or the long "API read access token".
//
// Answers are kept on disk (tmdb.json in the cache folder): titles that
// were found for 30 days, titles that weren't for 7.
//
// "This product uses the TMDB API but is not endorsed or certified by TMDB."
class TmdbClient : public QObject
{
    Q_OBJECT

public:
    TmdbClient(const QString &cacheDir, QObject *parent = nullptr);
    ~TmdbClient() override;

    void setKey(const QString &key);
    bool active() const { return !m_key.isEmpty() && !m_rejected; }

    // A playlist title made searchable: "EN - The Matrix (1999) [4K]" ->
    // ("The Matrix", "1999"). `year` is used when the title has none.
    // withNumber: the title with a trailing year-like number kept, to try
    // when the year reading finds nothing ("Blade Runner 2049").
    struct Query { QString title; QString year; QString withNumber; };
    static Query clean(const QString &title, const QString &year = QString());
    // The closest search result: same title (and year, give or take one)
    // first, then same title, then the first result.
    static int bestMatch(const QJsonObject &searchAnswer, const Query &q, bool tv);

    // Poster, year, rating and plot for the grid: {id, poster, year,
    // rating, plot} or {} when nothing matched (or no key). `kind` is
    // "movie" or "series".
    void lookup(const QString &kind, const QString &title, const QString &year, QObject *context,
                std::function<void(const QVariantMap &)> done);
    // Everything for the info screen: lookup() plus backdrop, genre, cast,
    // director, duration, and for series "episodes": {"<season>:<episode>":
    // {title, plot, still}} for the seasons asked for.
    void details(const QString &kind, const QString &title, const QString &year, const QList<int> &seasons,
                 QObject *context, std::function<void(const QVariantMap &)> done);
    // Tries the key: done(ok, message).
    void check(const QString &key, QObject *context, std::function<void(bool, const QString &)> done);

    static QString apiBase();     // RELAY_TMDB_URL overrides (tests)
    static QString imageBase();   // RELAY_TMDB_IMAGE_URL overrides (tests)

signals:
    void keyRejected();

private:
    struct Pending {
        QString path;
        QUrlQuery query;
        QString key;
        QPointer<QObject> context;
        std::function<void(int http, const QJsonObject &)> done;
    };
    void get(const QString &path, QUrlQuery query, QObject *context,
             std::function<void(int http, const QJsonObject &)> done);
    void pump();
    QJsonObject cached(const QString &key, bool *found) const;
    void store(const QString &key, const QJsonObject &value);
    void scheduleSave();
    void save();
    QVariantMap infoFromSearch(const QJsonObject &result, bool tv) const;

    QNetworkAccessManager *m_net;
    QString m_cacheFile;
    QString m_key;
    QString m_language;
    bool m_rejected = false;
    QHash<QString, QJsonObject> m_cache;
    QHash<QString, QList<std::function<void(const QVariantMap &)>>> m_waiting;   // lookups in flight
    QList<Pending> m_queue;
    int m_running = 0;
    QTimer m_saveTimer;
};
