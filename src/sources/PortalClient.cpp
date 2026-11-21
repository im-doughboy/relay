#include "PortalClient.h"

#include "app/Log.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QJsonArray>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QXmlStreamWriter>
#include <QtDebug>

// Portals only talk to set-top boxes; this is what one says.
const char PortalClient::kBoxAgent[] = "Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 (KHTML, like Gecko) "
                         "MAG200 stbapp ver: 2 rev: 250 Safari/533.3";
namespace {
constexpr int kGuideHours = 72;

QJsonValue js(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object().value(QLatin1String("js"));
}

QString text(const QJsonValue &v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));
    return QString();
}

QString netError(QNetworkReply *reply)
{
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (http >= 400) return QStringLiteral("The portal responded with HTTP %1.").arg(http);
    switch (reply->error()) {
    case QNetworkReply::HostNotFoundError: return QStringLiteral("Portal not found. Check the address.");
    case QNetworkReply::ConnectionRefusedError: return QStringLiteral("The portal refused the connection.");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError: return QStringLiteral("The portal took too long to respond.");
    default: return reply->errorString();
    }
}
} // namespace

PortalClient::PortalClient(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

PortalClient *PortalClient::instance()
{
    static QPointer<PortalClient> client;
    if (!client) client = new PortalClient(QCoreApplication::instance());
    return client;
}

// ---- helpers ---------------------------------------------------------------------------

QString PortalClient::normalizeMac(const QString &textIn)
{
    QString hex;
    for (const QChar c : textIn) {
        if (c.isDigit() || (c.toUpper() >= QLatin1Char('A') && c.toUpper() <= QLatin1Char('F'))) hex += c.toUpper();
        else if (c != QLatin1Char(':') && c != QLatin1Char('-') && c != QLatin1Char('.') && !c.isSpace()) return QString();
    }
    if (hex.size() != 12) return QString();
    QStringList parts;
    for (int i = 0; i < 12; i += 2) parts << hex.mid(i, 2);
    return parts.join(QLatin1Char(':'));
}

QList<QUrl> PortalClient::endpoints(const QString &server)
{
    QString s = server.trimmed();
    if (s.isEmpty()) return {};
    if (!s.contains(QLatin1String("://"))) s.prepend(QLatin1String("http://"));
    QUrl url(s);
    if (!url.isValid() || url.host().isEmpty()) return {};
    QString path = url.path();
    // An address that already names the API file.
    if (path.endsWith(QLatin1String(".php"))) {
        url.setQuery(QString());
        return {url};
    }
    // ".../c/", ".../c/index.html": the box's start page, next to the API.
    static const QRegularExpression page(QStringLiteral("/(c(/(index\\.html?)?)?)?$"));
    path.remove(page);
    auto at = [&url](const QString &p) { QUrl u = url; u.setPath(p); u.setQuery(QString()); u.setFragment(QString()); return u; };
    QList<QUrl> out;
    if (path.endsWith(QLatin1String("/stalker_portal"))) {
        out << at(path + QStringLiteral("/server/load.php")) << at(path + QStringLiteral("/portal.php"));
    } else {
        out << at(path + QStringLiteral("/portal.php")) << at(path + QStringLiteral("/server/load.php"))
            << at(path + QStringLiteral("/stalker_portal/server/load.php"));
    }
    return out;
}

QString PortalClient::accountId(const QString &server, const QString &mac)
{
    const QByteArray id = (server.trimmed().toLower() + QLatin1Char('|') + mac.toUpper()).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex().left(12));
}

QString PortalClient::channelUrl(const QString &account, const QString &channelId, const QString &cmd)
{
    QUrl u;
    u.setScheme(QStringLiteral("portal"));
    u.setHost(account);
    u.setPath(QLatin1Char('/') + channelId);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("cmd"), QString::fromLatin1(QUrl::toPercentEncoding(cmd)));
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

QString PortalClient::parseToken(const QByteArray &json)
{
    return js(json).toObject().value(QLatin1String("token")).toString();
}

QHash<QString, QString> PortalClient::parseGenres(const QByteArray &json, QStringList *order)
{
    QHash<QString, QString> out;
    for (const QJsonValue &v : js(json).toArray()) {
        const QJsonObject o = v.toObject();
        const QString id = text(o.value(QLatin1String("id")));
        const QString title = o.value(QLatin1String("title")).toString().trimmed();
        if (id.isEmpty() || id == QLatin1String("*") || title.isEmpty()) continue;
        out.insert(id, title);
        if (order && !order->contains(title)) order->append(title);
    }
    return out;
}

PlaylistResult PortalClient::parseChannels(const QByteArray &json, const QHash<QString, QString> &genres,
                                           const QStringList &genreOrder, const QString &account)
{
    PlaylistResult r;
    const QJsonValue root = js(json);
    const QJsonArray data = root.isArray() ? root.toArray() : root.toObject().value(QLatin1String("data")).toArray();
    r.channels.reserve(data.size());
    int position = 0;
    for (const QJsonValue &v : data) {
        const QJsonObject o = v.toObject();
        const QString id = text(o.value(QLatin1String("id")));
        const QString name = o.value(QLatin1String("name")).toString().trimmed();
        QString cmd = o.value(QLatin1String("cmd")).toString();
        if (cmd.isEmpty()) {   // newer portals: a list of sources
            const QJsonArray cmds = o.value(QLatin1String("cmds")).toArray();
            if (!cmds.isEmpty()) cmd = cmds.first().toObject().value(QLatin1String("url")).toString();
        }
        if (id.isEmpty() || name.isEmpty() || cmd.isEmpty()) continue;
        ++position;
        Channel c;
        const int number = text(o.value(QLatin1String("number"))).toInt();
        c.number = number > 0 ? number : position;
        c.name = name;
        c.group = genres.value(text(o.value(QLatin1String("tv_genre_id"))));
        if (c.group.isEmpty()) c.group = QStringLiteral("Other");
        c.logo = o.value(QLatin1String("logo")).toString();
        c.url = channelUrl(account, id, cmd);
        c.tvgId = id;   // the guide file made from the portal uses the same ids
        c.userAgent = QString::fromLatin1(kBoxAgent);
        // Catch-up: the portal keeps an archive of this channel (hours).
        if (text(o.value(QLatin1String("tv_archive"))) == QLatin1String("1")
            || text(o.value(QLatin1String("enable_tv_archive"))) == QLatin1String("1")) {
            const int hours = text(o.value(QLatin1String("tv_archive_duration"))).toInt();
            c.catchupType = QStringLiteral("portal");
            c.catchupDays = qMax(1, (hours > 0 ? hours : 72) / 24);
        }
        r.channels.append(c);
    }
    r.groupOrder = genreOrder;
    if (r.channels.isEmpty())
        r.error = QStringLiteral("The portal didn't list any channels for this MAC address.");
    return r;
}

QString PortalClient::parseLink(const QByteArray &json)
{
    QString cmd = js(json).toObject().value(QLatin1String("cmd")).toString().trimmed();
    // "ffmpeg http://…", "auto http://…": the player hint comes first.
    const int space = cmd.lastIndexOf(QLatin1Char(' '));
    if (space >= 0) cmd = cmd.mid(space + 1);
    return cmd.contains(QLatin1String("://")) ? cmd : QString();
}

QHash<QString, QMap<qint64, QString>> PortalClient::archiveIdsFromEpg(const QByteArray &json)
{
    QHash<QString, QMap<qint64, QString>> out;
    const QJsonValue root = js(json);
    const QJsonObject byChannel = root.toObject().contains(QLatin1String("data"))
        ? root.toObject().value(QLatin1String("data")).toObject() : root.toObject();
    for (auto it = byChannel.begin(); it != byChannel.end(); ++it)
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonObject p = v.toObject();
            const QString id = text(p.value(QLatin1String("id")));
            const qint64 start = text(p.value(QLatin1String("start_timestamp"))).toLongLong();
            if (!id.isEmpty() && start > 0) out[it.key()].insert(start, id);
        }
    return out;
}

QByteArray PortalClient::xmltvFromEpg(const QByteArray &json, int *programmes)
{
    int count = 0;
    QByteArray out;
    QXmlStreamWriter xml(&out);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("tv"));
    const QJsonValue root = js(json);
    const QJsonObject byChannel = root.toObject().contains(QLatin1String("data"))
        ? root.toObject().value(QLatin1String("data")).toObject() : root.toObject();
    for (auto it = byChannel.begin(); it != byChannel.end(); ++it) {
        xml.writeStartElement(QStringLiteral("channel"));
        xml.writeAttribute(QStringLiteral("id"), it.key());
        xml.writeTextElement(QStringLiteral("display-name"), it.key());
        xml.writeEndElement();
    }
    auto stamp = [](qint64 secs) {
        return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::utc()).toString(QStringLiteral("yyyyMMddHHmmss")) + QStringLiteral(" +0000");
    };
    for (auto it = byChannel.begin(); it != byChannel.end(); ++it) {
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonObject p = v.toObject();
            const qint64 start = text(p.value(QLatin1String("start_timestamp"))).toLongLong();
            const qint64 stop = text(p.value(QLatin1String("stop_timestamp"))).toLongLong();
            const QString title = p.value(QLatin1String("name")).toString().trimmed();
            if (start <= 0 || stop <= start || title.isEmpty()) continue;
            xml.writeStartElement(QStringLiteral("programme"));
            xml.writeAttribute(QStringLiteral("start"), stamp(start));
            xml.writeAttribute(QStringLiteral("stop"), stamp(stop));
            xml.writeAttribute(QStringLiteral("channel"), it.key());
            xml.writeTextElement(QStringLiteral("title"), title);
            const QString desc = p.value(QLatin1String("descr")).toString().trimmed();
            if (!desc.isEmpty()) xml.writeTextElement(QStringLiteral("desc"), desc);
            const QString category = p.value(QLatin1String("category")).toString().trimmed();
            if (!category.isEmpty()) xml.writeTextElement(QStringLiteral("category"), category);
            xml.writeEndElement();
            ++count;
        }
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    if (programmes) *programmes = count;
    return count > 0 ? out : QByteArray();
}

// ---- networking ------------------------------------------------------------------------

QNetworkReply *PortalClient::get(const Account &a, const QUrl &endpoint,
                                 const QList<QPair<QString, QString>> &query, int timeoutMs)
{
    QUrl url = endpoint;
    QUrlQuery q;
    for (const auto &item : query)
        q.addQueryItem(item.first, QString::fromLatin1(QUrl::toPercentEncoding(item.second)));
    q.addQueryItem(QStringLiteral("JsHttpRequest"), QStringLiteral("1-xml"));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", kBoxAgent);
    req.setRawHeader("X-User-Agent", "Model: MAG250; Link: WiFi");
    req.setRawHeader("Accept", "*/*");
    req.setRawHeader("Cookie", "mac=" + QUrl::toPercentEncoding(a.mac) + "; stb_lang=en; timezone=Europe/London");
    if (!a.token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + a.token.toLatin1());
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_net->get(req);
}

void PortalClient::signIn(const QString &account, int candidate, QObject *context,
                          std::function<void(const QString &)> done)
{
    const Account a = m_accounts.value(account);
    const QList<QUrl> list = endpoints(a.server);
    if (list.isEmpty()) { done(QStringLiteral("Enter the portal address, for example http://example.com:8080/c/")); return; }
    if (candidate >= list.size()) {
        done(QStringLiteral("This address didn't answer like a portal. Check the address your provider gave you."));
        return;
    }
    Account fresh = a;
    fresh.token.clear();
    QNetworkReply *shake = get(fresh, list[candidate], {{QStringLiteral("type"), QStringLiteral("stb")},
                                                        {QStringLiteral("action"), QStringLiteral("handshake")},
                                                        {QStringLiteral("token"), QString()},
                                                        {QStringLiteral("prehash"), QStringLiteral("0")}}, 15000);
    connect(shake, &QNetworkReply::finished, context, [=] {
        shake->deleteLater();
        const QString token = shake->error() == QNetworkReply::NoError ? parseToken(shake->readAll()) : QString();
        if (token.isEmpty()) {
            // Not here: try the next place. A server that can't be reached at all
            // won't answer anywhere else either.
            if (shake->error() == QNetworkReply::HostNotFoundError || shake->error() == QNetworkReply::ConnectionRefusedError
                || shake->error() == QNetworkReply::TimeoutError) { done(netError(shake)); return; }
            signIn(account, candidate + 1, context, done);
            return;
        }
        Account &acc = m_accounts[account];
        acc.token = token;
        acc.endpoint = list[candidate];
        const QString serial = QString::fromLatin1(QCryptographicHash::hash(acc.mac.toLatin1(), QCryptographicHash::Md5).toHex().left(13).toUpper());
        QNetworkReply *profile = get(acc, acc.endpoint, {
            {QStringLiteral("type"), QStringLiteral("stb")}, {QStringLiteral("action"), QStringLiteral("get_profile")},
            {QStringLiteral("hd"), QStringLiteral("1")}, {QStringLiteral("ver"), QStringLiteral("ImageDescription: 0.2.18-r14-pub-250; ImageDate: Fri Jan 15 15:20:44 EET 2016; PORTAL version: 5.6.1; API Version: JS API version: 328; STB API version: 134; Player Engine version: 0x566")},
            {QStringLiteral("num_banks"), QStringLiteral("2")}, {QStringLiteral("sn"), serial},
            {QStringLiteral("stb_type"), QStringLiteral("MAG250")}, {QStringLiteral("image_version"), QStringLiteral("218")},
            {QStringLiteral("video_out"), QStringLiteral("hdmi")}, {QStringLiteral("auth_second_step"), QStringLiteral("1")},
            {QStringLiteral("hw_version"), QStringLiteral("1.7-BD-00")}, {QStringLiteral("not_valid_token"), QStringLiteral("0")},
            {QStringLiteral("client_type"), QStringLiteral("STB")},
            {QStringLiteral("timestamp"), QString::number(QDateTime::currentSecsSinceEpoch())},
            {QStringLiteral("api_signature"), QStringLiteral("263")}}, 15000);
        connect(profile, &QNetworkReply::finished, context, [=] {
            profile->deleteLater();
            if (profile->error() != QNetworkReply::NoError) { done(netError(profile)); return; }
            const QJsonObject p = js(profile->readAll()).toObject();
            // A box the portal doesn't know gets an empty profile or a message.
            const QString blocked = p.value(QLatin1String("msg")).toString();
            const QString status = text(p.value(QLatin1String("status")));
            if (p.isEmpty() || (status != QLatin1String("0") && !status.isEmpty() && !blocked.isEmpty())) {
                done(blocked.isEmpty() ? QStringLiteral("The portal doesn't accept this MAC address.")
                                       : QStringLiteral("The portal refused this MAC address: %1").arg(blocked));
                return;
            }
            done(QString());
        });
    });
}

void PortalClient::load(const QString &server, const QString &macIn, QObject *context,
                        std::function<void(const QString &)> status,
                        std::function<void(const PlaylistResult &)> done)
{
    auto fail = [done](const QString &message) { PlaylistResult r; r.error = message; done(r); };
    const QString mac = normalizeMac(macIn);
    if (server.trimmed().isEmpty()) { fail(QStringLiteral("Enter the portal address.")); return; }
    if (mac.isEmpty()) { fail(QStringLiteral("Enter the MAC address as 00:1A:79:xx:xx:xx.")); return; }
    const QString account = accountId(server, mac);
    Account &a = m_accounts[account];
    a.server = server.trimmed();
    a.mac = mac;
    qInfo().noquote() << "Playlist: signing in to portal" << Log::redact(a.server);
    status(QStringLiteral("Signing in to the portal…"));
    signIn(account, 0, context, [=](const QString &error) {
        if (!error.isEmpty()) { fail(error); return; }
        const Account acc = m_accounts.value(account);
        status(QStringLiteral("Loading categories…"));
        QNetworkReply *genres = get(acc, acc.endpoint, {{QStringLiteral("type"), QStringLiteral("itv")},
                                                        {QStringLiteral("action"), QStringLiteral("get_genres")}});
        connect(genres, &QNetworkReply::finished, context, [=] {
            genres->deleteLater();
            QStringList order;
            const QHash<QString, QString> names = genres->error() == QNetworkReply::NoError
                ? parseGenres(genres->readAll(), &order) : QHash<QString, QString>();
            status(QStringLiteral("Loading channels…"));
            QNetworkReply *channels = get(acc, acc.endpoint, {{QStringLiteral("type"), QStringLiteral("itv")},
                                                              {QStringLiteral("action"), QStringLiteral("get_all_channels")}}, 60000);
            connect(channels, &QNetworkReply::finished, context, [=] {
                channels->deleteLater();
                if (channels->error() != QNetworkReply::NoError) { fail(netError(channels)); return; }
                PlaylistResult result = parseChannels(channels->readAll(), names, order, account);
                if (!result.error.isEmpty()) { done(result); return; }
                result.portalAccount = account;
                status(QStringLiteral("Loading the guide…"));
                QNetworkReply *epg = get(acc, acc.endpoint, {{QStringLiteral("type"), QStringLiteral("itv")},
                                                             {QStringLiteral("action"), QStringLiteral("get_epg_info")},
                                                             {QStringLiteral("period"), QString::number(kGuideHours)}}, 30000);
                connect(epg, &QNetworkReply::finished, context, [=]() mutable {
                    epg->deleteLater();
                    int count = 0;
                    const QByteArray body = epg->error() == QNetworkReply::NoError ? epg->readAll() : QByteArray();
                    const QByteArray xmltv = xmltvFromEpg(body, &count);
                    rememberArchive(account, archiveIdsFromEpg(body));
                    if (!xmltv.isEmpty()) {
                        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/portal");
                        QDir().mkpath(dir);
                        const QString path = dir + QStringLiteral("/guide-") + account + QStringLiteral(".xml");
                        QSaveFile f(path);
                        if (f.open(QIODevice::WriteOnly) && f.write(xmltv) == xmltv.size() && f.commit()) {
                            result.epgUrl = path;
                            result.guideIsNew = true;
                        }
                    }
                    qInfo().noquote() << QStringLiteral("Playlist: portal guide: %1 listings").arg(count);
                    done(result);
                });
            });
        });
    });
}

// ---- requests after sign-in -------------------------------------------------------------

void PortalClient::request(const QString &account, const QList<QPair<QString, QString>> &query, QObject *context,
                           std::function<void(const QByteArray &, const QString &)> done, int timeoutMs, bool retried)
{
    const Account acc = m_accounts.value(account);
    if (acc.server.isEmpty()) {
        done(QByteArray(), QStringLiteral("This portal isn't signed in. Reload the playlist."));
        return;
    }
    auto again = [=] {
        signIn(account, 0, context, [=](const QString &error) {
            if (!error.isEmpty()) done(QByteArray(), error);
            else request(account, query, context, done, timeoutMs, true);
        });
    };
    if (acc.token.isEmpty()) {
        if (retried) done(QByteArray(), QStringLiteral("Couldn't sign in to the portal."));
        else again();
        return;
    }
    QNetworkReply *reply = get(acc, acc.endpoint, query, timeoutMs);
    connect(reply, &QNetworkReply::finished, context, [=] {
        reply->deleteLater();
        const QByteArray body = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
        // A portal whose session ran out answers with nothing useful
        // ([] or a text message): sign in again, once.
        const QJsonValue v = js(body);
        const bool useless = v.isUndefined() || v.isNull() || (v.isArray() && v.toArray().isEmpty()
                                                               && !query.contains({QStringLiteral("action"), QStringLiteral("get_categories")}));
        if (reply->error() == QNetworkReply::NoError && !useless) { done(body, QString()); return; }
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool unreachable = reply->error() == QNetworkReply::HostNotFoundError
            || reply->error() == QNetworkReply::ConnectionRefusedError;
        if (!retried && !unreachable && (http == 0 || http == 200 || http == 401 || http == 403)) { again(); return; }
        done(QByteArray(), reply->error() != QNetworkReply::NoError ? netError(reply)
                                                                    : QStringLiteral("The portal didn't answer."));
    });
}

QString PortalClient::serverOf(const QString &account) const
{
    return m_accounts.value(account).server;
}

QUrl PortalClient::baseOf(const QString &account) const
{
    QUrl u = m_accounts.value(account).endpoint;
    u.setPath(QString());
    u.setQuery(QString());
    return u;
}

void PortalClient::rememberArchive(const QString &account, const QHash<QString, QMap<qint64, QString>> &ids)
{
    auto &known = m_archive[account];
    for (auto it = ids.begin(); it != ids.end(); ++it) {
        auto &channel = known[it.key()];
        for (auto p = it.value().begin(); p != it.value().end(); ++p) channel.insert(p.key(), p.value());
    }
}

// ---- turning a portal:// address into a real one ----------------------------------------

QString PortalClient::vodUrl(const QString &account, const QString &cmd, int series)
{
    QUrl u;
    u.setScheme(QStringLiteral("portal"));
    u.setHost(account);
    u.setPath(QStringLiteral("/vod"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("cmd"), QString::fromLatin1(QUrl::toPercentEncoding(cmd)));
    if (series > 0) q.addQueryItem(QStringLiteral("series"), QString::number(series));
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

void PortalClient::link(const QString &account, const QString &type, const QString &cmd, const QString &series,
                        QObject *context, std::function<void(const QString &, const QString &)> done)
{
    request(account, {{QStringLiteral("type"), type}, {QStringLiteral("action"), QStringLiteral("create_link")},
                      {QStringLiteral("cmd"), cmd}, {QStringLiteral("series"), series},
                      {QStringLiteral("forced_storage"), QStringLiteral("undefined")},
                      {QStringLiteral("disable_ad"), QStringLiteral("0")}, {QStringLiteral("download"), QStringLiteral("0")}},
            context, [=](const QByteArray &body, const QString &error) {
                if (!error.isEmpty()) { done(QString(), error); return; }
                const QString real = parseLink(body);
                if (real.isEmpty()) done(QString(), type == QLatin1String("itv")
                                                        ? QStringLiteral("The portal didn't give an address for this channel.")
                                                        : QStringLiteral("The portal didn't give an address for this video."));
                else done(real, QString());
            }, 15000);
}

QString PortalClient::archiveProgramme(const QHash<QString, QMap<qint64, QString>> &known, const QString &channel,
                                       qint64 at, qint64 *start)
{
    const QMap<qint64, QString> list = known.value(channel);
    auto it = list.upperBound(at);   // first programme starting after `at`
    if (it == list.begin()) return QString();
    --it;
    if (start) *start = it.key();
    return it.value();
}

QHash<QString, QMap<qint64, QString>> PortalClient::parseArchiveTable(const QByteArray &json, int *pages)
{
    QHash<QString, QMap<qint64, QString>> out;
    const QJsonObject o = js(json).toObject();
    const int total = text(o.value(QLatin1String("total_items"))).toInt();
    const int per = qMax(1, text(o.value(QLatin1String("max_page_items"))).toInt());
    if (pages) *pages = total > 0 ? (total + per - 1) / per : 1;
    for (const QJsonValue &v : o.value(QLatin1String("data")).toArray()) {
        const QJsonObject p = v.toObject();
        const QString id = text(p.value(QLatin1String("id")));
        const QString channel = text(p.value(QLatin1String("ch_id")));
        const qint64 start = text(p.value(QLatin1String("start_timestamp"))).toLongLong();
        if (!id.isEmpty() && !channel.isEmpty() && start > 0) out[channel].insert(start, id);
    }
    return out;
}

void PortalClient::resolveAt(const QString &portalUrl, QObject *context,
                             std::function<void(const QString &, const QString &, int)> done)
{
    const QUrl u(portalUrl);
    const QString account = u.host();
    const QUrlQuery q(u);
    if (!m_accounts.contains(account)) {
        done(QString(), QStringLiteral("This portal isn't signed in. Reload the playlist."), 0);
        return;
    }
    auto plain = [done](const QString &real, const QString &error) { done(real, error, 0); };
    const QString path = u.path();

    if (path == QLatin1String("/vod")) {
        const QString cmd = QUrl::fromPercentEncoding(q.queryItemValue(QStringLiteral("cmd"), QUrl::FullyEncoded).toLatin1());
        link(account, QStringLiteral("vod"), cmd, q.queryItemValue(QStringLiteral("series")), context, plain);
        return;
    }

    if (path.startsWith(QLatin1String("/archive/"))) {
        // A past show: the portal plays archives by programme id.
        const QString channel = path.mid(9);
        const qint64 from = q.queryItemValue(QStringLiteral("from")).toLongLong();
        auto play = [=](const QString &programme, qint64 start) {
            link(account, QStringLiteral("tv_archive"), QStringLiteral("auto /media/%1.mpg").arg(programme), QString(), context,
                 [=](const QString &real, const QString &error) { done(real, error, int(qMax<qint64>(0, from - start))); });
        };
        qint64 start = 0;
        const QString known = archiveProgramme(m_archive.value(account), channel, from, &start);
        if (!known.isEmpty()) { play(known, start); return; }
        // Not in the guide we loaded: ask for that day's listings (a page at a time).
        const QString date = QDateTime::fromSecsSinceEpoch(from).toString(QStringLiteral("yyyy-MM-dd"));
        auto page = std::make_shared<std::function<void(int)>>();
        *page = [=](int p) {
            request(account, {{QStringLiteral("type"), QStringLiteral("epg")}, {QStringLiteral("action"), QStringLiteral("get_simple_data_table")},
                              {QStringLiteral("ch_id"), channel}, {QStringLiteral("date"), date}, {QStringLiteral("p"), QString::number(p)}},
                    context, [=](const QByteArray &body, const QString &error) {
                        if (!error.isEmpty()) { done(QString(), error, 0); return; }
                        int pages = 1;
                        const auto table = parseArchiveTable(body, &pages);
                        m_archive[account][channel].insert(table.value(channel));
                        qint64 s = 0;
                        const QString id = archiveProgramme(m_archive.value(account), channel, from, &s);
                        const auto list = m_archive.value(account).value(channel);
                        const bool coversFrom = !list.isEmpty() && list.lastKey() > from;
                        if (!id.isEmpty() && (coversFrom || p >= pages)) { play(id, s); return; }
                        if (p < pages && p < 12) { (*page)(p + 1); return; }
                        done(QString(), QStringLiteral("The portal has no recording of this show."), 0);
                    });
        };
        (*page)(1);
        return;
    }

    // Live channel.
    const QString cmd = QUrl::fromPercentEncoding(q.queryItemValue(QStringLiteral("cmd"), QUrl::FullyEncoded).toLatin1());
    if (cmd.isEmpty()) { done(QString(), QStringLiteral("This channel's portal address is incomplete."), 0); return; }
    link(account, QStringLiteral("itv"), cmd, QString(), context, plain);
}

void PortalClient::resolve(const QString &portalUrl, QObject *context,
                           std::function<void(const QString &, const QString &)> done)
{
    resolveAt(portalUrl, context, [done](const QString &real, const QString &error, int) { done(real, error); });
}
