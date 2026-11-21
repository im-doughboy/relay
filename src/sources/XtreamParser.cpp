#include "XtreamParser.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace {

// Xtream panels are inconsistent about numbers vs strings, so read loosely.
QString str(const QJsonObject &o, const char *key)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));
    if (v.isBool()) return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    return QString();
}

} // namespace

QUrl XtreamParser::normalizeServer(const QString &server)
{
    QString s = server.trimmed();
    if (!s.contains(QLatin1String("://"))) s.prepend(QStringLiteral("http://"));
    while (s.endsWith(QLatin1Char('/'))) s.chop(1);
    if (s.endsWith(QLatin1String("/player_api.php"))) s.chop(15);
    if (s.endsWith(QLatin1String("/get.php"))) s.chop(8);
    return QUrl(s);
}

QUrl XtreamParser::apiUrl(const Credentials &c, const QString &action)
{
    QUrl url = normalizeServer(c.server);
    url.setPath(url.path() + QStringLiteral("/player_api.php"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("username"), c.username);
    q.addQueryItem(QStringLiteral("password"), c.password);
    if (!action.isEmpty()) q.addQueryItem(QStringLiteral("action"), action);
    url.setQuery(q);
    return url;
}

QString XtreamParser::guideUrlForPlaylist(const QUrl &playlistUrl)
{
    const QString path = playlistUrl.path();
    if (!path.endsWith(QLatin1String("/get.php"), Qt::CaseInsensitive)) return QString();
    const QUrlQuery in(playlistUrl);
    const QString user = in.queryItemValue(QStringLiteral("username"), QUrl::FullyDecoded);
    const QString pass = in.queryItemValue(QStringLiteral("password"), QUrl::FullyDecoded);
    if (user.isEmpty() || pass.isEmpty()) return QString();

    QUrl guide = playlistUrl;
    guide.setPath(path.left(path.size() - 7) + QStringLiteral("xmltv.php"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("username"), user);
    q.addQueryItem(QStringLiteral("password"), pass);
    guide.setQuery(q);
    guide.setFragment(QString());
    return guide.toString(QUrl::FullyEncoded);
}

bool XtreamParser::credentialsFromPlaylistUrl(const QUrl &playlistUrl, Credentials *out)
{
    const QString path = playlistUrl.path();
    if (!playlistUrl.scheme().startsWith(QLatin1String("http"))
        || !path.endsWith(QLatin1String("/get.php"), Qt::CaseInsensitive))
        return false;
    const QUrlQuery in(playlistUrl);
    const QString user = in.queryItemValue(QStringLiteral("username"), QUrl::FullyDecoded);
    const QString pass = in.queryItemValue(QStringLiteral("password"), QUrl::FullyDecoded);
    if (user.isEmpty() || pass.isEmpty()) return false;
    QUrl server = playlistUrl;
    server.setPath(path.left(path.size() - 8));
    server.setQuery(QString());
    server.setFragment(QString());
    if (out) {
        out->server = server.toString(QUrl::FullyEncoded);
        out->username = user;
        out->password = pass;
        const QString output = in.queryItemValue(QStringLiteral("output"));
        if (!output.isEmpty()) out->output = output;
    }
    return true;
}

XtreamParser::AuthResult XtreamParser::parseAuth(const QByteArray &json)
{
    AuthResult r;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        r.message = QStringLiteral("The server didn't return a valid Xtream Codes response.");
        return r;
    }
    const QJsonObject info = doc.object().value(QLatin1String("user_info")).toObject();
    if (info.isEmpty() || str(info, "auth") != QLatin1String("1")) {
        r.message = QStringLiteral("Login failed. Check the username and password.");
        return r;
    }
    const QString status = str(info, "status");
    if (!status.isEmpty() && status.compare(QLatin1String("Active"), Qt::CaseInsensitive) != 0) {
        r.message = QStringLiteral("Account status is \"%1\".").arg(status);
        return r;
    }
    r.ok = true;
    const QJsonObject server = doc.object().value(QLatin1String("server_info")).toObject();
    r.timezone = str(server, "timezone");
    r.timeNow = str(server, "time_now");
    return r;
}

QHash<QString, int> XtreamParser::parseArchiveDays(const QByteArray &json)
{
    QHash<QString, int> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        if (str(o, "tv_archive") != QLatin1String("1")) continue;
        const int days = str(o, "tv_archive_duration").toInt();
        const QString id = str(o, "stream_id");
        if (days > 0 && !id.isEmpty()) out.insert(id, days);
    }
    return out;
}

QHash<QString, QString> XtreamParser::parseCategories(const QByteArray &json,
                                                      QStringList *orderedNames)
{
    QHash<QString, QString> map;
    const QJsonArray arr = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString name = str(o, "category_name");
        map.insert(str(o, "category_id"), name);
        if (orderedNames && !orderedNames->contains(name)) orderedNames->append(name);
    }
    return map;
}

PlaylistResult XtreamParser::parseLiveStreams(const QByteArray &json,
                                              const QHash<QString, QString> &categories,
                                              const Credentials &c,
                                              const QStringList &groupOrder)
{
    PlaylistResult result;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) {
        result.error = QStringLiteral("Couldn't read the channel list from the server.");
        return result;
    }

    const QString base = normalizeServer(c.server).toString();
    const QString user = QString::fromUtf8(QUrl::toPercentEncoding(c.username));
    const QString pass = QString::fromUtf8(QUrl::toPercentEncoding(c.password));
    const QString ext = c.output.isEmpty() ? QStringLiteral("ts") : c.output;

    const QJsonArray arr = doc.array();
    result.channels.reserve(arr.size());
    int position = 0;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        ++position;
        const QString streamId = str(o, "stream_id");
        if (streamId.isEmpty()) continue;

        Channel ch;
        ch.number = str(o, "num").toInt();
        if (ch.number <= 0) ch.number = position;
        ch.name = str(o, "name");
        ch.logo = str(o, "stream_icon");
        ch.tvgId = str(o, "epg_channel_id");
        ch.group = categories.value(str(o, "category_id"));
        if (ch.group.isEmpty()) ch.group = QStringLiteral("Uncategorized");
        ch.url = QStringLiteral("%1/live/%2/%3/%4.%5").arg(base, user, pass, streamId, ext);
        if (str(o, "tv_archive") == QLatin1String("1")) {
            ch.catchupType = QStringLiteral("xc");
            ch.catchupDays = str(o, "tv_archive_duration").toInt();
        }
        result.channels.push_back(ch);
    }

    // Xtream's EPG endpoint, used in phase 2.
    QUrl epg = normalizeServer(c.server);
    epg.setPath(epg.path() + QStringLiteral("/xmltv.php"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("username"), c.username);
    q.addQueryItem(QStringLiteral("password"), c.password);
    epg.setQuery(q);
    result.epgUrl = epg.toString();
    result.groupOrder = groupOrder;

    if (result.channels.isEmpty())
        result.error = QStringLiteral("This account has no live channels.");
    return result;
}
