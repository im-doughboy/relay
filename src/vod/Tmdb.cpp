#include "Tmdb.h"

#include "VodParser.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtDebug>

namespace {
constexpr int kParallel = 4;
constexpr qint64 kFoundDays = 30, kMissingDays = 7;

QString fold(const QString &s)
{
    QString out;
    for (const QChar c : s.normalized(QString::NormalizationForm_KD))
        if (c.isLetterOrNumber()) out += c.toLower();
    return out;
}

QString yearOfDate(const QString &date) { return date.left(4).toInt() > 1800 ? date.left(4) : QString(); }
} // namespace

TmdbClient::TmdbClient(const QString &cacheDir, QObject *parent)
    : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    m_language = QLocale().name().replace(QLatin1Char('_'), QLatin1Char('-'));
    if (m_language == QLatin1String("C")) m_language = QStringLiteral("en-US");
    QDir().mkpath(cacheDir);
    m_cacheFile = cacheDir + QStringLiteral("/tmdb-") + m_language + QStringLiteral(".json");
    QFile f(m_cacheFile);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        for (auto it = all.begin(); it != all.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            const qint64 age = now - qint64(o.value(QLatin1String("t")).toDouble());
            const bool missing = o.value(QLatin1String("none")).toBool();
            if (age < (missing ? kMissingDays : kFoundDays) * 86400) m_cache.insert(it.key(), o);
        }
    }
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(4000);
    connect(&m_saveTimer, &QTimer::timeout, this, &TmdbClient::save);
}

TmdbClient::~TmdbClient()
{
    if (m_saveTimer.isActive()) save();
}

QString TmdbClient::apiBase()
{
    const QString env = qEnvironmentVariable("RELAY_TMDB_URL");
    return env.isEmpty() ? QStringLiteral("https://api.themoviedb.org/3") : env;
}

QString TmdbClient::imageBase()
{
    const QString env = qEnvironmentVariable("RELAY_TMDB_IMAGE_URL");
    return env.isEmpty() ? QStringLiteral("https://image.tmdb.org/t/p/") : env;
}

void TmdbClient::setKey(const QString &key)
{
    const QString k = key.trimmed();
    if (k == m_key) return;
    m_key = k;
    m_rejected = false;
}

// ---- titles ---------------------------------------------------------------------------------

TmdbClient::Query TmdbClient::clean(const QString &titleIn, const QString &yearIn)
{
    QString t = titleIn;
    // Provider prefixes: "EN - ", "|EN| ", "[EN] ", "VOD: ", "4K-NF: ".
    static const QRegularExpression prefix(QStringLiteral(
        "^\\s*(?:\\[[^\\]]{1,12}\\]|\\|[^|]{1,12}\\||[A-Z0-9][A-Z0-9+-]{1,7}\\s*(?:-|:|\\|)\\s+)"));
    for (int i = 0; i < 3; ++i) {
        const QRegularExpressionMatch m = prefix.match(t);
        if (!m.hasMatch()) break;
        t = t.mid(m.capturedLength());
    }
    QString year = yearIn.left(4);
    static const QRegularExpression bracketYear(QStringLiteral("[\\(\\[]\\s*((?:19|20)\\d{2})\\s*[\\)\\]]"));
    const QRegularExpressionMatch by = bracketYear.match(t);
    if (by.hasMatch() && year.isEmpty()) year = by.captured(1);
    static const QRegularExpression brackets(QStringLiteral("[\\(\\[\\{][^\\)\\]\\}]*[\\)\\]\\}]"));
    t.remove(brackets);
    // Scene-style names use dots or underscores for spaces.
    if (!t.contains(QLatin1Char(' ')) && (t.count(QLatin1Char('.')) >= 2 || t.contains(QLatin1Char('_'))))
        t.replace(QLatin1Char('.'), QLatin1Char(' ')).replace(QLatin1Char('_'), QLatin1Char(' '));
    static const QRegularExpression quality(QStringLiteral(
        "(?:^|\\s)(?:4K|UHD|FHD|HD|SD|HDR|HDR10|DV|1080p|720p|2160p|480p|HEVC|H\\.?26[45]|x26[45]|MULTI|VOSTFR|VOST|"
        "SUBS?|DUB(?:BED)?|LATINO|CAM|HDCAM|TS|WEB-?DL|WEB-?Rip|BluRay|BRRip|DVDRip|REMUX|10bit|ATMOS)(?=\\s|$)"),
        QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < 4 && t.contains(quality); ++i) t.replace(quality, QStringLiteral(" "));
    t = t.simplified();
    static const QRegularExpression edges(QStringLiteral("^[\\s\\-:|·.]+|[\\s\\-:|·.]+$"));
    t.remove(edges);
    // A year at the end ("Arrival 2016") — or a number that belongs to the
    // title ("Blade Runner 2049"): keep both spellings.
    QString withNumber;
    static const QRegularExpression trailingYear(QStringLiteral("(?:^|\\s|-)((?:19|20)\\d{2})$"));
    const QRegularExpressionMatch ty = trailingYear.match(t);
    if (ty.hasMatch() && ty.capturedStart() > 0) {
        withNumber = t;
        if (year.isEmpty()) year = ty.captured(1);
        t.truncate(ty.capturedStart());
        t.remove(edges);
    }
    t = t.simplified();
    Query q{t.isEmpty() ? titleIn.trimmed() : t, year.toInt() > 1800 ? year : QString(), withNumber};
    return q;
}

int TmdbClient::bestMatch(const QJsonObject &answer, const Query &q, bool tv)
{
    const QJsonArray results = answer.value(QLatin1String("results")).toArray();
    if (results.isEmpty()) return -1;
    const QString want = fold(q.title);
    int sameTitle = -1;
    for (int i = 0; i < results.size(); ++i) {
        const QJsonObject r = results[i].toObject();
        const QString title = r.value(QLatin1String(tv ? "name" : "title")).toString();
        const QString original = r.value(QLatin1String(tv ? "original_name" : "original_title")).toString();
        if (fold(title) != want && fold(original) != want) continue;
        const int year = yearOfDate(r.value(QLatin1String(tv ? "first_air_date" : "release_date")).toString()).toInt();
        if (!q.year.isEmpty() && qAbs(year - q.year.toInt()) <= 1) return i;
        if (sameTitle < 0) sameTitle = i;
    }
    return sameTitle >= 0 ? sameTitle : 0;
}

// ---- network --------------------------------------------------------------------------------

void TmdbClient::get(const QString &path, QUrlQuery query, QObject *context,
                     std::function<void(int, const QJsonObject &)> done)
{
    if (!m_key.startsWith(QLatin1String("eyJ"))) query.addQueryItem(QStringLiteral("api_key"), m_key);
    if (!query.hasQueryItem(QStringLiteral("language"))) query.addQueryItem(QStringLiteral("language"), m_language);
    m_queue.append({path, query, m_key, context, std::move(done)});
    pump();
}

void TmdbClient::pump()
{
    while (m_running < kParallel && !m_queue.isEmpty()) {
        Pending p = m_queue.takeFirst();
        if (!p.context) continue;
        QUrl url(apiBase() + p.path);
        url.setQuery(p.query);
        QNetworkRequest req(url);
        if (p.key.startsWith(QLatin1String("eyJ"))) req.setRawHeader("Authorization", "Bearer " + p.key.toUtf8());
        req.setRawHeader("Accept", "application/json");
        req.setTransferTimeout(20000);
        ++m_running;
        QNetworkReply *reply = m_net->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, p] {
            reply->deleteLater();
            --m_running;
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            if (http == 401 && !m_rejected && p.key == m_key) {
                m_rejected = true;
                qWarning().noquote() << "TMDB: the key was refused";
                emit keyRejected();
            }
            if (p.context) p.done(http, body);
            pump();
        });
    }
}

QJsonObject TmdbClient::cached(const QString &key, bool *found) const
{
    const auto it = m_cache.constFind(key);
    *found = it != m_cache.constEnd();
    return *found ? it.value() : QJsonObject();
}

void TmdbClient::store(const QString &key, const QJsonObject &valueIn)
{
    QJsonObject value = valueIn;
    value.insert(QStringLiteral("t"), double(QDateTime::currentSecsSinceEpoch()));
    m_cache.insert(key, value);
    scheduleSave();
}

void TmdbClient::scheduleSave()
{
    if (!m_saveTimer.isActive()) m_saveTimer.start();
}

void TmdbClient::save()
{
    m_saveTimer.stop();
    QJsonObject all;
    for (auto it = m_cache.constBegin(); it != m_cache.constEnd(); ++it) all.insert(it.key(), it.value());
    QSaveFile f(m_cacheFile);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

QVariantMap TmdbClient::infoFromSearch(const QJsonObject &r, bool tv) const
{
    QVariantMap info;
    info.insert(QStringLiteral("id"), r.value(QLatin1String("id")).toInt());
    const QString poster = r.value(QLatin1String("poster_path")).toString();
    if (!poster.isEmpty()) info.insert(QStringLiteral("poster"), imageBase() + QStringLiteral("w342") + poster);
    const QString backdrop = r.value(QLatin1String("backdrop_path")).toString();
    if (!backdrop.isEmpty()) info.insert(QStringLiteral("backdrop"), imageBase() + QStringLiteral("w1280") + backdrop);
    info.insert(QStringLiteral("year"), yearOfDate(r.value(QLatin1String(tv ? "first_air_date" : "release_date")).toString()));
    info.insert(QStringLiteral("rating"), r.value(QLatin1String("vote_average")).toDouble());
    info.insert(QStringLiteral("plot"), r.value(QLatin1String("overview")).toString());
    info.insert(QStringLiteral("tmdbTitle"), r.value(QLatin1String(tv ? "name" : "title")).toString());
    return info;
}

// ---- lookups -------------------------------------------------------------------------------

void TmdbClient::lookup(const QString &kind, const QString &title, const QString &year, QObject *context,
                        std::function<void(const QVariantMap &)> done)
{
    if (!active() || title.trimmed().isEmpty()) { done({}); return; }
    const bool tv = kind != QLatin1String("movie");
    const Query q = clean(title, year);
    const QString key = QStringLiteral("search|%1|%2|%3").arg(tv ? QStringLiteral("tv") : QStringLiteral("movie"), fold(q.title), q.year);
    bool found = false;
    const QJsonObject hit = cached(key, &found);
    if (found) {
        done(hit.value(QLatin1String("none")).toBool() ? QVariantMap() : hit.value(QLatin1String("info")).toObject().toVariantMap());
        return;
    }
    // The same title asked for twice at once: one request.
    QPointer<QObject> ctx(context);
    auto &waiting = m_waiting[key];
    waiting.append([ctx, done](const QVariantMap &info) { if (ctx) done(info); });
    if (waiting.size() > 1) return;

    auto finish = [this, key](const QVariantMap &info, bool remember) {
        if (remember) {
            if (info.isEmpty()) store(key, QJsonObject{{"none", true}});
            else store(key, QJsonObject{{"info", QJsonObject::fromVariantMap(info)}});
        }
        const auto callbacks = m_waiting.take(key);
        for (const auto &cb : callbacks) cb(info);
    };
    // Tries: title + year, the title with its number (no year), the title alone.
    auto search = std::make_shared<std::function<void(int)>>();
    *search = [=](int attempt) {
        if (attempt == 1 && q.withNumber.isEmpty()) attempt = 2;
        if (attempt == 0 && q.year.isEmpty()) attempt = q.withNumber.isEmpty() ? 2 : 1;
        const bool withYear = attempt == 0;
        const Query asked{attempt == 1 ? q.withNumber : q.title, withYear ? q.year : QString(), QString()};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("query"), asked.title);
        if (withYear) query.addQueryItem(tv ? QStringLiteral("first_air_date_year") : QStringLiteral("year"), q.year);
        query.addQueryItem(QStringLiteral("include_adult"), QStringLiteral("false"));
        get(tv ? QStringLiteral("/search/tv") : QStringLiteral("/search/movie"), query, this,
            [=](int http, const QJsonObject &body) {
                if (http != 200) { finish({}, http == 404); return; }   // try again next time
                const int best = bestMatch(body, asked, tv);
                if (best < 0 && attempt < 2) { (*search)(attempt + 1); return; }   // the playlist's year may be off
                finish(best < 0 ? QVariantMap() : infoFromSearch(body.value(QLatin1String("results")).toArray()[best].toObject(), tv), true);
            });
    };
    (*search)(0);
}

void TmdbClient::details(const QString &kind, const QString &title, const QString &year, const QList<int> &seasons,
                         QObject *context, std::function<void(const QVariantMap &)> done)
{
    QPointer<QObject> ctx(context);
    lookup(kind, title, year, this, [=](const QVariantMap &base) {
        if (!ctx) return;
        const int id = base.value(QStringLiteral("id")).toInt();
        if (base.isEmpty() || id <= 0) { done(base); return; }
        const bool tv = kind != QLatin1String("movie");
        const QString type = tv ? QStringLiteral("tv") : QStringLiteral("movie");

        struct State { QVariantMap out; QVariantMap episodes; int left = 0; };
        auto st = std::make_shared<State>();
        st->out = base;
        auto oneDone = [=] {
            if (--st->left > 0 || !ctx) return;
            if (!st->episodes.isEmpty()) st->out.insert(QStringLiteral("episodes"), st->episodes);
            done(st->out);
        };
        auto applyFull = [=](const QJsonObject &o) {
            QStringList genres;
            for (const QJsonValue &g : o.value(QLatin1String("genres")).toArray())
                genres << g.toObject().value(QLatin1String("name")).toString();
            if (!genres.isEmpty()) st->out.insert(QStringLiteral("genre"), genres.join(QStringLiteral(", ")));
            const QJsonObject credits = o.value(QLatin1String("credits")).toObject();
            QStringList cast;
            for (const QJsonValue &c : credits.value(QLatin1String("cast")).toArray()) {
                cast << c.toObject().value(QLatin1String("name")).toString();
                if (cast.size() == 6) break;
            }
            if (!cast.isEmpty()) st->out.insert(QStringLiteral("cast"), cast.join(QStringLiteral(", ")));
            QStringList directors;
            for (const QJsonValue &c : credits.value(QLatin1String("crew")).toArray())
                if (c.toObject().value(QLatin1String("job")).toString() == QLatin1String("Director"))
                    directors << c.toObject().value(QLatin1String("name")).toString();
            for (const QJsonValue &c : o.value(QLatin1String("created_by")).toArray())
                directors << c.toObject().value(QLatin1String("name")).toString();
            directors.removeDuplicates();
            if (!directors.isEmpty()) st->out.insert(QStringLiteral("director"), directors.mid(0, 3).join(QStringLiteral(", ")));
            int minutes = o.value(QLatin1String("runtime")).toInt();
            if (minutes <= 0) {
                const QJsonArray runs = o.value(QLatin1String("episode_run_time")).toArray();
                if (!runs.isEmpty()) minutes = runs.first().toInt();
            }
            if (minutes > 0)
                st->out.insert(QStringLiteral("duration"), minutes >= 60 ? QStringLiteral("%1 h %2 min").arg(minutes / 60).arg(minutes % 60)
                                                                         : QStringLiteral("%1 min").arg(minutes));
        };
        auto fetch = [=](const QString &cacheKey, const QString &path, QUrlQuery query,
                         std::function<void(const QJsonObject &)> use) {
            bool found = false;
            const QJsonObject hit = cached(cacheKey, &found);
            if (found) { use(hit.value(QLatin1String("data")).toObject()); oneDone(); return; }
            get(path, query, this, [=](int http, const QJsonObject &body) {
                if (http == 200) {
                    store(cacheKey, QJsonObject{{"data", body}});
                    use(body);
                }
                oneDone();
            });
        };

        st->left = 1 + (tv ? int(seasons.size()) : 0);
        QUrlQuery withCredits;
        withCredits.addQueryItem(QStringLiteral("append_to_response"), QStringLiteral("credits"));
        fetch(QStringLiteral("%1|%2").arg(type).arg(id), QStringLiteral("/%1/%2").arg(type).arg(id), withCredits, applyFull);
        if (!tv) return;
        for (int season : seasons) {
            fetch(QStringLiteral("season|%1|%2").arg(id).arg(season), QStringLiteral("/tv/%1/season/%2").arg(id).arg(season), {},
                  [=](const QJsonObject &o) {
                for (const QJsonValue &v : o.value(QLatin1String("episodes")).toArray()) {
                    const QJsonObject e = v.toObject();
                    const QString still = e.value(QLatin1String("still_path")).toString();
                    st->episodes.insert(QStringLiteral("%1:%2").arg(season).arg(e.value(QLatin1String("episode_number")).toInt()),
                                        QVariantMap{{QStringLiteral("title"), e.value(QLatin1String("name")).toString()},
                                                    {QStringLiteral("plot"), e.value(QLatin1String("overview")).toString()},
                                                    {QStringLiteral("still"), still.isEmpty() ? QString() : imageBase() + QStringLiteral("w300") + still}});
                }
            });
        }
    });
}

void TmdbClient::check(const QString &key, QObject *context, std::function<void(bool, const QString &)> done)
{
    const QString previous = m_key;
    const bool wasRejected = m_rejected;
    m_key = key.trimmed();
    m_rejected = false;
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("query"), QStringLiteral("Casablanca"));
    get(QStringLiteral("/search/movie"), q, context, [=](int http, const QJsonObject &) {
        if (http == 200) done(true, tr("The key works."));
        else if (http == 401) done(false, tr("TMDB didn't accept this key."));
        else if (http == 0) done(false, tr("Couldn't reach TMDB. Check the internet connection."));
        else done(false, tr("TMDB answered with HTTP %1.").arg(http));
    });
    m_key = previous;   // the request already has the key being tried
    m_rejected = wasRejected;
}
