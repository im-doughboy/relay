#pragma once

#include "sources/Channel.h"
#include "sources/XtreamParser.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <QVector>

// One movie or one series in the catalogue.
struct VodItem
{
    QString id;           // Xtream stream_id / series_id; "u:<url>" / "g:<group>/<show>" from an M3U
    QString title;
    QString poster;
    QString categoryId;
    QString year;
    double rating = 0;    // 0..10, 0 = unknown
    qint64 added = 0;     // Unix seconds, for "recently added" sorting
    QString ext;          // movies: container extension
    QString url;          // movies from an M3U playlist: direct URL
};

// Pure parsing for the Xtream Codes VOD API and for movie/series entries
// in M3U playlists. No networking; everything here is unit-tested.
namespace VodParser {

struct Category { QString id; QString name; };
// get_vod_categories / get_series_categories, in the server's order.
QVector<Category> parseCategories(const QByteArray &json);
// True when `json` is a JSON array (an Xtream list), even an empty one.
bool isList(const QByteArray &json);

QVector<VodItem> parseMovies(const QByteArray &json);
QVector<VodItem> parseSeries(const QByteArray &json);

// Details for the info screen. Keys: title, plot, year, genre, rating,
// duration, cast, director, poster, backdrop, (movies) url;
// (series) seasons: [{number, name}], episodes: [{id, season, episode,
// title, plot, duration, poster, url}].
QVariantMap parseMovieInfo(const QByteArray &json, const XtreamParser::Credentials &c,
                           const VodItem &item);
QVariantMap parseSeriesInfo(const QByteArray &json, const XtreamParser::Credentials &c,
                            const VodItem &item);

QString movieUrl(const XtreamParser::Credentials &c, const QString &id, const QString &ext);
QString episodeUrl(const XtreamParser::Credentials &c, const QString &id, const QString &ext);

// Year from "2019-05-03", "2019" or a title like "Film Name (2019)".
QString yearOf(const QString &date, const QString &title = QString());

// ---- Stalker / MAC portals ----
// Ids: movies and series from the portal's movie section keep the portal's
// id; series from its separate series section get "s" in front.
struct PortalPage {
    QVector<VodItem> movies, series;
    // "movie:<id>" / "series:<id>" -> details shown on the info screen, plus
    // what's needed to play: section ("vod"/"series"), movieId, cmd, numbers
    // (episode numbers for series listed on one item).
    QHash<QString, QVariantMap> info;
    int pages = 1;
};
// One page of get_ordered_list. `section` is "vod" or "series"; `account`
// is the PortalClient account; relative picture paths are made absolute
// against `base`.
PortalPage parsePortalPage(const QByteArray &json, const QString &section, const QString &categoryId,
                           const QString &account, const QUrl &base);
// A series' seasons (get_ordered_list with movie_id and season_id=0) or a
// season's episodes (with season_id and episode_id=0): [{id, name, number,
// cmd, numbers (episode numbers on a season)}].
QVariantList parsePortalSeasonList(const QByteArray &json, int *pages = nullptr);
// Episodes in the shape parseSeriesInfo() gives, for one season: from the
// season's episode numbers, or from its own episode list.
QVariantList portalEpisodes(const QString &account, const QString &seriesId, int season,
                            const QString &seasonCmd, const QVariantList &numbersOrEpisodes);

// ---- M3U fallback (playlists that aren't from an Xtream server) ----
struct M3uCatalogue {
    QVector<VodItem> movies;
    QVector<VodItem> series;
    QStringList movieCategories;
    QStringList seriesCategories;
    // series id -> episodes as returned by parseSeriesInfo()["episodes"]
    QHash<QString, QVariantList> episodes;
};
// Splits "Show Name S01 E02" style entries into series with episodes.
M3uCatalogue buildM3uCatalogue(const QVector<Channel> &movies, const QVector<Channel> &episodes);

} // namespace VodParser
