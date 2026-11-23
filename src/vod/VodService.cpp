#include "app/AppSettings.h"
#include "VodService.h"

#include <QDateTime>

#include "app/Log.h"
#include "app/UserLibrary.h"
#include "sources/PortalClient.h"
#include "Tmdb.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrent>

#include <algorithm>
#include <limits>
#include <memory>
#include <numeric>

namespace {

constexpr qint64 kCacheMaxAgeSecs = 24 * 3600;
constexpr int kTimeoutMs = 120000;
const char *const kLists[4] = {"get_vod_categories", "get_vod_streams",
                               "get_series_categories", "get_series"};

QString fold(const QString &s)
{
    const QString d = s.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d) {
        if (c.category() == QChar::Mark_NonSpacing) continue;
        out.append(c.toLower());
    }
    return out;
}

QStringList foldAll(const QVector<VodItem> &items)
{
    QStringList out;
    out.reserve(items.size());
    for (const VodItem &it : items) out << fold(it.title);
    return out;
}

bool matches(const QString &folded, const QStringList &tokens)
{
    for (const QString &t : tokens)
        if (!folded.contains(t)) return false;
    return true;
}

QStringList tokensOf(const QString &text)
{
    return fold(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

QString minutesLeft(double pos, double dur)
{
    if (dur <= 0) return QCoreApplication::translate("VodService", "%1 min in").arg(qRound(pos / 60));
    const int left = qMax(1, qRound((dur - pos) / 60));
    return QCoreApplication::translate("VodService", "%1 min left").arg(left);
}

QString episodeCode(int season, int episode)
{
    return QStringLiteral("S%1 E%2").arg(season).arg(episode);
}

QVariantMap itemMap(const VodItem &it, const QString &kind)
{
    return {
        {QStringLiteral("kind"), kind},
        {QStringLiteral("id"), it.id},
        {QStringLiteral("title"), it.title},
        {QStringLiteral("poster"), it.poster},
        {QStringLiteral("year"), it.year},
        {QStringLiteral("rating"), it.rating},
        {QStringLiteral("added"), it.added},   // seconds since 1970, 0: the server doesn't say
    };
}

} // namespace

// ---- VodModel ------------------------------------------------------------------

void VodModel::setRows(QVector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    emit countChanged();
}

void VodModel::updateItem(const QString &kind, const VodItem &item)
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].kind != kind || m_rows[i].item.id != item.id) continue;
        m_rows[i].item.poster = item.poster;
        m_rows[i].item.year = item.year;
        m_rows[i].item.rating = item.rating;
        emit dataChanged(index(i), index(i), {PosterRole, YearRole, RatingRole});
    }
}

void VodModel::refreshMarks()
{
    if (m_rows.isEmpty()) return;
    emit dataChanged(index(0), index(int(m_rows.size()) - 1),
                     {ProgressRole, WatchedRole, FavoriteRole, SubtitleRole});
}

int VodModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant VodModel::data(const QModelIndex &index, int role) const
{
    const Row *r = row(index.row());
    if (!r) return {};
    switch (role) {
    case IdRole: return r->item.id;
    case KindRole: return r->kind;
    case TitleRole: return r->item.title;
    case PosterRole: return r->item.poster;
    case YearRole: return r->item.year;
    case RatingRole: return r->item.rating;
    case SubtitleRole: return r->subtitle;
    case ProgressRole:
    case WatchedRole: {
        const QString key = r->progressKey.isEmpty() && r->kind == QLatin1String("movie")
            ? QStringLiteral("movie:") + r->item.id : r->progressKey;
        const QVariantMap p = m_library && !key.isEmpty() ? m_library->vodProgress(key) : QVariantMap();
        const bool watched = p.value(QStringLiteral("watched")).toBool();
        if (role == WatchedRole) return watched;
        const double dur = p.value(QStringLiteral("dur")).toDouble();
        if (watched || dur <= 0) return -1.0;
        return qBound(0.0, p.value(QStringLiteral("pos")).toDouble() / dur, 1.0);
    }
    case FavoriteRole:
        return m_library && m_library->isVodFavorite(r->kind + QLatin1Char(':') + r->item.id);
    }
    return {};
}

QHash<int, QByteArray> VodModel::roleNames() const
{
    return {
        {IdRole, "itemId"}, {KindRole, "kind"}, {TitleRole, "title"}, {PosterRole, "poster"},
        {YearRole, "year"}, {RatingRole, "rating"}, {SubtitleRole, "subtitle"},
        {ProgressRole, "progress"}, {WatchedRole, "watched"}, {FavoriteRole, "favorite"},
    };
}

// ---- VodService ------------------------------------------------------------------

VodService::VodService(UserLibrary *library, const QString &cacheDir, QObject *parent)
    : QObject(parent), m_library(library), m_cacheRoot(cacheDir),
      m_net(new QNetworkAccessManager(this)), m_items(new VodModel(this)),
      m_tmdb(new TmdbClient(cacheDir, this))
{
    m_items->setLibrary(library);
    connect(m_tmdb, &TmdbClient::keyRejected, this, &VodService::tmdbKeyRejected);
    m_combine = QSettings().value(QStringLiteral("vod/combine"), true).toBool();
    // Other playlists' catalogues arrive one by one: merged once they settle.
    m_mergeTimer = new QTimer(this);
    m_mergeTimer->setSingleShot(true);
    m_mergeTimer->setInterval(250);
    connect(m_mergeTimer, &QTimer::timeout, this, [this] {
        merge();
        emit stateChanged();
        rebuildCategories();
        rebuild();
    });
}

// ---- TMDB ----------------------------------------------------------------------------------

void VodService::setTmdbKey(const QString &key)
{
    const bool was = m_tmdb->active();
    m_tmdb->setKey(key);
    if (m_tmdb->active() && !was && m_loaded) fillPosters();
}

void VodService::checkTmdbKey(const QString &key)
{
    m_tmdb->check(key, this, [this](bool ok, const QString &message) { emit tmdbChecked(ok, message); });
}

// ---- Trakt ---------------------------------------------------------------------------------

QString VodService::matchKey(const QString &title)
{
    QString out;
    for (const QChar c : TmdbClient::clean(title).title.normalized(QString::NormalizationForm_KD))
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}

bool VodService::traktMovieWatched(const VodItem &item) const
{
    const auto it = m_traktMovies.constFind(matchKey(item.title));
    if (it == m_traktMovies.constEnd()) return false;
    const int year = TmdbClient::clean(item.title, item.year).year.toInt();
    if (year <= 0) return true;
    for (int y : it.value())
        if (y <= 0 || qAbs(y - year) <= 1) return true;
    return false;
}

void VodService::applyTraktWatched(const QVariantMap &watched)
{
    m_traktMovies.clear();
    m_traktEpisodes.clear();
    for (const QVariant &v : watched.value(QStringLiteral("movies")).toList()) {
        const QVariantMap m = v.toMap();
        m_traktMovies[matchKey(m.value(QStringLiteral("title")).toString())].append(m.value(QStringLiteral("year")).toInt());
    }
    for (const QVariant &v : watched.value(QStringLiteral("episodes")).toList()) {
        const QVariantMap e = v.toMap();
        m_traktEpisodes[matchKey(e.value(QStringLiteral("title")).toString())]
            .insert(QStringLiteral("%1:%2").arg(e.value(QStringLiteral("season")).toInt()).arg(e.value(QStringLiteral("episode")).toInt()));
    }
    markTraktMovies();
}

void VodService::markTraktMovies()
{
    if (m_traktMovies.isEmpty() || m_movies.isEmpty()) return;
    int marked = 0;
    for (const VodItem &it : std::as_const(m_movies)) {
        const QString key = QStringLiteral("movie:") + it.id;
        const QVariantMap p = m_library->vodProgress(key);
        if (p.value(QStringLiteral("watched")).toBool() || !traktMovieWatched(it)) continue;
        QVariantMap entry = itemMap(it, QStringLiteral("movie"));
        entry.remove(QStringLiteral("url"));
        entry.insert(QStringLiteral("pos"), 1);
        entry.insert(QStringLiteral("dur"), 1);
        entry.insert(QStringLiteral("t"), p.isEmpty() ? QDateTime::currentMSecsSinceEpoch() : p.value(QStringLiteral("t")).toLongLong());
        entry.insert(QStringLiteral("watched"), true);
        entry.insert(QStringLiteral("trakt"), true);
        m_library->setVodProgress(key, entry);
        ++marked;
    }
    if (marked > 0) {
        qInfo().noquote() << QStringLiteral("Trakt: marked %1 movies watched").arg(marked);
        m_items->refreshMarks();
    }
}

void VodService::fillPosters()
{
    if (!m_tmdb->active()) return;
    const quint64 gen = m_generation;
    QSet<QString> asked;
    auto ask = [&](const QString &kind, const VodItem &it) {
        const QString key = kind + QLatin1Char(':') + it.id;
        if (!it.poster.isEmpty() || asked.contains(key)) return;
        asked.insert(key);
        const QString id = it.id;
        m_tmdb->lookup(kind, it.title, it.year, this, [this, gen, kind, id](const QVariantMap &info) {
            if (gen != m_generation || info.isEmpty()) return;
            QVector<VodItem> &list = kind == QLatin1String("movie") ? m_movies : m_series;
            const auto &index = kind == QLatin1String("movie") ? m_movieIndex : m_seriesIndex;
            const auto at = index.constFind(id);
            if (at == index.constEnd()) return;
            VodItem &item = list[at.value()];
            const QString poster = info.value(QStringLiteral("poster")).toString();
            if (poster.isEmpty()) return;
            item.poster = poster;
            if (item.year.isEmpty()) item.year = info.value(QStringLiteral("year")).toString();
            if (item.rating <= 0) item.rating = info.value(QStringLiteral("rating")).toDouble();
            m_items->updateItem(kind, item);
        });
    };
    // What's on screen first, then everything else.
    for (int i = 0; i < m_items->count(); ++i)
        if (const VodModel::Row *r = m_items->row(i)) ask(r->kind, r->item);
    for (const VodItem &it : std::as_const(m_movies)) ask(QStringLiteral("movie"), it);
    for (const VodItem &it : std::as_const(m_series)) ask(QStringLiteral("series"), it);
    if (!asked.isEmpty())
        qInfo().noquote() << QStringLiteral("TMDB: looking up %1 titles without a poster").arg(asked.size());
}

void VodService::publish(const QVariantMap &d)
{
    auto send = [this](const QVariantMap &out) {
        // Kept a while: browsing moves across many titles and back.
        if (m_recentDetails.size() > 400) m_recentDetails.clear();
        m_recentDetails.insert(out.value(QStringLiteral("kind")).toString() + QLatin1Char(':') + out.value(QStringLiteral("id")).toString(),
                               {QDateTime::currentSecsSinceEpoch(), out});
        QMetaObject::invokeMethod(this, [this, out] { emit detailsReady(out); }, Qt::QueuedConnection);
    };
    const bool series = d.value(QStringLiteral("kind")).toString() == QLatin1String("series");
    QList<int> seasons;
    bool episodeGaps = false;
    for (const QVariant &v : d.value(QStringLiteral("seasons")).toList())
        if (seasons.size() < 12) seasons << v.toMap().value(QStringLiteral("number")).toInt();
    for (const QVariant &v : d.value(QStringLiteral("episodes")).toList())
        if (v.toMap().value(QStringLiteral("title")).toString().isEmpty() || v.toMap().value(QStringLiteral("plot")).toString().isEmpty())
            { episodeGaps = true; break; }
    const bool gaps = d.value(QStringLiteral("plot")).toString().isEmpty() || d.value(QStringLiteral("poster")).toString().isEmpty()
        || d.value(QStringLiteral("backdrop")).toString().isEmpty() || d.value(QStringLiteral("cast")).toString().isEmpty()
        || (series && episodeGaps);
    if (!m_tmdb->active()) { send(d); return; }
    // Nothing missing: shown now, and TMDB's extras (logo, age rating,
    // writers) follow when they're here (usually from its cache).
    if (!gaps) send(d);

    const quint64 gen = m_generation;
    const QString kind = series ? QStringLiteral("series") : QStringLiteral("movie");
    m_tmdb->details(kind, d.value(QStringLiteral("title")).toString(), d.value(QStringLiteral("year")).toString(),
                    series && episodeGaps ? seasons : QList<int>(), this, [this, gen, d, kind, send, gaps](const QVariantMap &t) {
        if (gen != m_generation) return;
        if (t.isEmpty()) { if (gaps) send(d); return; }
        QVariantMap out = d;
        bool added = false;
        for (const char *k : {"plot", "poster", "backdrop", "genre", "cast", "director", "duration", "year", "trailer",
                              "logo", "certification", "writer"}) {
            const QString key = QLatin1String(k);
            if (out.value(key).toString().isEmpty() && !t.value(key).toString().isEmpty()) { out.insert(key, t.value(key)); added = true; }
        }
        if (!gaps && !added) return;   // already sent, and TMDB had nothing more
        if (out.value(QStringLiteral("rating")).toDouble() <= 0) out.insert(QStringLiteral("rating"), t.value(QStringLiteral("rating")));
        const QVariantMap eps = t.value(QStringLiteral("episodes")).toMap();
        if (!eps.isEmpty()) {
            QVariantList episodes = out.value(QStringLiteral("episodes")).toList();
            for (QVariant &v : episodes) {
                QVariantMap e = v.toMap();
                const QVariantMap m = eps.value(QStringLiteral("%1:%2").arg(e.value(QStringLiteral("season")).toInt())
                                                    .arg(e.value(QStringLiteral("episode")).toInt())).toMap();
                if (m.isEmpty()) continue;
                for (const auto &[mine, theirs] : {std::pair{"title", "title"}, std::pair{"plot", "plot"}, std::pair{"poster", "still"}})
                    if (e.value(QLatin1String(mine)).toString().isEmpty())
                        e.insert(QLatin1String(mine), m.value(QLatin1String(theirs)));
                v = e;
            }
            out.insert(QStringLiteral("episodes"), episodes);
        }
        // The grid gets the poster too.
        const QString id = out.value(QStringLiteral("id")).toString();
        const auto &index = kind == QLatin1String("movie") ? m_movieIndex : m_seriesIndex;
        const auto at = index.constFind(id);
        if (at != index.constEnd()) {
            VodItem &item = (kind == QLatin1String("movie") ? m_movies : m_series)[at.value()];
            if (item.poster.isEmpty() && !out.value(QStringLiteral("poster")).toString().isEmpty()) {
                item.poster = out.value(QStringLiteral("poster")).toString();
                m_items->updateItem(kind, item);
            }
        }
        send(out);
    });
}

QStringList VodService::sortNames() const
{
    return {tr("Default order"), tr("Recently added"), tr("A–Z"), tr("Top rated"), tr("Newest")};
}

void VodService::setSection(int s)
{
    s = qBound(0, s, 1);
    if (s == m_section) return;
    m_section = s;
    emit sectionChanged();
    rebuildCategories();
    // Keep the same kind of category when switching between movies and series.
    if (!m_category.startsWith(QLatin1String("c:"))) rebuild();
    else setCategory(QStringLiteral("all"));
}

void VodService::setCategory(const QString &id)
{
    if (id == m_category) return;
    m_category = id;
    emit categoryChanged();
    rebuild();
}

void VodService::setFilter(const QString &f)
{
    if (f == m_filter) return;
    m_filter = f;
    emit filterChanged();
    rebuild();
}

void VodService::setSort(int s)
{
    s = qBound(0, s, 4);
    if (s == m_sort) return;
    m_sort = s;
    emit sortChanged();
    rebuild();
}

void VodService::setStatus(const QString &status)
{
    if (status == m_status) return;
    m_status = status;
    emit stateChanged();
}

void VodService::setError(const QString &error)
{
    if (error == m_error) return;
    m_error = error;
    emit stateChanged();
}

void VodService::setLoading(bool loading)
{
    if (loading == m_loading) return;
    m_loading = loading;
    emit stateChanged();
}

QString VodService::cachePath(const QString &name) const
{
    return m_cacheRoot + QLatin1Char('/') + m_account + QLatin1Char('/') + name + QStringLiteral(".json");
}

// ---- source -------------------------------------------------------------------------

void VodService::setSource(const PlaylistResult &result)
{
    const bool wasAvailable = available();
    Mode mode = Mode::None;
    XtreamParser::Credentials creds;
    QString account;
    if (!result.portalAccount.isEmpty()) {
        mode = Mode::Portal;
        account = QStringLiteral("portal-") + result.portalAccount;
    } else if (!result.xtreamServer.isEmpty() && !result.xtreamUser.isEmpty()) {
        mode = Mode::Xtream;
        creds.server = result.xtreamServer;
        creds.username = result.xtreamUser;
        creds.password = result.xtreamPassword;
        account = QString::fromLatin1(
            QCryptographicHash::hash((XtreamParser::normalizeServer(creds.server).toString()
                                      + QLatin1Char('\n') + creds.username).toUtf8(),
                                     QCryptographicHash::Sha1).toHex().left(12));
    } else if (!result.vodMovies.isEmpty() || !result.vodEpisodes.isEmpty()) {
        mode = Mode::M3u;
        account = QStringLiteral("m3u");
    }

    const bool same = mode == m_mode && account == m_account
        && (mode != Mode::Xtream || creds.password == m_creds.password);
    m_m3uMovies = result.vodMovies;
    m_m3uEpisodes = result.vodEpisodes;
    if (same && (mode == Mode::Xtream || mode == Mode::Portal) && m_loaded) {
        // Same account, catalogue already here: the playlist entries aren't needed.
        m_m3uMovies.clear();
        m_m3uEpisodes.clear();
        return;
    }

    clear();
    m_mode = mode;
    m_creds = creds;
    m_account = account;
    m_portal = result.portalAccount;
    m_m3uMovies = result.vodMovies;
    m_m3uEpisodes = result.vodEpisodes;
    if (wasAvailable != available()) emit availableChanged();
    if (m_mode == Mode::None) return;
    qInfo().noquote() << (m_mode == Mode::Portal ? QStringLiteral("VOD: using the portal's movies and series")
                          : m_mode == Mode::Xtream
                              ? QStringLiteral("VOD: using the server's movie and series lists")
                              : QStringLiteral("VOD: using the playlist's %1 movie and %2 episode entries")
                                    .arg(m_m3uMovies.size()).arg(m_m3uEpisodes.size()));
    // Have it ready by the time someone opens Movies; the cache makes this cheap.
    QTimer::singleShot(6000, this, &VodService::ensureLoaded);
}

void VodService::clear()
{
    const bool wasAvailable = available();
    ++m_generation;
    m_mode = Mode::None;
    m_creds = {};
    m_account.clear();
    m_m3uMovies.clear();
    m_m3uEpisodes.clear();
    m_movies.clear();
    m_series.clear();
    m_movieCats.clear();
    m_seriesCats.clear();
    m_movieFolded.clear();
    m_seriesFolded.clear();
    m_movieIndex.clear();
    m_seriesIndex.clear();
    m_ownMovies.clear();
    m_ownSeries.clear();
    m_ownMovieCats.clear();
    m_ownSeriesCats.clear();
    m_ownMovieFolded.clear();
    m_ownSeriesFolded.clear();
    m_sourcesOf.clear();
    m_m3uEpisodeMap.clear();
    m_seriesDetails.clear();
    m_portal.clear();
    m_portalInfo.clear();
    m_loaded = false;
    m_loading = false;
    m_status.clear();
    m_error.clear();
    emit stateChanged();
    rebuildCategories();
    rebuild();
    if (wasAvailable != available()) emit availableChanged();
}

void VodService::ensureLoaded()
{
    for (const Extra &e : std::as_const(m_extras)) e.svc->ensureLoaded();
    if (m_mode == Mode::None || m_loaded || m_loading) return;
    loadFromCacheOrNetwork(false);
}

void VodService::reload()
{
    for (const Extra &e : std::as_const(m_extras)) e.svc->reload();
    if (m_mode == Mode::None || m_loading) return;
    m_seriesDetails.clear();
    loadFromCacheOrNetwork(true);
}

void VodService::loadFromCacheOrNetwork(bool forceNetwork)
{
    setError(QString());
    if (m_mode == Mode::M3u) {
        useM3uFallback(QString());
        return;
    }
    if (m_mode == Mode::Portal) {
        const QFileInfo fi(cachePath(QStringLiteral("portal")));
        if (forceNetwork || !fi.exists()) { fetchFromPortal(false); return; }
        setLoading(true);
        setStatus(tr("Loading movies and series…"));
        const quint64 gen = m_generation;
        const bool stale = QDateTime::currentSecsSinceEpoch() - fi.lastModified().toSecsSinceEpoch() > kCacheMaxAgeSecs;
        const QString path = fi.filePath();
        QtConcurrent::run([path] {
            QFile f(path);
            Catalogue cat = f.open(QIODevice::ReadOnly) ? portalCatalogueFromJson(f.readAll()) : Catalogue();
            cat.fromCache = true;
            return cat;
        }).then(this, [this, gen, stale](Catalogue cat) {
            if (gen != m_generation) return;
            if (cat.movies.isEmpty() && cat.series.isEmpty()) { fetchFromPortal(false); return; }
            applyCatalogue(std::move(cat));
            if (stale) fetchFromPortal(true);
        });
        return;
    }

    bool haveCache = !forceNetwork;
    qint64 oldest = std::numeric_limits<qint64>::max();
    for (const char *name : kLists) {
        const QFileInfo fi(cachePath(QString::fromLatin1(name)));
        if (!fi.exists()) { haveCache = false; break; }
        oldest = qMin(oldest, fi.lastModified().toSecsSinceEpoch());
    }
    if (!haveCache) { fetchFromServer(false); return; }

    setLoading(true);
    setStatus(tr("Loading movies and series…"));
    const quint64 gen = m_generation;
    QStringList paths;
    for (const char *name : kLists) paths << cachePath(QString::fromLatin1(name));
    const bool stale = QDateTime::currentSecsSinceEpoch() - oldest > kCacheMaxAgeSecs;
    QtConcurrent::run([paths] {
        Catalogue cat;
        QByteArray data[4];
        for (int i = 0; i < 4; ++i) {
            QFile f(paths[i]);
            if (f.open(QIODevice::ReadOnly)) data[i] = f.readAll();
        }
        cat.movieCats = VodParser::parseCategories(data[0]);
        cat.movies = VodParser::parseMovies(data[1]);
        cat.seriesCats = VodParser::parseCategories(data[2]);
        cat.series = VodParser::parseSeries(data[3]);
        cat.movieFolded = foldAll(cat.movies);
        cat.seriesFolded = foldAll(cat.series);
        cat.fromCache = true;
        return cat;
    }).then(this, [this, gen, stale](Catalogue cat) {
        if (gen != m_generation) return;
        const bool empty = cat.movies.isEmpty() && cat.series.isEmpty();
        if (empty) { fetchFromServer(false); return; }
        applyCatalogue(std::move(cat));
        // The saved copy is good, so the playlist's own entries aren't needed.
        m_m3uMovies.clear();
        m_m3uEpisodes.clear();
        if (stale) fetchFromServer(true);   // refresh in the background
    });
}

QNetworkReply *VodService::get(const QString &action, const QString &extra)
{
    QUrl url = XtreamParser::apiUrl(m_creds, action);
    if (!extra.isEmpty()) {
        QUrlQuery q(url);
        const int eq = extra.indexOf(QLatin1Char('='));
        q.addQueryItem(extra.left(eq), extra.mid(eq + 1));
        url.setQuery(q);
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  AppSettings::effectiveUserAgent());
    req.setTransferTimeout(kTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_net->get(req);
}

void VodService::fetchFromServer(bool quiet)
{
    setLoading(true);
    if (!quiet) setStatus(tr("Loading movies and series…"));
    const quint64 gen = m_generation;

    struct Pending {
        QByteArray data[4];
        QString error[4];
        qint64 received[4] = {0, 0, 0, 0};
        int remaining = 4;
        QElapsedTimer timer;
    };
    auto pending = std::make_shared<Pending>();
    pending->timer.start();

    for (int i = 0; i < 4; ++i) {
        QNetworkReply *reply = get(QString::fromLatin1(kLists[i]));
        if (!quiet) {
            connect(reply, &QNetworkReply::downloadProgress, this, [this, gen, pending, i](qint64 got, qint64) {
                if (gen != m_generation) return;
                pending->received[i] = got;
                const qint64 total = pending->received[0] + pending->received[1]
                    + pending->received[2] + pending->received[3];
                if (total > 512 * 1024)
                    setStatus(tr("Loading movies and series… %1 MB").arg(total / 1048576.0, 0, 'f', 1));
            });
        }
        connect(reply, &QNetworkReply::finished, this, [this, reply, gen, pending, i, quiet] {
            reply->deleteLater();
            if (gen != m_generation) return;
            if (reply->error() != QNetworkReply::NoError) {
                const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                pending->error[i] = http >= 400 ? tr("the server responded with HTTP %1").arg(http)
                                                : reply->errorString();
            } else {
                pending->data[i] = reply->readAll();
                if (!VodParser::isList(pending->data[i])) {
                    pending->error[i] = tr("the server didn't send a list");
                    pending->data[i].clear();
                }
            }
            if (--pending->remaining > 0) return;

            const bool moviesOk = pending->error[1].isEmpty();
            const bool seriesOk = pending->error[3].isEmpty();
            qInfo().noquote() << QStringLiteral("VOD: server lists downloaded in %1 ms (%2 MB)%3")
                                     .arg(pending->timer.elapsed())
                                     .arg((pending->data[1].size() + pending->data[3].size()) / 1048576.0, 0, 'f', 1)
                                     .arg(moviesOk && seriesOk ? QString()
                                          : QStringLiteral(" · problems: movies %1, series %2")
                                                .arg(moviesOk ? QStringLiteral("ok") : pending->error[1],
                                                     seriesOk ? QStringLiteral("ok") : pending->error[3]));
            if (!moviesOk && !seriesOk) {
                const QString why = pending->error[1];
                if (!m_m3uMovies.isEmpty() || !m_m3uEpisodes.isEmpty()) { useM3uFallback(why); return; }
                setLoading(false);
                setStatus(QString());
                if (!quiet) setError(tr("Couldn't load movies and series: %1.").arg(why));
                return;
            }

            // Cache what arrived, then parse off the UI thread.
            QDir().mkpath(m_cacheRoot + QLatin1Char('/') + m_account);
            for (int k = 0; k < 4; ++k) {
                if (!pending->error[k].isEmpty()) continue;
                QSaveFile f(cachePath(QString::fromLatin1(kLists[k])));
                if (f.open(QIODevice::WriteOnly)) { f.write(pending->data[k]); f.commit(); }
            }
            setStatus(tr("Reading movies and series…"));
            QtConcurrent::run([pending] {
                Catalogue cat;
                cat.movieCats = VodParser::parseCategories(pending->data[0]);
                cat.movies = VodParser::parseMovies(pending->data[1]);
                cat.seriesCats = VodParser::parseCategories(pending->data[2]);
                cat.series = VodParser::parseSeries(pending->data[3]);
                cat.movieFolded = foldAll(cat.movies);
                cat.seriesFolded = foldAll(cat.series);
                for (QByteArray &d : pending->data) d.clear();
                return cat;
            }).then(this, [this, gen](Catalogue cat) {
                if (gen != m_generation) return;
                if (cat.movies.isEmpty() && cat.series.isEmpty()
                    && (!m_m3uMovies.isEmpty() || !m_m3uEpisodes.isEmpty())) {
                    useM3uFallback(tr("the server's lists are empty"));
                    return;
                }
                applyCatalogue(std::move(cat));
                m_m3uMovies.clear();       // not needed with the server's lists
                m_m3uEpisodes.clear();
            });
        });
    }
}

void VodService::useM3uFallback(const QString &reason)
{
    if (!reason.isEmpty())
        qInfo().noquote() << "VOD: server lists unavailable (" << reason << "), using the playlist entries";
    setLoading(true);
    setStatus(tr("Reading movies and series…"));
    const quint64 gen = m_generation;
    const QVector<Channel> movies = m_m3uMovies, episodes = m_m3uEpisodes;
    QtConcurrent::run([movies, episodes] {
        const VodParser::M3uCatalogue m = VodParser::buildM3uCatalogue(movies, episodes);
        Catalogue cat;
        cat.movies = m.movies;
        cat.series = m.series;
        for (const QString &c : m.movieCategories) cat.movieCats.append({c, c});
        for (const QString &c : m.seriesCategories) cat.seriesCats.append({c, c});
        cat.m3uEpisodes = m.episodes;
        cat.movieFolded = foldAll(cat.movies);
        cat.seriesFolded = foldAll(cat.series);
        return cat;
    }).then(this, [this, gen](Catalogue cat) {
        if (gen != m_generation) return;
        applyCatalogue(std::move(cat));
    });
}

// ---- Stalker / MAC portal -------------------------------------------------------------------

namespace {
QJsonObject itemJson(const VodItem &it)
{
    return {{"id", it.id}, {"title", it.title}, {"poster", it.poster}, {"cat", it.categoryId}, {"year", it.year},
            {"rating", it.rating}, {"added", double(it.added)}, {"url", it.url}};
}
VodItem itemFromJson(const QJsonObject &o)
{
    VodItem it;
    it.id = o.value(QLatin1String("id")).toString();
    it.title = o.value(QLatin1String("title")).toString();
    it.poster = o.value(QLatin1String("poster")).toString();
    it.categoryId = o.value(QLatin1String("cat")).toString();
    it.year = o.value(QLatin1String("year")).toString();
    it.rating = o.value(QLatin1String("rating")).toDouble();
    it.added = qint64(o.value(QLatin1String("added")).toDouble());
    it.url = o.value(QLatin1String("url")).toString();
    return it;
}
// get_categories: [{id, title}], "*" (all) left out.
QVector<VodParser::Category> portalCategories(const QByteArray &json, const QString &prefix)
{
    QVector<VodParser::Category> out;
    const QJsonValue js = QJsonDocument::fromJson(json).object().value(QLatin1String("js"));
    for (const QJsonValue &v : js.toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonValue idv = o.value(QLatin1String("id"));
        const QString id = idv.isDouble() ? QString::number(idv.toInt()) : idv.toString();
        const QString name = o.value(QLatin1String("title")).toString().trimmed();
        if (id.isEmpty() || id == QLatin1String("*") || name.isEmpty()) continue;
        out.append({prefix + id, name});
    }
    return out;
}
constexpr int kPortalParallel = 6;
constexpr int kPortalMaxPagesPerCategory = 500;
} // namespace

QByteArray VodService::portalCatalogueToJson(const Catalogue &cat)
{
    auto cats = [](const QVector<VodParser::Category> &list) {
        QJsonArray a;
        for (const auto &c : list) a.append(QJsonObject{{"id", c.id}, {"name", c.name}});
        return a;
    };
    auto items = [](const QVector<VodItem> &list) {
        QJsonArray a;
        for (const VodItem &it : list) a.append(itemJson(it));
        return a;
    };
    QJsonObject info;
    for (auto it = cat.portalInfo.begin(); it != cat.portalInfo.end(); ++it)
        info.insert(it.key(), QJsonObject::fromVariantMap(it.value()));
    return QJsonDocument(QJsonObject{{"version", 1}, {"movieCats", cats(cat.movieCats)}, {"seriesCats", cats(cat.seriesCats)},
                                     {"movies", items(cat.movies)}, {"series", items(cat.series)}, {"info", info}})
        .toJson(QJsonDocument::Compact);
}

VodService::Catalogue VodService::portalCatalogueFromJson(const QByteArray &json)
{
    Catalogue cat;
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.value(QLatin1String("version")).toInt() != 1) return cat;
    auto cats = [&o](const char *key) {
        QVector<VodParser::Category> out;
        for (const QJsonValue &v : o.value(QLatin1String(key)).toArray())
            out.append({v.toObject().value(QLatin1String("id")).toString(), v.toObject().value(QLatin1String("name")).toString()});
        return out;
    };
    cat.movieCats = cats("movieCats");
    cat.seriesCats = cats("seriesCats");
    for (const QJsonValue &v : o.value(QLatin1String("movies")).toArray()) cat.movies.append(itemFromJson(v.toObject()));
    for (const QJsonValue &v : o.value(QLatin1String("series")).toArray()) cat.series.append(itemFromJson(v.toObject()));
    const QJsonObject info = o.value(QLatin1String("info")).toObject();
    for (auto it = info.begin(); it != info.end(); ++it) cat.portalInfo.insert(it.key(), it.value().toObject().toVariantMap());
    cat.movieFolded = foldAll(cat.movies);
    cat.seriesFolded = foldAll(cat.series);
    return cat;
}

void VodService::fetchFromPortal(bool quiet)
{
    PortalClient *portal = PortalClient::instance();
    const QString account = m_portal;
    if (portal->serverOf(account).isEmpty()) {
        setLoading(false);
        if (!quiet) setError(tr("The portal isn't signed in yet. Reload the playlist."));
        return;
    }
    setLoading(true);
    if (!quiet) setStatus(tr("Loading movies and series…"));
    const quint64 gen = m_generation;
    const QUrl base = portal->baseOf(account);

    struct Job { QString section, category; int page; };
    struct State {
        Catalogue cat;
        QVector<Job> queue;
        int running = 0, done = 0, total = 0, failed = 0;
        QSet<QString> seenMovies, seenSeries;
        QSet<QString> seriesCatsUsed;
        QElapsedTimer timer;
        QString firstError;
        int categoriesLeft = 2;
    };
    auto st = std::make_shared<State>();
    st->timer.start();

    auto finish = std::make_shared<std::function<void()>>();
    auto pump = std::make_shared<std::function<void()>>();
    *pump = [=] {
        if (gen != m_generation) return;
        while (st->running < kPortalParallel && !st->queue.isEmpty()) {
            const Job job = st->queue.takeFirst();
            ++st->running;
            const QString catId = job.category.mid(1);   // without the section letter
            portal->request(account, {{QStringLiteral("type"), job.section}, {QStringLiteral("action"), QStringLiteral("get_ordered_list")},
                                      {QStringLiteral("category"), catId}, {QStringLiteral("genre"), catId},
                                      {QStringLiteral("p"), QString::number(job.page)}, {QStringLiteral("sortby"), QStringLiteral("added")}},
                            this, [=](const QByteArray &body, const QString &error) {
                if (gen != m_generation) return;
                --st->running;
                ++st->done;
                if (!error.isEmpty()) {
                    ++st->failed;
                    if (st->firstError.isEmpty()) st->firstError = error;
                } else {
                    const VodParser::PortalPage page = VodParser::parsePortalPage(body, job.section, job.category, account, base);
                    for (const VodItem &it : page.movies)
                        if (!st->seenMovies.contains(it.id)) { st->seenMovies.insert(it.id); st->cat.movies.append(it); }
                    for (const VodItem &it : page.series)
                        if (!st->seenSeries.contains(it.id)) {
                            st->seenSeries.insert(it.id);
                            st->cat.series.append(it);
                            st->seriesCatsUsed.insert(it.categoryId);
                        }
                    for (auto it = page.info.begin(); it != page.info.end(); ++it) st->cat.portalInfo.insert(it.key(), it.value());
                    if (job.page == 1)
                        for (int p = 2; p <= qMin(page.pages, kPortalMaxPagesPerCategory); ++p) {
                            st->queue.append({job.section, job.category, p});
                            ++st->total;
                        }
                }
                if (!quiet && st->total > 0)
                    setStatus(tr("Loading movies and series… %1%").arg(qMin(99, 100 * st->done / st->total)));
                if (st->running == 0 && st->queue.isEmpty()) (*finish)();
                else (*pump)();
            }, 60000);
        }
    };
    *finish = [=] {
        if (gen != m_generation) return;
        Catalogue &cat = st->cat;
        // Series that live in the movie section show under their movie categories.
        for (const auto &c : std::as_const(cat.movieCats))
            if (st->seriesCatsUsed.contains(c.id)) cat.seriesCats.append(c);
        QVector<VodParser::Category> movieCats;
        QSet<QString> movieCatsUsed;
        for (const VodItem &it : std::as_const(cat.movies)) movieCatsUsed.insert(it.categoryId);
        for (const auto &c : std::as_const(cat.movieCats))
            if (movieCatsUsed.contains(c.id)) movieCats.append(c);
        cat.movieCats = movieCats;
        qInfo().noquote() << QStringLiteral("VOD: portal lists: %1 pages in %2 s%3")
                                 .arg(st->done).arg(st->timer.elapsed() / 1000.0, 0, 'f', 1)
                                 .arg(st->failed ? QStringLiteral(" · %1 failed (%2)").arg(st->failed).arg(st->firstError) : QString());
        if (cat.movies.isEmpty() && cat.series.isEmpty()) {
            setLoading(false);
            setStatus(QString());
            if (!quiet) setError(st->firstError.isEmpty() ? tr("This portal has no movies or series.")
                                                          : tr("Couldn't load movies and series: %1").arg(st->firstError));
            return;
        }
        QDir().mkpath(m_cacheRoot + QLatin1Char('/') + m_account);
        QSaveFile f(cachePath(QStringLiteral("portal")));
        if (f.open(QIODevice::WriteOnly)) { f.write(portalCatalogueToJson(cat)); f.commit(); }
        cat.movieFolded = foldAll(cat.movies);
        cat.seriesFolded = foldAll(cat.series);
        applyCatalogue(std::move(cat));
    };

    // Categories first (both sections), then page 1 of each.
    for (const QString &section : {QStringLiteral("vod"), QStringLiteral("series")}) {
        portal->request(account, {{QStringLiteral("type"), section}, {QStringLiteral("action"), QStringLiteral("get_categories")}}, this,
                        [=](const QByteArray &body, const QString &error) {
            if (gen != m_generation) return;
            const QString prefix = section == QLatin1String("vod") ? QStringLiteral("v") : QStringLiteral("s");
            const QVector<VodParser::Category> cats = error.isEmpty() ? portalCategories(body, prefix) : QVector<VodParser::Category>();
            if (section == QLatin1String("vod")) st->cat.movieCats = cats;
            else st->cat.seriesCats = cats;
            if (!error.isEmpty() && section == QLatin1String("vod") && st->firstError.isEmpty()) st->firstError = error;
            for (const auto &c : cats) {
                st->queue.append({section, c.id, 1});
                ++st->total;
            }
            if (--st->categoriesLeft > 0) return;
            if (st->queue.isEmpty()) { (*finish)(); return; }
            (*pump)();
        }, 30000);
    }
}

void VodService::portalSeriesDetails(const VodItem &item, std::function<void(const QVariantMap &, const QString &)> done)
{
    const QVariantMap info = m_portalInfo.value(QStringLiteral("series:") + item.id);
    QVariantMap d = info;
    for (const char *k : {"section", "movieId", "cmd", "numbers"}) d.remove(QLatin1String(k));
    const QString account = m_portal;
    // Older portals list the episode numbers on the series itself.
    const QVariantList numbers = info.value(QStringLiteral("numbers")).toList();
    if (!numbers.isEmpty()) {
        d.insert(QStringLiteral("episodes"), VodParser::portalEpisodes(account, item.id, 1, info.value(QStringLiteral("cmd")).toString(), numbers));
        d.insert(QStringLiteral("seasons"), QVariantList{QVariantMap{{QStringLiteral("number"), 1}, {QStringLiteral("name"), tr("Season %1").arg(1)}}});
        done(d, QString());
        return;
    }
    const QString section = info.value(QStringLiteral("section")).toString();
    const QString movieId = info.value(QStringLiteral("movieId")).toString();
    if (section.isEmpty() || movieId.isEmpty()) { done({}, tr("The portal has no episodes listed for this series.")); return; }
    PortalClient *portal = PortalClient::instance();
    const quint64 gen = m_generation;

    struct State { QVariantList seasons, episodes, seasonList; int next = 0; };
    auto st = std::make_shared<State>();
    auto step = std::make_shared<std::function<void()>>();
    *step = [=] {
        if (gen != m_generation) return;
        if (st->next >= st->seasons.size()) {
            if (st->episodes.isEmpty()) { done({}, tr("The portal has no episodes listed for this series.")); return; }
            QVariantMap out = d;
            out.insert(QStringLiteral("episodes"), st->episodes);
            out.insert(QStringLiteral("seasons"), st->seasonList);
            done(out, QString());
            return;
        }
        const QVariantMap season = st->seasons[st->next++].toMap();
        const int number = season.value(QStringLiteral("number")).toInt();
        const QString name = season.value(QStringLiteral("name")).toString();
        const QString cmd = season.value(QStringLiteral("cmd")).toString();
        auto add = [=](const QVariantList &eps) {
            if (eps.isEmpty()) return;
            st->episodes.append(eps);
            st->seasonList.append(QVariantMap{{QStringLiteral("number"), number},
                                              {QStringLiteral("name"), name.isEmpty() ? tr("Season %1").arg(number) : name}});
        };
        const QVariantList numbersOnSeason = season.value(QStringLiteral("numbers")).toList();
        if (!numbersOnSeason.isEmpty()) {
            add(VodParser::portalEpisodes(account, item.id, number, cmd, numbersOnSeason));
            (*step)();
            return;
        }
        // Newer portals: the season lists its episodes.
        portal->request(account, {{QStringLiteral("type"), section}, {QStringLiteral("action"), QStringLiteral("get_ordered_list")},
                                  {QStringLiteral("movie_id"), movieId}, {QStringLiteral("season_id"), season.value(QStringLiteral("id")).toString()},
                                  {QStringLiteral("episode_id"), QStringLiteral("0")}, {QStringLiteral("p"), QStringLiteral("1")}},
                        this, [=](const QByteArray &body, const QString &error) {
            if (gen != m_generation) return;
            if (error.isEmpty()) add(VodParser::portalEpisodes(account, item.id, number, cmd, VodParser::parsePortalSeasonList(body)));
            (*step)();
        });
    };
    portal->request(account, {{QStringLiteral("type"), section}, {QStringLiteral("action"), QStringLiteral("get_ordered_list")},
                              {QStringLiteral("movie_id"), movieId}, {QStringLiteral("season_id"), QStringLiteral("0")},
                              {QStringLiteral("episode_id"), QStringLiteral("0")}, {QStringLiteral("p"), QStringLiteral("1")}},
                    this, [=](const QByteArray &body, const QString &error) {
        if (gen != m_generation) return;
        if (!error.isEmpty()) { done({}, tr("Couldn't load the episodes: %1").arg(error)); return; }
        st->seasons = VodParser::parsePortalSeasonList(body);
        std::stable_sort(st->seasons.begin(), st->seasons.end(), [](const QVariant &a, const QVariant &b) {
            return a.toMap().value(QStringLiteral("number")).toInt() < b.toMap().value(QStringLiteral("number")).toInt();
        });
        (*step)();
    });
}

void VodService::applyCatalogue(Catalogue cat)
{
    m_ownMovies = std::move(cat.movies);
    m_ownSeries = std::move(cat.series);
    m_ownMovieCats = std::move(cat.movieCats);
    m_ownSeriesCats = std::move(cat.seriesCats);
    m_ownMovieFolded = std::move(cat.movieFolded);
    m_ownSeriesFolded = std::move(cat.seriesFolded);
    m_m3uEpisodeMap = std::move(cat.m3uEpisodes);
    m_portalInfo = std::move(cat.portalInfo);
    merge();   // with the other playlists' titles (or just these)
    m_seriesDetails.clear();
    m_loaded = true;
    m_loading = false;
    m_status.clear();
    m_error.clear();
    qInfo().noquote() << QStringLiteral("VOD: %1 movies in %2 categories, %3 series in %4 categories%5")
                             .arg(m_movies.size()).arg(m_movieCats.size())
                             .arg(m_series.size()).arg(m_seriesCats.size())
                             .arg(cat.fromCache ? QStringLiteral(" (saved copy)") : QString());
    emit stateChanged();
    rebuildCategories();
    bool known = !m_category.startsWith(QLatin1String("c:"));
    for (const QVariant &c : std::as_const(m_categoryList))
        if (c.toMap().value(QStringLiteral("id")).toString() == m_category) known = true;
    if (!known) { m_category = QStringLiteral("all"); emit categoryChanged(); }
    rebuild();
    fillPosters();
    markTraktMovies();
}

// ---- categories and the grid ------------------------------------------------------

void VodService::rebuildCategories()
{
    const bool movies = m_section == 0;
    const QVector<VodItem> &items = movies ? m_movies : m_series;
    const QVector<VodParser::Category> &cats = movies ? m_movieCats : m_seriesCats;

    QHash<QString, int> counts;
    for (const VodItem &it : items) ++counts[it.categoryId];

    QVariantList list;
    auto add = [&list](const QString &id, const QString &label, int count) {
        list.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("label"), label},
                                {QStringLiteral("count"), count}});
    };
    const int cont = int(continueRows().size());
    const int favs = int(favoriteRows().size());
    if (cont > 0 || m_category == QLatin1String("continue")) add(QStringLiteral("continue"), tr("Continue watching"), cont);
    if (favs > 0 || m_category == QLatin1String("favorites")) add(QStringLiteral("favorites"), tr("Favorites"), favs);
    add(QStringLiteral("all"), movies ? tr("All movies") : tr("All series"), int(items.size()));
    QSet<QString> listed;
    for (const VodParser::Category &c : cats) {
        const int n = counts.value(c.id);
        listed.insert(c.id);
        if (n > 0) add(QStringLiteral("c:") + c.id, c.name, n);
    }
    int other = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it)
        if (!listed.contains(it.key())) other += it.value();
    if (other > 0) {
        // Items whose category the server didn't list.
        add(QStringLiteral("other"), tr("Other"), other);
    }
    if (list != m_categoryList) {
        m_categoryList = list;
        emit categoriesChanged();
    }
}

QVector<VodModel::Row> VodService::continueRows() const
{
    QVector<VodModel::Row> rows;
    QVector<qint64> times;
    const auto &all = m_library->allVodProgress();
    const bool movies = m_section == 0;
    if (movies) {
        for (auto it = all.cbegin(); it != all.cend(); ++it) {
            const QVariantMap &p = it.value();
            if (p.value(QStringLiteral("kind")).toString() != QLatin1String("movie")
                || p.value(QStringLiteral("watched")).toBool())
                continue;
            VodModel::Row r;
            const QString id = p.value(QStringLiteral("id")).toString();
            if (const VodItem *known = find(QStringLiteral("movie"), id)) r.item = *known;
            else {
                r.item.id = id;
                r.item.title = p.value(QStringLiteral("title")).toString();
                r.item.poster = p.value(QStringLiteral("poster")).toString();
                r.item.year = p.value(QStringLiteral("year")).toString();
            }
            r.kind = QStringLiteral("movie");
            r.progressKey = it.key();
            r.subtitle = minutesLeft(p.value(QStringLiteral("pos")).toDouble(), p.value(QStringLiteral("dur")).toDouble());
            rows.append(r);
            times.append(p.value(QStringLiteral("t")).toLongLong());
        }
    } else {
        // One poster per series, for its most recently watched episode.
        QHash<QString, QString> latest;   // series id -> progress key
        for (auto it = all.cbegin(); it != all.cend(); ++it) {
            const QVariantMap &p = it.value();
            if (p.value(QStringLiteral("kind")).toString() != QLatin1String("episode")) continue;
            const QString sid = p.value(QStringLiteral("seriesId")).toString();
            const QString cur = latest.value(sid);
            if (cur.isEmpty() || all.value(cur).value(QStringLiteral("t")).toLongLong()
                                     < p.value(QStringLiteral("t")).toLongLong())
                latest.insert(sid, it.key());
        }
        for (auto it = latest.cbegin(); it != latest.cend(); ++it) {
            const QVariantMap p = all.value(it.value());
            if (p.value(QStringLiteral("watched")).toBool()) continue;   // finished, nothing queued
            VodModel::Row r;
            if (const VodItem *known = find(QStringLiteral("series"), it.key())) r.item = *known;
            else {
                r.item.id = it.key();
                r.item.title = p.value(QStringLiteral("seriesTitle")).toString();
                r.item.poster = p.value(QStringLiteral("seriesPoster")).toString();
            }
            r.kind = QStringLiteral("series");
            r.progressKey = it.value();
            const QString code = episodeCode(p.value(QStringLiteral("season")).toInt(),
                                             p.value(QStringLiteral("episode")).toInt());
            r.subtitle = p.value(QStringLiteral("upNext")).toBool()
                ? tr("Next: %1").arg(code)
                : code + QStringLiteral(" · ") + minutesLeft(p.value(QStringLiteral("pos")).toDouble(),
                                                             p.value(QStringLiteral("dur")).toDouble());
            rows.append(r);
            times.append(p.value(QStringLiteral("t")).toLongLong());
        }
    }
    QVector<int> order(rows.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&times](int a, int b) { return times[a] > times[b]; });
    QVector<VodModel::Row> sorted;
    sorted.reserve(rows.size());
    for (int i : order) sorted.append(rows[i]);
    return sorted;
}

QVector<VodModel::Row> VodService::favoriteRows() const
{
    QVector<VodModel::Row> rows;
    const QString kind = m_section == 0 ? QStringLiteral("movie") : QStringLiteral("series");
    const QString prefix = kind + QLatin1Char(':');
    for (const QString &key : m_library->vodFavorites()) {
        if (!key.startsWith(prefix)) continue;
        const QString id = key.mid(prefix.size());
        VodModel::Row r;
        if (const VodItem *known = find(kind, id)) r.item = *known;
        else {
            // Not in the catalogue (yet): show what was saved with the favorite.
            const QVariantMap meta = m_library->vodFavorite(key);
            r.item.id = id;
            r.item.title = meta.value(QStringLiteral("title")).toString();
            r.item.poster = meta.value(QStringLiteral("poster")).toString();
            r.item.year = meta.value(QStringLiteral("year")).toString();
        }
        r.kind = kind;
        rows.append(r);
    }
    return rows;
}

void VodService::refresh()
{
    rebuildCategories();
    rebuild();
}

void VodService::rebuild()
{
    const bool movies = m_section == 0;
    const QString kind = movies ? QStringLiteral("movie") : QStringLiteral("series");
    const QStringList tokens = tokensOf(m_filter);
    QVector<VodModel::Row> rows;

    if (m_category == QLatin1String("continue") || m_category == QLatin1String("favorites")) {
        const QVector<VodModel::Row> all = m_category == QLatin1String("continue") ? continueRows() : favoriteRows();
        for (const VodModel::Row &r : all)
            if ((tokens.isEmpty() || matches(fold(r.item.title), tokens)) && !blockedItem(m_section, r.item))
                rows.append(r);
        m_items->setRows(std::move(rows));
        return;
    }

    const QVector<VodItem> &items = movies ? m_movies : m_series;
    const QStringList &folded = movies ? m_movieFolded : m_seriesFolded;
    QSet<QString> listed;
    if (m_category == QLatin1String("other"))
        for (const VodParser::Category &c : movies ? m_movieCats : m_seriesCats) listed.insert(c.id);
    const QString cat = m_category.startsWith(QLatin1String("c:")) ? m_category.mid(2) : QString();

    QVector<int> idx;
    idx.reserve(items.size());
    for (int i = 0; i < items.size(); ++i) {
        const VodItem &it = items[i];
        if (!cat.isEmpty() && it.categoryId != cat) continue;
        if (blockedItem(m_section, it)) continue;   // locked category
        if (m_category == QLatin1String("other") && listed.contains(it.categoryId)) continue;
        if (!tokens.isEmpty() && !matches(folded[i], tokens)) continue;
        idx.append(i);
    }
    switch (m_sort) {
    case 1: std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return items[a].added > items[b].added; }); break;
    case 2: std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return folded[a] < folded[b]; }); break;
    case 3: std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return items[a].rating > items[b].rating; }); break;
    case 4: std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return items[a].year > items[b].year; }); break;
    default: break;
    }
    rows.reserve(idx.size());
    for (int i : idx) rows.append(VodModel::Row{items[i], kind, QString(), QString()});
    m_items->setRows(std::move(rows));
}

QVariantMap VodService::shelfItem(const VodModel::Row &r) const
{
    QVariantMap m = itemMap(r.item, r.kind);
    m.insert(QStringLiteral("subtitle"), r.subtitle);
    m.insert(QStringLiteral("progressKey"), r.progressKey);
    const QString key = r.progressKey.isEmpty() && r.kind == QLatin1String("movie")
        ? QStringLiteral("movie:") + r.item.id : r.progressKey;
    const QVariantMap p = m_library && !key.isEmpty() ? m_library->vodProgress(key) : QVariantMap();
    const double dur = p.value(QStringLiteral("dur")).toDouble();
    const bool watched = p.value(QStringLiteral("watched")).toBool();
    m.insert(QStringLiteral("watched"), watched);
    m.insert(QStringLiteral("progress"), watched || dur <= 0 ? -1.0 : qBound(0.0, p.value(QStringLiteral("pos")).toDouble() / dur, 1.0));
    return m;
}

QVariantList VodService::shelves(int perRow) const
{
    const bool movies = m_section == 0;
    const QString kind = movies ? QStringLiteral("movie") : QStringLiteral("series");
    const QVector<VodItem> &items = movies ? m_movies : m_series;
    const QVector<VodParser::Category> &cats = movies ? m_movieCats : m_seriesCats;
    perRow = qBound(4, perRow, 60);
    QVariantList out;
    auto shelf = [&](const QString &id, const QString &label, int count, const QVariantList &list) {
        if (!list.isEmpty())
            out.append(QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("label"), label},
                                   {QStringLiteral("count"), count}, {QStringLiteral("items"), list}});
    };
    auto fromRows = [&](const QVector<VodModel::Row> &rows) {
        QVariantList list;
        for (const VodModel::Row &r : rows) {
            if (blockedItem(m_section, r.item)) continue;
            list.append(shelfItem(r));
            if (list.size() >= perRow) break;
        }
        return list;
    };
    const QVector<VodModel::Row> cont = continueRows(), favs = favoriteRows();
    shelf(QStringLiteral("continue"), tr("Continue watching"), int(cont.size()), fromRows(cont));
    shelf(QStringLiteral("favorites"), tr("Favorites"), int(favs.size()), fromRows(favs));

    auto topBy = [&](auto better, auto keep) {
        QVector<int> idx;
        for (int i = 0; i < items.size(); ++i) if (keep(items[i]) && !blockedItem(m_section, items[i])) idx.append(i);
        const int n = qMin<int>(perRow, int(idx.size()));
        std::partial_sort(idx.begin(), idx.begin() + n, idx.end(), [&](int a, int b) { return better(items[a], items[b]); });
        QVector<VodModel::Row> rows;
        for (int i = 0; i < n; ++i) rows.append(VodModel::Row{items[idx[i]], kind, QString(), QString()});
        return rows;
    };
    // Recently added (when the server says when), and top rated.
    shelf(QStringLiteral("recent"), tr("Recently added"), int(items.size()),
          fromRows(topBy([](const VodItem &a, const VodItem &b) { return a.added > b.added; },
                         [](const VodItem &it) { return it.added > 0; })));
    shelf(QStringLiteral("top"), tr("Top rated"), int(items.size()),
          fromRows(topBy([](const VodItem &a, const VodItem &b) { return a.rating > b.rating; },
                         [](const VodItem &it) { return it.rating > 0; })));

    // Then each of the provider's categories, in its order.
    QHash<QString, QVector<VodModel::Row>> byCat;
    QHash<QString, int> counts;
    for (const VodItem &it : items) {
        int &n = counts[it.categoryId];
        ++n;
        QVector<VodModel::Row> &rows = byCat[it.categoryId];
        if (rows.size() < perRow && !blockedItem(m_section, it)) rows.append(VodModel::Row{it, kind, QString(), QString()});
    }
    for (const VodParser::Category &c : cats)
        if (counts.value(c.id) > 0) shelf(QStringLiteral("c:") + c.id, c.name, counts.value(c.id), fromRows(byCat.value(c.id)));
    return out;
}

QVariantMap VodService::rowAt(int row) const
{
    const VodModel::Row *r = m_items->row(row);
    if (!r) return {};
    QVariantMap m = itemMap(r->item, r->kind);
    m.insert(QStringLiteral("subtitle"), r->subtitle);
    m.insert(QStringLiteral("progressKey"), r->progressKey);
    return m;
}

const VodItem *VodService::find(const QString &kind, const QString &id) const
{
    if (kind == QLatin1String("movie")) {
        const int i = m_movieIndex.value(id, -1);
        return i >= 0 ? &m_movies[i] : nullptr;
    }
    const int i = m_seriesIndex.value(id, -1);
    return i >= 0 ? &m_series[i] : nullptr;
}

QVariantList VodService::search(const QString &text, int limit) const
{
    QVariantList out;
    const QStringList tokens = tokensOf(text);
    if (tokens.isEmpty() || !m_loaded) return out;
    for (int pass = 0; pass < 2; ++pass) {
        const QVector<VodItem> &items = pass == 0 ? m_movies : m_series;
        const QStringList &folded = pass == 0 ? m_movieFolded : m_seriesFolded;
        const QString kind = pass == 0 ? QStringLiteral("movie") : QStringLiteral("series");
        int n = 0;
        for (int i = 0; i < items.size() && n < limit; ++i) {
            if (!matches(folded[i], tokens) || blockedItem(pass, items[i])) continue;
            out.append(itemMap(items[i], kind));
            ++n;
        }
    }
    return out;
}

// ---- details ------------------------------------------------------------------------

QVariantMap VodService::decorate(QVariantMap d, const QString &kind, const VodItem &item) const
{
    d.insert(QStringLiteral("kind"), kind);
    d.insert(QStringLiteral("id"), item.id);
    const QString key = kind + QLatin1Char(':') + item.id;
    d.insert(QStringLiteral("key"), key);
    d.insert(QStringLiteral("favorite"), m_library->isVodFavorite(key));
    if (d.value(QStringLiteral("title")).toString().isEmpty()) d.insert(QStringLiteral("title"), item.title);
    if (d.value(QStringLiteral("poster")).toString().isEmpty()) d.insert(QStringLiteral("poster"), item.poster);
    if (d.value(QStringLiteral("year")).toString().isEmpty()) d.insert(QStringLiteral("year"), item.year);

    if (kind == QLatin1String("movie")) {
        const QVariantMap p = m_library->vodProgress(key);
        d.insert(QStringLiteral("pos"), p.value(QStringLiteral("pos")).toDouble());
        d.insert(QStringLiteral("dur"), p.value(QStringLiteral("dur")).toDouble());
        d.insert(QStringLiteral("watched"), p.value(QStringLiteral("watched")).toBool());
        return d;
    }

    // Episodes watched on Trakt (shown as watched; Relay's own progress wins).
    const QSet<QString> onTrakt = m_traktEpisodes.value(matchKey(d.value(QStringLiteral("title")).toString()));
    QVariantList episodes = d.value(QStringLiteral("episodes")).toList();
    int latest = -1, traktLast = -1;
    qint64 latestT = -1;
    for (int i = 0; i < episodes.size(); ++i) {
        QVariantMap e = episodes[i].toMap();
        const QString ekey = QStringLiteral("episode:") + e.value(QStringLiteral("id")).toString();
        const QVariantMap p = m_library->vodProgress(ekey);
        e.insert(QStringLiteral("key"), ekey);
        e.insert(QStringLiteral("pos"), p.value(QStringLiteral("pos")).toDouble());
        e.insert(QStringLiteral("dur"), p.value(QStringLiteral("dur")).toDouble());
        const bool traktWatched = p.isEmpty()
            && onTrakt.contains(QStringLiteral("%1:%2").arg(e.value(QStringLiteral("season")).toInt()).arg(e.value(QStringLiteral("episode")).toInt()));
        e.insert(QStringLiteral("watched"), p.value(QStringLiteral("watched")).toBool() || traktWatched);
        if (traktWatched && latest < 0) traktLast = i;
        if (!p.isEmpty() && p.value(QStringLiteral("t")).toLongLong() > latestT) {
            latestT = p.value(QStringLiteral("t")).toLongLong();
            latest = i;
        }
        episodes[i] = e;
    }
    int resume = 0;
    if (latest < 0 && traktLast >= 0) resume = qMin(traktLast + 1, int(episodes.size()) - 1);   // after the last one watched on Trakt
    if (latest >= 0) {
        resume = latest;
        if (episodes[latest].toMap().value(QStringLiteral("watched")).toBool() && latest + 1 < episodes.size())
            resume = latest + 1;
    }
    d.insert(QStringLiteral("episodes"), episodes);
    d.insert(QStringLiteral("resumeIndex"), episodes.isEmpty() ? -1 : resume);
    d.insert(QStringLiteral("started"), latest >= 0);
    return d;
}

void VodService::requestDetails(const QString &kind, const QString &id)
{
    // A title only another playlist has: that playlist's details.
    if (id.startsWith(QLatin1Char('@'))) {
        const QString pid = id.mid(1).section(QLatin1Char(':'), 0, 0), theirs = id.mid(1).section(QLatin1Char(':'), 1);
        for (int i = 0; i < m_extras.size(); ++i) {
            if (m_extras[i].playlistId != pid) continue;
            m_extraPages.insert(QString::number(i) + QLatin1Char('|') + kind + QLatin1Char(':') + theirs);
            m_extras[i].svc->requestDetails(kind, theirs);
            return;
        }
        QMetaObject::invokeMethod(this, [this, kind, id] {
            emit detailsFailed(kind, id, tr("That playlist's movies and series aren't loaded"));
        }, Qt::QueuedConnection);
        return;
    }
    VodItem item;
    if (const VodItem *known = find(kind, id)) item = *known;
    else {
        // e.g. opened from Continue watching before the catalogue loaded
        item.id = id;
        if (id.startsWith(QLatin1String("u:"))) item.url = id.mid(2);   // playlist entries are keyed by address
        const QVariantMap fav = m_library->vodFavorite(kind + QLatin1Char(':') + id);
        item.title = fav.value(QStringLiteral("title")).toString();
        item.poster = fav.value(QStringLiteral("poster")).toString();
        for (const QVariantMap &p : m_library->allVodProgress()) {
            if (kind == QLatin1String("movie") && p.value(QStringLiteral("id")).toString() == id) {
                item.title = p.value(QStringLiteral("title")).toString();
                item.poster = p.value(QStringLiteral("poster")).toString();
                if (p.contains(QStringLiteral("url"))) item.url = p.value(QStringLiteral("url")).toString();   // older saves
                break;
            }
            if (kind == QLatin1String("series") && p.value(QStringLiteral("seriesId")).toString() == id) {
                item.title = p.value(QStringLiteral("seriesTitle")).toString();
                item.poster = p.value(QStringLiteral("seriesPoster")).toString();
                break;
            }
        }
    }

    // Seen in the last half hour: the provider's and TMDB's part again, with
    // progress, favorite and watched marks as they are now.
    const auto recent = m_recentDetails.constFind(kind + QLatin1Char(':') + id);
    if (recent != m_recentDetails.constEnd() && QDateTime::currentSecsSinceEpoch() - recent->first < 30 * 60) {
        const QVariantMap d = decorate(recent->second, kind, item);
        QMetaObject::invokeMethod(this, [this, d] { emit detailsReady(d); }, Qt::QueuedConnection);
        return;
    }

    auto deliver = [this](const QVariantMap &d) { publish(d); };

    if (m_mode == Mode::Portal) {
        if (kind == QLatin1String("movie")) {
            QVariantMap d = m_portalInfo.value(QStringLiteral("movie:") + id);
            for (const char *k : {"section", "movieId", "cmd", "numbers"}) d.remove(QLatin1String(k));
            d.insert(QStringLiteral("url"), item.url);
            deliver(decorate(d, kind, item));
            return;
        }
        if (m_seriesDetails.contains(id)) { deliver(decorate(m_seriesDetails.value(id), kind, item)); return; }
        portalSeriesDetails(item, [this, item, kind, id](const QVariantMap &d, const QString &error) {
            if (!error.isEmpty()) { emit detailsFailed(kind, id, error); return; }
            m_seriesDetails.insert(id, d);
            publish(decorate(d, kind, item));
        });
        return;
    }
    const bool m3u = m_mode == Mode::M3u || id.startsWith(QLatin1String("u:")) || id.startsWith(QLatin1String("g:"));
    if (kind == QLatin1String("movie")) {
        if (m3u || m_mode != Mode::Xtream) {
            deliver(decorate(QVariantMap{{QStringLiteral("url"), item.url}}, kind, item));
            return;
        }
        const quint64 gen = m_generation;
        QNetworkReply *reply = get(QStringLiteral("get_vod_info"), QStringLiteral("vod_id=") + id);
        connect(reply, &QNetworkReply::finished, this, [this, reply, gen, item, kind] {
            reply->deleteLater();
            if (gen != m_generation) return;
            QVariantMap d;
            if (reply->error() == QNetworkReply::NoError)
                d = VodParser::parseMovieInfo(reply->readAll(), m_creds, item);
            else {
                // Details are a nicety; the movie still plays without them.
                qInfo().noquote() << "VOD: movie details unavailable:" << reply->errorString();
                d.insert(QStringLiteral("url"), item.url.isEmpty()
                                                    ? VodParser::movieUrl(m_creds, item.id, item.ext) : item.url);
            }
            publish(decorate(d, kind, item));
        });
        return;
    }

    if (m_seriesDetails.contains(id)) { deliver(decorate(m_seriesDetails.value(id), kind, item)); return; }
    if (m3u || m_mode != Mode::Xtream) {
        QVariantList episodes = m_m3uEpisodeMap.value(id);
        std::stable_sort(episodes.begin(), episodes.end(), [](const QVariant &a, const QVariant &b) {
            const QVariantMap x = a.toMap(), y = b.toMap();
            const int sx = x.value(QStringLiteral("season")).toInt(), sy = y.value(QStringLiteral("season")).toInt();
            return sx != sy ? sx < sy : x.value(QStringLiteral("episode")).toInt() < y.value(QStringLiteral("episode")).toInt();
        });
        QVariantList seasons;
        int last = -1;
        for (const QVariant &e : std::as_const(episodes)) {
            const int s = e.toMap().value(QStringLiteral("season")).toInt();
            if (s == last) continue;
            last = s;
            seasons.append(QVariantMap{{QStringLiteral("number"), s}, {QStringLiteral("name"), tr("Season %1").arg(s)}});
        }
        QVariantMap d{{QStringLiteral("episodes"), episodes}, {QStringLiteral("seasons"), seasons}};
        m_seriesDetails.insert(id, d);
        deliver(decorate(d, kind, item));
        return;
    }
    const quint64 gen = m_generation;
    QNetworkReply *reply = get(QStringLiteral("get_series_info"), QStringLiteral("series_id=") + id);
    connect(reply, &QNetworkReply::finished, this, [this, reply, gen, item, kind, id] {
        reply->deleteLater();
        if (gen != m_generation) return;
        if (reply->error() != QNetworkReply::NoError) {
            emit detailsFailed(kind, id, tr("Couldn't load the episodes: %1").arg(reply->errorString()));
            return;
        }
        const QVariantMap d = VodParser::parseSeriesInfo(reply->readAll(), m_creds, item);
        if (d.value(QStringLiteral("episodes")).toList().isEmpty()) {
            emit detailsFailed(kind, id, tr("The server has no episodes listed for this series."));
            return;
        }
        m_seriesDetails.insert(id, d);
        publish(decorate(d, kind, item));
    });
}

// ---- playback and progress ------------------------------------------------------------

QVariantMap VodService::playback(const QVariantMap &details, int episodeIndex, bool fromStart) const
{
    const QString kind = details.value(QStringLiteral("kind")).toString();
    auto startAt = [fromStart](const QVariantMap &m) {
        if (fromStart || m.value(QStringLiteral("watched")).toBool()) return 0.0;
        return qMax(0.0, m.value(QStringLiteral("pos")).toDouble() - 5);   // a few seconds of context
    };
    if (kind == QLatin1String("movie")) {
        const QString year = details.value(QStringLiteral("year")).toString();
        return {
            {QStringLiteral("kind"), QStringLiteral("movie")},
            {QStringLiteral("id"), details.value(QStringLiteral("id"))},
            {QStringLiteral("key"), details.value(QStringLiteral("key"))},
            {QStringLiteral("url"), details.value(QStringLiteral("url"))},
            {QStringLiteral("title"), details.value(QStringLiteral("title"))},
            {QStringLiteral("subtitle"), year},
            {QStringLiteral("year"), year},
            {QStringLiteral("poster"), details.value(QStringLiteral("poster"))},
            {QStringLiteral("start"), startAt(details)},
        };
    }
    const QVariantList episodes = details.value(QStringLiteral("episodes")).toList();
    if (episodeIndex < 0) episodeIndex = details.value(QStringLiteral("resumeIndex")).toInt();
    if (episodeIndex < 0 || episodeIndex >= episodes.size()) return {};
    const QVariantMap e = episodes[episodeIndex].toMap();
    const int season = e.value(QStringLiteral("season")).toInt();
    const int number = e.value(QStringLiteral("episode")).toInt();
    const QString title = e.value(QStringLiteral("title")).toString();
    QString subtitle = episodeCode(season, number);
    if (!title.isEmpty() && !title.contains(QStringLiteral("E%1").arg(number, 2, 10, QLatin1Char('0'))))
        subtitle += QStringLiteral(" · ") + title;
    const QString poster = e.value(QStringLiteral("poster")).toString();
    return {
        {QStringLiteral("kind"), QStringLiteral("episode")},
        {QStringLiteral("id"), e.value(QStringLiteral("id"))},
        {QStringLiteral("key"), QStringLiteral("episode:") + e.value(QStringLiteral("id")).toString()},
        {QStringLiteral("url"), e.value(QStringLiteral("url"))},
        {QStringLiteral("title"), details.value(QStringLiteral("title"))},
        {QStringLiteral("subtitle"), subtitle},
        {QStringLiteral("poster"), poster.isEmpty() ? details.value(QStringLiteral("poster")) : poster},
        {QStringLiteral("seriesId"), details.value(QStringLiteral("id"))},
        {QStringLiteral("seriesTitle"), details.value(QStringLiteral("title"))},
        {QStringLiteral("seriesPoster"), details.value(QStringLiteral("poster"))},
        {QStringLiteral("season"), season},
        {QStringLiteral("episode"), number},
        {QStringLiteral("episodeIndex"), episodeIndex},
        {QStringLiteral("start"), startAt(e)},
    };
}

QVariantMap VodService::nextEpisode(const QVariantMap &descriptor) const
{
    if (descriptor.value(QStringLiteral("kind")).toString() != QLatin1String("episode")) return {};
    const QString sid = descriptor.value(QStringLiteral("seriesId")).toString();
    if (!m_seriesDetails.contains(sid)) return {};
    VodItem item;
    if (const VodItem *known = find(QStringLiteral("series"), sid)) item = *known;
    else {
        item.id = sid;
        item.title = descriptor.value(QStringLiteral("seriesTitle")).toString();
        item.poster = descriptor.value(QStringLiteral("seriesPoster")).toString();
    }
    const QVariantMap d = decorate(m_seriesDetails.value(sid), QStringLiteral("series"), item);
    const QVariantList episodes = d.value(QStringLiteral("episodes")).toList();
    const QString id = descriptor.value(QStringLiteral("id")).toString();
    for (int i = 0; i + 1 < episodes.size(); ++i)
        if (episodes[i].toMap().value(QStringLiteral("id")).toString() == id)
            return playback(d, i + 1, true);
    return {};
}

void VodService::saveProgress(const QVariantMap &descriptor, double position, double duration)
{
    const QString key = descriptor.value(QStringLiteral("key")).toString();
    if (key.isEmpty() || position < 0) return;
    const bool watched = duration > 0 && position / duration >= 0.9;
    if (position < 15 && !watched) return;   // just a peek

    QVariantMap entry;
    // No "url": stream addresses contain the login.
    for (const char *k : {"kind", "id", "title", "poster", "year", "subtitle", "seriesId",
                          "seriesTitle", "seriesPoster", "season", "episode"}) {
        const QVariant v = descriptor.value(QLatin1String(k));
        if (v.isValid() && !v.toString().isEmpty()) entry.insert(QLatin1String(k), v);
    }
    entry.insert(QStringLiteral("pos"), qRound(position));
    entry.insert(QStringLiteral("dur"), qRound(duration));
    entry.insert(QStringLiteral("t"), QDateTime::currentMSecsSinceEpoch());
    entry.insert(QStringLiteral("watched"), watched);
    m_library->setVodProgress(key, entry);

    // Finished an episode: queue the next one in Continue watching.
    if (watched) {
        const QVariantMap next = nextEpisode(descriptor);
        const QString nextKey = next.value(QStringLiteral("key")).toString();
        if (!nextKey.isEmpty() && m_library->vodProgress(nextKey).isEmpty()) {
            QVariantMap n;
            for (const char *k : {"kind", "id", "title", "poster", "subtitle", "seriesId",
                                  "seriesTitle", "seriesPoster", "season", "episode"})
                if (next.contains(QLatin1String(k))) n.insert(QLatin1String(k), next.value(QLatin1String(k)));
            n.insert(QStringLiteral("pos"), 0);
            n.insert(QStringLiteral("dur"), 0);
            n.insert(QStringLiteral("t"), QDateTime::currentMSecsSinceEpoch() + 1);
            n.insert(QStringLiteral("watched"), false);
            n.insert(QStringLiteral("upNext"), true);
            m_library->setVodProgress(nextKey, n);
        }
    }

    m_items->refreshMarks();
    rebuildCategories();
    if (m_category == QLatin1String("continue")) rebuild();
}

void VodService::forgetProgress(const QString &key)
{
    // A series' key forgets every episode of it.
    if (key.startsWith(QLatin1String("series:"))) {
        const QString sid = key.mid(7);
        QStringList keys;
        const auto &all = m_library->allVodProgress();
        for (auto it = all.cbegin(); it != all.cend(); ++it)
            if (it.value().value(QStringLiteral("seriesId")).toString() == sid) keys << it.key();
        for (const QString &k : std::as_const(keys)) m_library->removeVodProgress(k);
    } else {
        m_library->removeVodProgress(key);
    }
    m_items->refreshMarks();
    rebuildCategories();
    if (m_category == QLatin1String("continue")) rebuild();
}

void VodService::setWatched(const QVariantMap &descriptor, bool watched)
{
    const QString key = descriptor.value(QStringLiteral("key")).toString();
    if (key.isEmpty()) return;
    emit watchedByHand(descriptor, watched);
    if (!watched) { forgetProgress(key); return; }
    saveProgress(descriptor, 1, 1);   // 100 %
}

bool VodService::toggleFavorite(const QString &kind, const QString &id)
{
    const QString key = kind + QLatin1Char(':') + id;
    const bool fav = !m_library->isVodFavorite(key);
    QVariantMap meta;
    if (const VodItem *it = find(kind, id)) meta = itemMap(*it, kind);
    m_library->setVodFavorite(key, fav, meta);
    m_items->refreshMarks();
    rebuildCategories();
    if (m_category == QLatin1String("favorites")) rebuild();
    return fav;
}

// ---- Several playlists as one catalogue ------------------------------------------------

int VodService::qualityOf(const QString &title)
{
    static const QRegularExpression uhd(QStringLiteral("(?<![A-Z0-9])(4K|UHD|2160P?)(?![A-Z0-9])"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fhd(QStringLiteral("(?<![A-Z0-9])(FHD|FULL ?HD|1080P?)(?![A-Z0-9])"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hd(QStringLiteral("(?<![A-Z0-9])(HD|720P?)(?![A-Z0-9])"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sd(QStringLiteral("(?<![A-Z0-9])(SD|480P?|576P?)(?![A-Z0-9])"), QRegularExpression::CaseInsensitiveOption);
    if (uhd.match(title).hasMatch()) return 4;
    if (fhd.match(title).hasMatch()) return 3;
    if (hd.match(title).hasMatch()) return 2;
    if (sd.match(title).hasMatch()) return 1;
    return 0;
}

void VodService::setCombine(bool on)
{
    if (on == m_combine) return;
    m_combine = on;
    QSettings().setValue(QStringLiteral("vod/combine"), on);
    emit combineChanged();
    if (on) for (const Extra &e : std::as_const(m_extras)) if (m_loaded || m_loading) e.svc->ensureLoaded();
    m_mergeTimer->stop();
    merge();
    emit stateChanged();
    rebuildCategories();
    rebuild();
}

void VodService::addExtraSource(const QString &playlistId, const QString &name, const PlaylistResult &result)
{
    if (m_isExtra || playlistId.isEmpty() || playlistId == m_ownId) return;
    for (const Extra &e : std::as_const(m_extras)) if (e.playlistId == playlistId) return;
    auto *svc = new VodService(m_library, m_cacheRoot, this);
    svc->m_isExtra = true;
    svc->setSource(result);
    if (!svc->available()) { svc->deleteLater(); return; }
    const int index = int(m_extras.size());
    m_extras.append(Extra{playlistId, name, svc});
    connect(svc, &VodService::stateChanged, this, [this, svc] { if (svc->loaded() && m_combine) m_mergeTimer->start(); });
    connect(svc, &VodService::detailsReady, this, [this, index](const QVariantMap &d) { extraDetails(index, d); });
    connect(svc, &VodService::detailsFailed, this, [this, index](const QString &kind, const QString &id, const QString &message) {
        const QString key = QString::number(index) + QLatin1Char('|') + kind + QLatin1Char(':') + id;
        if (auto done = m_waitingExtra.take(key)) done({});
        if (m_extraPages.remove(key))
            emit detailsFailed(kind, QLatin1Char('@') + m_extras.value(index).playlistId + QLatin1Char(':') + id, message);
    });
    qInfo().noquote() << "VOD: adding the movies and series of" << name;
    if (m_combine && (m_loaded || m_loading)) svc->ensureLoaded();
}

void VodService::clearExtraSources()
{
    if (m_extras.isEmpty()) return;
    for (const Extra &e : std::as_const(m_extras)) e.svc->deleteLater();
    m_extras.clear();
    m_waitingExtra.clear();
    m_extraPages.clear();
    m_mergeTimer->start();
}

// Each title once: this playlist's first, then the others' in playlist
// order. Titles match by name (tags like "EN -", "[4K]" left out) and year;
// one without a year matches by name. The first one found is the title
// (its details, its place); the rest are sources to play from.
void VodService::merge()
{
    m_sourcesOf.clear();
    auto index = [this] {
        m_movieIndex.clear();
        m_seriesIndex.clear();
        for (int i = 0; i < m_movies.size(); ++i) m_movieIndex.insert(m_movies[i].id, i);
        for (int i = 0; i < m_series.size(); ++i) m_seriesIndex.insert(m_series[i].id, i);
    };
    QVector<int> loaded;
    for (int i = 0; i < m_extras.size(); ++i) if (m_extras[i].svc->loaded()) loaded << i;
    if (m_isExtra || !m_combine) {
        m_movies = m_ownMovies;
        m_series = m_ownSeries;
        m_movieCats = m_ownMovieCats;
        m_seriesCats = m_ownSeriesCats;
        m_movieFolded = m_ownMovieFolded;
        m_seriesFolded = m_ownSeriesFolded;
        index();
        return;
    }

    int shared = 0;
    auto one = [&](bool movies, QVector<VodItem> &out, QVector<VodParser::Category> &cats, QStringList &folded,
                   QHash<QString, int> &aliases) {
        const QString kind = movies ? QStringLiteral("movie") : QStringLiteral("series");
        out.clear();
        folded.clear();
        cats = movies ? m_ownMovieCats : m_ownSeriesCats;
        QHash<QString, QString> catByName;               // folded name -> category id
        for (const VodParser::Category &c : std::as_const(cats)) catByName.insert(matchKey(c.name), c.id);
        QHash<QString, int> exact, loose;                // "name|year" / "name" -> position in out
        QStringList years;
        QVector<QVector<SourceRef>> refs;

        auto add = [&](const QVector<VodItem> &items, const QStringList &itemFolded, const QVector<VodParser::Category> &itemCats,
                       int extra) {
            QHash<QString, QString> catName, catMap;
            for (const VodParser::Category &c : itemCats) catName.insert(c.id, c.name);
            if (extra >= 0) {
                const QString prefix = QLatin1Char('@') + m_extras[extra].playlistId + QLatin1Char(':');
                for (const VodParser::Category &c : itemCats) {
                    const QString k = matchKey(c.name);
                    QString id = catByName.value(k);
                    if (id.isEmpty()) {
                        id = prefix + c.id;
                        catByName.insert(k, id);
                        cats.append(VodParser::Category{id, c.name});
                    }
                    catMap.insert(c.id, id);
                }
            }
            for (int i = 0; i < items.size(); ++i) {
                const VodItem &src = items[i];
                const TmdbClient::Query q = TmdbClient::clean(src.title, src.year);
                const QString name = matchKey(src.title);
                const QString year = q.year;
                int at = -1;
                if (!name.isEmpty()) {
                    if (!year.isEmpty()) at = exact.value(name + QLatin1Char('|') + year, -1);
                    if (at < 0) {
                        const int l = loose.value(name, -1);
                        if (l >= 0 && (year.isEmpty() || years[l].isEmpty())) at = l;
                    }
                }
                const SourceRef ref{extra, src.id, qualityOf(src.title + QLatin1Char(' ') + catName.value(src.categoryId))};
                const QString mergedId = extra < 0 ? src.id : QLatin1Char('@') + m_extras[extra].playlistId + QLatin1Char(':') + src.id;
                if (at >= 0) {
                    refs[at].append(ref);
                    aliases.insert(mergedId, at);
                    VodItem &kept = out[at];
                    if (kept.poster.isEmpty()) kept.poster = src.poster;
                    if (kept.rating <= 0) kept.rating = src.rating;
                    if (kept.year.isEmpty()) kept.year = src.year;
                    kept.added = qMax(kept.added, src.added);
                    continue;
                }
                VodItem it = src;
                it.id = mergedId;
                if (extra >= 0) it.categoryId = catMap.value(src.categoryId, it.categoryId.isEmpty() ? QString() : mergedId.section(QLatin1Char(':'), 0, 0) + QLatin1Char(':') + src.categoryId);
                const int pos = int(out.size());
                out.append(it);
                folded.append(i < itemFolded.size() ? itemFolded[i] : fold(src.title));
                years.append(year);
                refs.append({ref});
                if (!name.isEmpty()) {
                    if (!year.isEmpty() && !exact.contains(name + QLatin1Char('|') + year)) exact.insert(name + QLatin1Char('|') + year, pos);
                    if (!loose.contains(name)) loose.insert(name, pos);
                }
            }
        };
        add(movies ? m_ownMovies : m_ownSeries, movies ? m_ownMovieFolded : m_ownSeriesFolded,
            movies ? m_ownMovieCats : m_ownSeriesCats, -1);
        for (int e : std::as_const(loaded)) {
            const VodService *svc = m_extras[e].svc;
            add(movies ? svc->m_ownMovies : svc->m_ownSeries, movies ? svc->m_ownMovieFolded : svc->m_ownSeriesFolded,
                movies ? svc->m_ownMovieCats : svc->m_ownSeriesCats, e);
        }
        for (int i = 0; i < refs.size(); ++i)
            if (refs[i].size() > 1) { m_sourcesOf.insert(kind + QLatin1Char(':') + out[i].id, refs[i]); ++shared; }
    };
    QHash<QString, int> movieAliases, seriesAliases;
    one(true, m_movies, m_movieCats, m_movieFolded, movieAliases);
    one(false, m_series, m_seriesCats, m_seriesFolded, seriesAliases);
    index();
    // Another source's id (Continue watching from before, a favorite) finds the title.
    for (auto it = movieAliases.constBegin(); it != movieAliases.constEnd(); ++it) if (!m_movieIndex.contains(it.key())) m_movieIndex.insert(it.key(), it.value());
    for (auto it = seriesAliases.constBegin(); it != seriesAliases.constEnd(); ++it) if (!m_seriesIndex.contains(it.key())) m_seriesIndex.insert(it.key(), it.value());
    if (!loaded.isEmpty() || shared > 0)
        qInfo().noquote() << QStringLiteral("VOD: %1 movies and %2 series from %3 playlists, %4 titles on more than one source")
                                 .arg(m_movies.size()).arg(m_series.size()).arg(loaded.size() + 1).arg(shared);
}

int VodService::bestSource(const QVector<SourceRef> &refs) const
{
    int best = 0, bestQ = -1;
    for (int i = 0; i < refs.size(); ++i) {
        const int q = refs[i].quality > 0 ? refs[i].quality : 2;   // not saying: about HD
        if (q > bestQ) { best = i; bestQ = q; }
    }
    return best;
}

QVariantList VodService::sources(const QString &kind, const QString &id) const
{
    const QString k = kind == QLatin1String("episode") ? QStringLiteral("series") : kind;
    const QVector<SourceRef> refs = m_sourcesOf.value(k + QLatin1Char(':') + id);
    QVariantList out;
    if (refs.size() < 2) return out;
    const int best = bestSource(refs);
    static const char *names[] = {"", "SD", "HD", "FHD", "4K"};
    for (int i = 0; i < refs.size(); ++i) {
        const QString pid = playlistOf(refs[i]);
        out.append(QVariantMap{{QStringLiteral("index"), i}, {QStringLiteral("playlistId"), pid},
                               {QStringLiteral("name"), nameOf(refs[i])},
                               {QStringLiteral("quality"), QString::fromLatin1(names[qBound(0, refs[i].quality, 4)])},
                               {QStringLiteral("busy"), m_busy ? m_busy(pid) : false},
                               {QStringLiteral("best"), i == best}});
    }
    return out;
}

// Another source's details for a title (its address, its episodes).
void VodService::detailsOf(const SourceRef &r, const QString &kind, std::function<void(const QVariantMap &)> done)
{
    if (r.extra >= 0) {
        VodService *svc = m_extras.value(r.extra).svc;
        if (!svc) { done({}); return; }
        m_waitingExtra.insert(QString::number(r.extra) + QLatin1Char('|') + kind + QLatin1Char(':') + r.id, std::move(done));
        svc->requestDetails(kind, r.id);
        return;
    }
    // This playlist (a duplicate it lists twice).
    auto conns = std::make_shared<QList<QMetaObject::Connection>>();
    auto finish = [conns, done](const QVariantMap &d) {
        for (const auto &c : std::as_const(*conns)) QObject::disconnect(c);
        conns->clear();
        done(d);
    };
    conns->append(connect(this, &VodService::detailsReady, this, [kind, r, finish](const QVariantMap &d) {
        if (d.value(QStringLiteral("kind")).toString() == kind && d.value(QStringLiteral("id")).toString() == r.id) finish(d);
    }));
    conns->append(connect(this, &VodService::detailsFailed, this, [kind, r, finish](const QString &k, const QString &id, const QString &) {
        if (k == kind && id == r.id) finish({});
    }));
    requestDetails(kind, r.id);
}

void VodService::extraDetails(int extra, const QVariantMap &theirs)
{
    const QString kind = theirs.value(QStringLiteral("kind")).toString();
    const QString id = theirs.value(QStringLiteral("id")).toString();
    const QString key = QString::number(extra) + QLatin1Char('|') + kind + QLatin1Char(':') + id;
    if (auto done = m_waitingExtra.take(key)) done(theirs);
    if (!m_extraPages.remove(key)) return;
    // For its page here: under this catalogue's id, its episodes with their own keys.
    const QString prefix = QLatin1Char('@') + m_extras.value(extra).playlistId + QLatin1Char(':');
    QVariantMap d = theirs;
    QVariantList episodes = d.value(QStringLiteral("episodes")).toList();
    for (QVariant &v : episodes) {
        QVariantMap e = v.toMap();
        e.insert(QStringLiteral("id"), prefix + e.value(QStringLiteral("id")).toString());
        v = e;
    }
    if (!episodes.isEmpty()) d.insert(QStringLiteral("episodes"), episodes);
    VodItem item;
    if (const VodItem *known = find(kind, prefix + id)) item = *known;
    else { item.id = prefix + id; item.title = d.value(QStringLiteral("title")).toString(); }
    publish(decorate(d, kind, item));
}

void VodService::startPlayback(const QVariantMap &details, int episodeIndex, bool fromStart, int sourceIndex)
{
    startPlaybackAt(details, episodeIndex, fromStart, sourceIndex, -1);
}

void VodService::startPlaybackAt(const QVariantMap &details, int episodeIndex, bool fromStart, int sourceIndex, double start)
{
    const QString kind = details.value(QStringLiteral("kind")).toString();
    const QString id = details.value(QStringLiteral("id")).toString();
    QVariantMap base = playback(details, episodeIndex, fromStart);
    if (base.isEmpty()) { emit playbackProblem(tr("There's nothing to play")); return; }
    if (start >= 0) base.insert(QStringLiteral("start"), start);
    base.insert(QStringLiteral("detailsKind"), kind);
    base.insert(QStringLiteral("detailsId"), id);
    const QVector<SourceRef> refs = m_sourcesOf.value(kind + QLatin1Char(':') + id);
    const int index = sourceIndex >= 0 && sourceIndex < refs.size() ? sourceIndex : (refs.size() > 1 ? bestSource(refs) : 0);
    auto ready = [this, base, refs, index](const QString &url) {
        QVariantMap d = base;
        if (!url.isEmpty()) d.insert(QStringLiteral("url"), url);
        d.insert(QStringLiteral("sourceIndex"), index);
        d.insert(QStringLiteral("sourceCount"), qMax(1, int(refs.size())));
        d.insert(QStringLiteral("sourceName"), refs.size() > 1 ? nameOf(refs[index]) : QString());
        if (refs.size() > 1)
            qInfo().noquote() << "VOD: playing" << d.value(QStringLiteral("title")).toString() << "from" << nameOf(refs[index])
                              << QStringLiteral("(%1 of %2)").arg(index + 1).arg(refs.size());
        emit playbackReady(d);
    };
    if (refs.size() < 2 || index == 0) { ready(QString()); return; }   // the source whose details these are

    // Another source: its address for the same movie, or the same episode.
    const SourceRef r = refs[index];
    const int season = base.value(QStringLiteral("season")).toInt(), number = base.value(QStringLiteral("episode")).toInt();
    const bool movie = kind == QLatin1String("movie");
    const QString name = nameOf(r);
    detailsOf(r, kind, [this, ready, movie, season, number, name](const QVariantMap &d) {
        QString url;
        if (movie) url = d.value(QStringLiteral("url")).toString();
        else
            for (const QVariant &v : d.value(QStringLiteral("episodes")).toList()) {
                const QVariantMap e = v.toMap();
                if (e.value(QStringLiteral("season")).toInt() == season && e.value(QStringLiteral("episode")).toInt() == number) {
                    url = e.value(QStringLiteral("url")).toString();
                    break;
                }
            }
        if (url.isEmpty()) {
            emit playbackProblem(movie ? tr("%1 couldn't give this movie's address").arg(name)
                                       : tr("%1 doesn't have this episode").arg(name));
            return;
        }
        ready(url);
    });
}

void VodService::switchSource(const QVariantMap &descriptor, int sourceIndex, double position)
{
    const QString kind = descriptor.value(QStringLiteral("detailsKind")).toString();
    const QString id = descriptor.value(QStringLiteral("detailsId")).toString();
    const int episodeIndex = descriptor.value(QStringLiteral("episodeIndex"), -1).toInt();
    if (kind.isEmpty() || id.isEmpty()) { emit playbackProblem(tr("This can't switch source")); return; }
    auto go = [this, episodeIndex, sourceIndex, position](const QVariantMap &details) {
        if (details.isEmpty()) { emit playbackProblem(tr("The title's details didn't load")); return; }
        startPlaybackAt(details, episodeIndex, false, sourceIndex, position);
    };
    const auto recent = m_recentDetails.constFind(kind + QLatin1Char(':') + id);
    if (recent != m_recentDetails.constEnd()) {
        VodItem item;
        if (const VodItem *known = find(kind, id)) item = *known;
        else item.id = id;
        go(decorate(recent->second, kind, item));
        return;
    }
    detailsOf(SourceRef{-1, id, 0}, kind, go);
}
