#pragma once

#include "Channel.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Stalker / Ministra portals ("MAC portals"): the provider gives a portal
// address and the box is identified by its MAC address instead of a login.
//
// Channels from a portal don't have a fixed stream address: the portal hands
// out a temporary link each time a channel is opened. So a portal channel's
// url is "portal://<account>/<channel id>?cmd=…", and whoever plays it
// (MpvItem, the recorder) asks resolve() for the real address first.
//
// The parsing helpers are static and synchronous (unit-testable); the
// networking runs on the thread that calls it (the UI thread).
class PortalClient : public QObject
{
    Q_OBJECT

public:
    static PortalClient *instance();

    // "00:1a:79:AB:CD:EF" / "001A79ABCDEF" -> "00:1A:79:AB:CD:EF"; "" if it isn't a MAC address.
    static QString normalizeMac(const QString &text);
    // Where the portal's API may live for an address as typed
    // ("http://host:8080/c/", "host/stalker_portal/c", …), most likely first.
    static QList<QUrl> endpoints(const QString &server);
    static bool isPortalUrl(const QString &url) { return url.startsWith(QLatin1String("portal://")); }
    static const char kBoxAgent[];
    // portal://<account>/vod?cmd=…[&series=n]: a movie or episode.
    static QString vodUrl(const QString &account, const QString &cmd, int series = 0);
    // portal://<account>/archive/<channel id>?from=…&stop=… is a past show
    // (catch-up); Catchup::url makes those.
    // Archive programme ids from the guide (get_epg_info) or a day's table
    // (get_simple_data_table): channel id -> start -> programme id.
    static QHash<QString, QMap<qint64, QString>> archiveIdsFromEpg(const QByteArray &json);
    static QHash<QString, QMap<qint64, QString>> parseArchiveTable(const QByteArray &json, int *pages = nullptr);
    // The programme on at `at`, and its start.
    static QString archiveProgramme(const QHash<QString, QMap<qint64, QString>> &known, const QString &channel,
                                    qint64 at, qint64 *start = nullptr);
    static QString accountId(const QString &server, const QString &mac);
    static QString channelUrl(const QString &account, const QString &channelId, const QString &cmd);

    static QString parseToken(const QByteArray &json);
    // genre id -> title, with the portal's order.
    static QHash<QString, QString> parseGenres(const QByteArray &json, QStringList *order = nullptr);
    static PlaylistResult parseChannels(const QByteArray &json, const QHash<QString, QString> &genres,
                                        const QStringList &genreOrder, const QString &account);
    // The stream address inside a create_link answer ("ffmpeg http://…" -> "http://…").
    static QString parseLink(const QByteArray &json);
    // The portal's guide (get_epg_info) as an XMLTV file whose channel ids
    // are the portal's channel ids. Empty when there are no listings.
    static QByteArray xmltvFromEpg(const QByteArray &json, int *programmes = nullptr);

    // Signs in and loads the channel list (and the guide, into a file in the
    // cache folder, named in result.epgUrl). `status` reports progress;
    // `alive` is asked before each step (false = the load was superseded).
    void load(const QString &server, const QString &mac, QObject *context,
              std::function<void(const QString &)> status,
              std::function<void(const PlaylistResult &)> done);

    // The real address of a portal channel right now. The callback gets
    // ("", reason) when the portal refuses. Signs in again when the session
    // has expired.
    void resolve(const QString &portalUrl, QObject *context,
                 std::function<void(const QString &url, const QString &error)> done);
    // Same, with how many seconds into the returned stream playback should
    // start (a catch-up stream starts where the show starts).
    void resolveAt(const QString &portalUrl, QObject *context,
                   std::function<void(const QString &url, const QString &error, int skipSecs)> done);

    // A call to the portal's API (type, action, …) on a signed-in account.
    // Signs in again, once, when the session has run out.
    void request(const QString &account, const QList<QPair<QString, QString>> &query, QObject *context,
                 std::function<void(const QByteArray &body, const QString &error)> done,
                 int timeoutMs = 30000, bool retried = false);
    QString serverOf(const QString &account) const;
    QUrl baseOf(const QString &account) const;   // scheme://host:port, for relative picture paths
    void rememberArchive(const QString &account, const QHash<QString, QMap<qint64, QString>> &ids);

private:
    explicit PortalClient(QObject *parent = nullptr);
    struct Account {
        QString server, mac, token;
        QUrl endpoint;
    };
    QNetworkReply *get(const Account &a, const QUrl &endpoint, const QList<QPair<QString, QString>> &query,
                       int timeoutMs = 30000);
    // Handshake + profile on each possible endpoint until one answers.
    void signIn(const QString &account, int candidate, QObject *context,
                std::function<void(const QString &error)> done);
    void link(const QString &account, const QString &type, const QString &cmd, const QString &series,
              QObject *context, std::function<void(const QString &, const QString &)> done);

    QNetworkAccessManager *m_net;
    QHash<QString, Account> m_accounts;
    QHash<QString, QHash<QString, QMap<qint64, QString>>> m_archive;   // account -> channel -> start -> id
};
