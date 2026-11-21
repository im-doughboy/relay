#include "VodParser.h"

#include "sources/PortalClient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace {

// Xtream panels mix numbers, strings and nulls for the same field.
QString str(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString()) return v.toString().trimmed();
    if (v.isDouble()) {
        const double d = v.toDouble();
        return d == qint64(d) ? QString::number(qint64(d)) : QString::number(d);
    }
    return QString();
}

double num(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble()) return v.toDouble();
    bool ok = false;
    const double d = v.toString().toDouble(&ok);
    return ok ? d : 0;
}

QString firstOf(const QJsonObject &o, std::initializer_list<const char *> keys)
{
    for (const char *k : keys) {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isArray()) {
            const QJsonArray a = v.toArray();
            if (!a.isEmpty() && a.first().isString() && !a.first().toString().isEmpty())
                return a.first().toString();
            continue;
        }
        const QString s = str(o, k);
        if (!s.isEmpty()) return s;
    }
    return QString();
}

QString durationText(const QJsonObject &info)
{
    qint64 secs = qint64(num(info, "duration_secs"));
    if (secs <= 0) {
        const QString d = str(info, "duration");   // "01:52:30"
        const QStringList p = d.split(QLatin1Char(':'));
        if (p.size() == 3) secs = p[0].toInt() * 3600 + p[1].toInt() * 60 + p[2].toInt();
    }
    if (secs <= 0) return QString();
    const qint64 h = secs / 3600, m = (secs % 3600) / 60;
    return h > 0 ? QStringLiteral("%1 h %2 min").arg(h).arg(m) : QStringLiteral("%1 min").arg(qMax<qint64>(1, m));
}

QString base(const XtreamParser::Credentials &c)
{
    return XtreamParser::normalizeServer(c.server).toString();
}

QString enc(const QString &s) { return QString::fromUtf8(QUrl::toPercentEncoding(s)); }

// Panels often name episodes "Show Name - S01E02 - Title"; keep "Title".
QString cleanEpisodeTitle(QString t, const QString &show)
{
    t = t.trimmed();
    static const QRegularExpression code(
        QStringLiteral("^(.*?)[\\s._\\-|:]*[Ss]\\d{1,2}\\s*[Ee]\\d{1,3}\\b[\\s._\\-|:]*"));
    const auto m = code.match(t);
    if (m.hasMatch()) {
        const QString before = m.captured(1).trimmed();
        // Only drop the part before the code when it's the show's name (or empty).
        if (before.isEmpty() || before.compare(show, Qt::CaseInsensitive) == 0
            || show.contains(before, Qt::CaseInsensitive) || before.contains(show, Qt::CaseInsensitive))
            t = t.mid(m.capturedLength());
    }
    return t.trimmed();
}

} // namespace

namespace VodParser {

QString yearOf(const QString &date, const QString &title)
{
    static const QRegularExpression year(QStringLiteral("\\b(19\\d\\d|20\\d\\d)\\b"));
    auto m = year.match(date);
    if (m.hasMatch()) return m.captured(1);
    m = year.match(title);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString movieUrl(const XtreamParser::Credentials &c, const QString &id, const QString &ext)
{
    return QStringLiteral("%1/movie/%2/%3/%4.%5")
        .arg(base(c), enc(c.username), enc(c.password), id, ext.isEmpty() ? QStringLiteral("mp4") : ext);
}

QString episodeUrl(const XtreamParser::Credentials &c, const QString &id, const QString &ext)
{
    return QStringLiteral("%1/series/%2/%3/%4.%5")
        .arg(base(c), enc(c.username), enc(c.password), id, ext.isEmpty() ? QStringLiteral("mp4") : ext);
}

QVector<Category> parseCategories(const QByteArray &json)
{
    QVector<Category> out;
    QSet<QString> seen;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        Category c{str(o, "category_id"), str(o, "category_name")};
        if (c.id.isEmpty() || seen.contains(c.id)) continue;
        if (c.name.isEmpty()) c.name = c.id;
        seen.insert(c.id);
        out.append(c);
    }
    return out;
}

bool isList(const QByteArray &json)
{
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(json, &err);
    return err.error == QJsonParseError::NoError && d.isArray();
}

QVector<VodItem> parseMovies(const QByteArray &json)
{
    QVector<VodItem> out;
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        VodItem it;
        it.id = str(o, "stream_id");
        if (it.id.isEmpty()) continue;
        it.title = str(o, "name");
        it.poster = str(o, "stream_icon");
        it.categoryId = str(o, "category_id");
        it.rating = num(o, "rating");
        it.added = qint64(num(o, "added"));
        it.ext = str(o, "container_extension");
        it.year = yearOf(str(o, "year") + QLatin1Char(' ') + str(o, "releasedate"), it.title);
        out.append(it);
    }
    return out;
}

QVector<VodItem> parseSeries(const QByteArray &json)
{
    QVector<VodItem> out;
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        VodItem it;
        it.id = str(o, "series_id");
        if (it.id.isEmpty()) continue;
        it.title = str(o, "name");
        it.poster = firstOf(o, {"cover", "stream_icon"});
        it.categoryId = str(o, "category_id");
        it.rating = num(o, "rating");
        it.added = qint64(num(o, "last_modified"));
        it.year = yearOf(firstOf(o, {"releaseDate", "release_date", "year"}), it.title);
        out.append(it);
    }
    return out;
}

// ---- Stalker / MAC portals ---------------------------------------------------------------

static QJsonValue portalJs(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object().value(QLatin1String("js"));
}

static QString absolute(const QString &path, const QUrl &base)
{
    if (path.isEmpty() || path.contains(QLatin1String("://")) || !base.isValid()) return path;
    return base.resolved(QUrl(path.startsWith(QLatin1Char('/')) ? path : QLatin1Char('/') + path)).toString();
}

PortalPage parsePortalPage(const QByteArray &json, const QString &section, const QString &categoryId,
                           const QString &account, const QUrl &base)
{
    PortalPage page;
    const QJsonObject o = portalJs(json).toObject();
    const int total = int(num(o, "total_items"));
    const int per = qMax(1, int(num(o, "max_page_items")));
    page.pages = total > 0 ? (total + per - 1) / per : 1;
    const bool seriesSection = section == QLatin1String("series");
    for (const QJsonValue &v : o.value(QLatin1String("data")).toArray()) {
        const QJsonObject m = v.toObject();
        const QString id = str(m, "id");
        const QString title = firstOf(m, {"name", "o_name"});
        if (id.isEmpty() || title.isEmpty()) continue;
        QVector<int> numbers;
        for (const QJsonValue &n : m.value(QLatin1String("series")).toArray())
            if (n.toInt() > 0 || n.toString().toInt() > 0) numbers.append(n.isDouble() ? n.toInt() : n.toString().toInt());
        const bool isSeries = seriesSection || str(m, "is_series") == QLatin1String("1") || !numbers.isEmpty();

        VodItem it;
        it.id = seriesSection ? QStringLiteral("s") + id : id;
        it.title = title;
        it.poster = absolute(firstOf(m, {"screenshot_uri", "cover_big", "pic", "logo"}), base);
        it.categoryId = categoryId;
        it.year = yearOf(str(m, "year"), title);
        double rating = num(m, "rating_imdb");
        if (rating <= 0) rating = num(m, "rating_kinopoisk");
        it.rating = rating;
        const QDateTime added = QDateTime::fromString(str(m, "added"), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        it.added = added.isValid() ? added.toSecsSinceEpoch() : 0;
        const QString cmd = str(m, "cmd");
        if (!isSeries) it.url = PortalClient::vodUrl(account, cmd);

        const int minutes = int(num(m, "time"));
        QVariantList numberList;
        for (int n : numbers) numberList.append(n);
        QVariantMap info{
            {QStringLiteral("plot"), str(m, "description")},
            {QStringLiteral("genre"), str(m, "genres_str")},
            {QStringLiteral("cast"), str(m, "actors")},
            {QStringLiteral("director"), str(m, "director")},
            {QStringLiteral("duration"), minutes > 0 ? (minutes >= 60 ? QStringLiteral("%1 h %2 min").arg(minutes / 60).arg(minutes % 60)
                                                                      : QStringLiteral("%1 min").arg(minutes))
                                                     : QString()},
            {QStringLiteral("section"), section},
            {QStringLiteral("movieId"), id},
            {QStringLiteral("cmd"), cmd},
            {QStringLiteral("numbers"), numberList},
        };
        if (isSeries) {
            page.series.append(it);
            page.info.insert(QStringLiteral("series:") + it.id, info);
        } else {
            page.movies.append(it);
            page.info.insert(QStringLiteral("movie:") + it.id, info);
        }
    }
    return page;
}

QVariantList parsePortalSeasonList(const QByteArray &json, int *pages)
{
    QVariantList out;
    const QJsonObject o = portalJs(json).toObject();
    const int total = int(num(o, "total_items"));
    const int per = qMax(1, int(num(o, "max_page_items")));
    if (pages) *pages = total > 0 ? (total + per - 1) / per : 1;
    static const QRegularExpression digits(QStringLiteral("(\\d+)"));
    int position = 0;
    for (const QJsonValue &v : o.value(QLatin1String("data")).toArray()) {
        const QJsonObject m = v.toObject();
        const QString id = str(m, "id");
        if (id.isEmpty()) continue;
        ++position;
        const QString name = firstOf(m, {"name", "o_name"});
        int number = int(num(m, "season_number"));
        if (number <= 0) number = int(num(m, "series_number"));
        if (number <= 0) {
            const QRegularExpressionMatch d = digits.match(name);
            number = d.hasMatch() ? d.captured(1).toInt() : position;
        }
        QVariantList numbers;
        for (const QJsonValue &n : m.value(QLatin1String("series")).toArray()) {
            const int k = n.isDouble() ? n.toInt() : n.toString().toInt();
            if (k > 0) numbers.append(k);
        }
        out.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), name},
                               {QStringLiteral("number"), number}, {QStringLiteral("cmd"), str(m, "cmd")},
                               {QStringLiteral("plot"), str(m, "description")},
                               {QStringLiteral("numbers"), numbers}});
    }
    return out;
}

QVariantList portalEpisodes(const QString &account, const QString &seriesId, int season,
                            const QString &seasonCmd, const QVariantList &numbersOrEpisodes)
{
    QVariantList out;
    for (const QVariant &v : numbersOrEpisodes) {
        int number;
        QString cmd = seasonCmd, title, plot;
        int series = 0;
        if (v.typeId() == QMetaType::QVariantMap) {   // an episode of its own
            const QVariantMap e = v.toMap();
            number = e.value(QStringLiteral("number")).toInt();
            cmd = e.value(QStringLiteral("cmd")).toString();
            title = e.value(QStringLiteral("name")).toString();
            plot = e.value(QStringLiteral("plot")).toString();
            const QVariantList inner = e.value(QStringLiteral("numbers")).toList();
            if (cmd.isEmpty()) { cmd = seasonCmd; series = number; }
            else if (!inner.isEmpty()) series = inner.first().toInt();
        } else {                                       // a number on the season
            number = v.toInt();
            series = number;
        }
        if (number <= 0 || cmd.isEmpty()) continue;
        static const QRegularExpression justNumber(QStringLiteral("^(episode|серия|episodio|épisode|folge)?\\s*\\d+$"),
                                                   QRegularExpression::CaseInsensitiveOption);
        if (justNumber.match(title.trimmed()).hasMatch()) title.clear();
        out.append(QVariantMap{
            {QStringLiteral("id"), QStringLiteral("%1:%2:%3").arg(seriesId).arg(season).arg(number)},
            {QStringLiteral("season"), season},
            {QStringLiteral("episode"), number},
            {QStringLiteral("title"), title},
            {QStringLiteral("plot"), plot},
            {QStringLiteral("duration"), QString()},
            {QStringLiteral("poster"), QString()},
            {QStringLiteral("url"), PortalClient::vodUrl(account, cmd, series)},
        });
    }
    std::stable_sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("episode")).toInt() < b.toMap().value(QStringLiteral("episode")).toInt();
    });
    return out;
}

static QVariantMap commonInfo(const QJsonObject &info, const VodItem &item)
{
    QVariantMap d;
    d.insert(QStringLiteral("title"), item.title.isEmpty() ? firstOf(info, {"name", "o_name"}) : item.title);
    d.insert(QStringLiteral("plot"), firstOf(info, {"plot", "description"}));
    d.insert(QStringLiteral("year"), yearOf(firstOf(info, {"releasedate", "releaseDate", "release_date"}),
                                            item.title).isEmpty() ? item.year
                                       : yearOf(firstOf(info, {"releasedate", "releaseDate", "release_date"}), item.title));
    d.insert(QStringLiteral("genre"), str(info, "genre"));
    const double r = num(info, "rating");
    d.insert(QStringLiteral("rating"), r > 0 ? r : item.rating);
    d.insert(QStringLiteral("cast"), firstOf(info, {"cast", "actors"}));
    d.insert(QStringLiteral("director"), str(info, "director"));
    d.insert(QStringLiteral("poster"), firstOf(info, {"movie_image", "cover_big", "cover"}).isEmpty()
                                           ? item.poster : firstOf(info, {"movie_image", "cover_big", "cover"}));
    d.insert(QStringLiteral("backdrop"), firstOf(info, {"backdrop_path"}));
    d.insert(QStringLiteral("duration"), durationText(info));
    return d;
}

QVariantMap parseMovieInfo(const QByteArray &json, const XtreamParser::Credentials &c, const VodItem &item)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject info = root.value(QLatin1String("info")).toObject();
    const QJsonObject data = root.value(QLatin1String("movie_data")).toObject();
    QVariantMap d = commonInfo(info, item);
    QString ext = str(data, "container_extension");
    if (ext.isEmpty()) ext = item.ext;
    d.insert(QStringLiteral("url"), item.url.isEmpty() ? movieUrl(c, item.id, ext) : item.url);
    return d;
}

QVariantMap parseSeriesInfo(const QByteArray &json, const XtreamParser::Credentials &c, const VodItem &item)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QVariantMap d = commonInfo(root.value(QLatin1String("info")).toObject(), item);

    // Episodes come as {"1": [...], "2": [...]} or as a flat array.
    QVariantList episodes;
    QMap<int, QString> seasonNames;
    auto addEpisode = [&](const QJsonObject &e, int seasonHint) {
        const QJsonObject info = e.value(QLatin1String("info")).toObject();
        int season = int(num(e, "season"));
        if (season <= 0) season = seasonHint;
        const QString id = str(e, "id");
        if (id.isEmpty()) return;
        seasonNames.insert(season, QString());
        episodes.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("season"), season},
            {QStringLiteral("episode"), int(num(e, "episode_num"))},
            {QStringLiteral("title"), cleanEpisodeTitle(str(e, "title"), item.title)},
            {QStringLiteral("plot"), str(info, "plot")},
            {QStringLiteral("duration"), durationText(info)},
            {QStringLiteral("poster"), str(info, "movie_image")},
            {QStringLiteral("url"), episodeUrl(c, id, str(e, "container_extension"))},
        });
    };
    const QJsonValue eps = root.value(QLatin1String("episodes"));
    if (eps.isObject()) {
        const QJsonObject bySeason = eps.toObject();
        for (auto it = bySeason.constBegin(); it != bySeason.constEnd(); ++it)
            for (const QJsonValue &e : it.value().toArray()) addEpisode(e.toObject(), it.key().toInt());
    } else {
        for (const QJsonValue &e : eps.toArray()) addEpisode(e.toObject(), 1);
    }
    std::stable_sort(episodes.begin(), episodes.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const int sx = x.value(QStringLiteral("season")).toInt(), sy = y.value(QStringLiteral("season")).toInt();
        if (sx != sy) return sx < sy;
        return x.value(QStringLiteral("episode")).toInt() < y.value(QStringLiteral("episode")).toInt();
    });

    for (const QJsonValue &s : root.value(QLatin1String("seasons")).toArray()) {
        const QJsonObject o = s.toObject();
        const int n = int(num(o, "season_number"));
        if (seasonNames.contains(n)) seasonNames[n] = str(o, "name");
    }
    QVariantList seasons;
    for (auto it = seasonNames.constBegin(); it != seasonNames.constEnd(); ++it)
        seasons.append(QVariantMap{
            {QStringLiteral("number"), it.key()},
            {QStringLiteral("name"), it.value().isEmpty()
                                         ? QStringLiteral("Season %1").arg(it.key()) : it.value()},
        });

    d.insert(QStringLiteral("seasons"), seasons);
    d.insert(QStringLiteral("episodes"), episodes);
    return d;
}

// ---- M3U fallback ------------------------------------------------------------------

M3uCatalogue buildM3uCatalogue(const QVector<Channel> &movies, const QVector<Channel> &episodes)
{
    M3uCatalogue cat;
    QSet<QString> movieCats, seriesCats;

    for (const Channel &c : movies) {
        VodItem it;
        it.id = QStringLiteral("u:") + c.url;   // stable across playlist refreshes
        it.title = c.name;
        it.poster = c.logo;
        it.categoryId = c.group;
        it.url = c.url;
        it.year = yearOf(QString(), c.name);
        cat.movies.append(it);
        if (!movieCats.contains(c.group)) { movieCats.insert(c.group); cat.movieCategories << c.group; }
    }

    // "Show Name S01 E02", "Show Name - S1E2", "Show.Name.S01E02"
    static const QRegularExpression se(
        QStringLiteral("^(.*?)[\\s._\\-]*[Ss](\\d{1,2})[\\s._\\-]*[Ee](\\d{1,3})\\b(.*)$"));
    QHash<QString, int> seriesIndex;   // group + title -> index in cat.series
    for (int i = 0; i < episodes.size(); ++i) {
        const Channel &c = episodes[i];
        QString show = c.name, title;
        int season = 1, episode = i + 1;
        const auto m = se.match(c.name);
        if (m.hasMatch()) {
            show = m.captured(1).trimmed();
            season = m.captured(2).toInt();
            episode = m.captured(3).toInt();
            title = m.captured(4).trimmed();
            while (title.startsWith(QLatin1Char('-')) || title.startsWith(QLatin1Char(':')))
                title = title.mid(1).trimmed();
        }
        if (show.isEmpty()) show = c.name;
        const QString sk = c.group + QLatin1Char('\x1f') + show;
        int idx = seriesIndex.value(sk, -1);
        if (idx < 0) {
            VodItem s;
            s.id = QStringLiteral("g:") + c.group + QLatin1Char('/') + show;
            s.title = show;
            s.poster = c.logo;
            s.categoryId = c.group;
            s.year = yearOf(QString(), show);
            idx = cat.series.size();
            cat.series.append(s);
            seriesIndex.insert(sk, idx);
            if (!seriesCats.contains(c.group)) { seriesCats.insert(c.group); cat.seriesCategories << c.group; }
        }
        cat.episodes[cat.series[idx].id].append(QVariantMap{
            {QStringLiteral("id"), QStringLiteral("u:") + c.url},
            {QStringLiteral("season"), season},
            {QStringLiteral("episode"), episode},
            {QStringLiteral("title"), title},
            {QStringLiteral("plot"), QString()},
            {QStringLiteral("duration"), QString()},
            {QStringLiteral("poster"), c.logo},
            {QStringLiteral("url"), c.url},
        });
    }
    return cat;
}

} // namespace VodParser
