#include "app/AppSettings.h"
#include "app/Backup.h"
#include "app/Log.h"
#include "app/UserLibrary.h"
#include "app/Parental.h"
#include "app/OtherPlaylists.h"
#include "app/Session.h"
#include "models/ChannelModel.h"
#include "player/PlayerController.h"
#include "sources/M3uParser.h"
#include "sources/SavedPlaylist.h"
#include "sources/XtreamParser.h"
#include "sources/PortalClient.h"
#include "sources/Catchup.h"
#include "vod/Tmdb.h"
#include "app/Trakt.h"
#include "app/Updater.h"
#include "app/RemoteInput.h"
#include "vod/Trailers.h"
#include "app/Gamepads.h"
#include "app/PhoneRemote.h"
#ifdef RELAY_HAVE_SDL
# ifndef SDL_MAIN_HANDLED
#  define SDL_MAIN_HANDLED
# endif
# include <SDL.h>
#endif
#include "cast/CastDevices.h"
#include "cast/CastRelay.h"
#include "cast/HlsJoiner.h"
#include "cast/CastService.h"
#ifdef RELAY_HAVE_LIBAV
#include "cast/Remuxer.h"
#endif
#include "app/Languages.h"
#include "cast/TsSegmenter.h"
#include "vod/VodParser.h"
#include "epg/EpgService.h"
#include "vod/VodParser.h"
#include "sources/Catchup.h"
#include "sources/CatchupService.h"
#include "record/HlsPlaylist.h"
#include "record/Recorder.h"
#include "record/ScheduleService.h"

#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include "vod/VodService.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <memory>

// Tiny HTTP server for recording tests: /live.ts streams 64 KB chunks every
// 50 ms and never ends; /hls.m3u8 is a live playlist whose segments are
// /seg<N>.ts with 1000 bytes each.
class StreamServer : public QTcpServer
{
public:
    int hlsSequence = 10;
    int requests = 0;
    StreamServer() { listen(QHostAddress::LocalHost); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
protected:
    void incomingConnection(qintptr fd) override
    {
        auto *sock = new QTcpSocket(this);
        sock->setSocketDescriptor(fd);
        connect(sock, &QTcpSocket::readyRead, sock, [this, sock] {
            const QByteArray req = sock->readAll();
            const QByteArray path = req.split(' ').value(1);
            ++requests;
            if (path == "/live.ts") {
                sock->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\n\r\n");
                auto *t = new QTimer(sock);
                connect(t, &QTimer::timeout, sock, [sock] { sock->write(QByteArray(65536, 'G')); });
                t->start(50);
                return;
            }
            QByteArray body;
            if (path == "/hls.m3u8") {
                body = "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:" + QByteArray::number(hlsSequence) + "\n";
                for (int i = 0; i < 5; ++i)
                    body += "#EXTINF:1.0,\nseg" + QByteArray::number(hlsSequence + i) + ".ts\n";
                ++hlsSequence;
            } else if (path.startsWith("/seg")) {
                body = QByteArray(1000, 'S');
            } else {
                sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                sock->disconnectFromHost();
                return;
            }
            sock->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n" + body);
            sock->disconnectFromHost();
        });
    }
};


// Stand-in for GitHub's API: the latest release, and its installer behind a
// redirect to another host (localhost instead of 127.0.0.1), the way GitHub
// sends downloads elsewhere.
class GithubServer : public QTcpServer
{
public:
    QByteArray file = QByteArray(300000, 'R');
    QByteArray digest;            // "sha256:..." in the release, if set
    QByteArray requireToken;      // the API answers 404 without it
    QList<QByteArray> requests;   // "path|authorization"
    GithubServer() { listen(QHostAddress::Any); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
protected:
    void incomingConnection(qintptr fd) override
    {
        auto *sock = new QTcpSocket(this);
        sock->setSocketDescriptor(fd);
        connect(sock, &QTcpSocket::readyRead, sock, [this, sock] {
            const QByteArray req = sock->readAll();
            const QByteArray path = req.split(' ').value(1);
            QByteArray auth;
            for (const QByteArray &line : req.split('\n'))
                if (line.toLower().startsWith("authorization:")) auth = line.mid(14).trimmed();
            requests << path + '|' + auth;
            const QByteArray port = QByteArray::number(serverPort());
            auto send = [sock](const QByteArray &status, const QByteArray &body, const QByteArray &extra = {}) {
                sock->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + extra
                            + "Connection: close\r\n\r\n" + body);
                sock->disconnectFromHost();
            };
            if (path == "/repos/me/relay/releases/latest") {
                if (!requireToken.isEmpty() && auth != "Bearer " + requireToken) { send("404 Not Found", "{}"); return; }
                QJsonObject asset{{"name", "Relay-0.9.0-setup.exe"}, {"size", file.size()},
                                  {"url", QString::fromLatin1("http://127.0.0.1:" + port + "/repos/me/relay/releases/assets/7")}};
                if (!digest.isEmpty()) asset.insert("digest", QString::fromLatin1(digest));
                QJsonObject zip{{"name", "Relay-0.9.0-windows.zip"}, {"size", 5}, {"url", "http://x/zip"}};
                const QJsonObject rel{{"tag_name", "v0.9.0"}, {"html_url", "https://github.com/me/relay/releases/tag/v0.9.0"},
                                      {"body", "Better things"}, {"draft", false}, {"prerelease", false},
                                      {"assets", QJsonArray{zip, asset}}};
                send("200 OK", QJsonDocument(rel).toJson());
            } else if (path == "/repos/me/relay/releases/assets/7") {
                send("302 Found", "", "Location: http://localhost:" + port + "/download/setup.exe\r\n");
            } else if (path == "/download/setup.exe") {
                send("200 OK", file);
            } else {
                send("404 Not Found", "");
            }
        });
    }
};

// JSON written with ' for " (moc trips over raw string literals).
static QByteArray js(const char *text)
{
    QByteArray b(text);
    b.replace('\'', '"');
    return b;
}

// Stand-in for Trakt: device sign-in, token refresh and sign-out, keeping
// each request's path and JSON body. /users/settings turns the first call
// away (401), so Relay has to refresh its sign-in once.
class TraktServer : public QTcpServer
{
public:
    QList<QPair<QByteArray, QJsonObject>> requests;
    bool turnAwayOnce = true;
    TraktServer() { listen(QHostAddress::LocalHost); }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }
    QJsonObject last(const QByteArray &path) const
    {
        for (qsizetype i = requests.size() - 1; i >= 0; --i)
            if (requests[i].first == path) return requests[i].second;
        return {};
    }
    bool asked(const QByteArray &path) const
    {
        for (const auto &r : requests) if (r.first == path) return true;
        return false;
    }
protected:
    void incomingConnection(qintptr fd) override
    {
        auto *sock = new QTcpSocket(this);
        sock->setSocketDescriptor(fd);
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        auto buffer = std::make_shared<QByteArray>();
        connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buffer] {
            buffer->append(sock->readAll());
            const qsizetype end = buffer->indexOf("\r\n\r\n");
            if (end < 0) return;
            qsizetype length = 0;
            for (const QByteArray &line : buffer->left(end).split('\n'))
                if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
            if (buffer->size() < end + 4 + length) return;   // the body is still on its way
            const QByteArray path = buffer->left(end).split(' ').value(1);
            requests << qMakePair(path, QJsonDocument::fromJson(buffer->mid(end + 4, length)).object());
            buffer->clear();
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            auto tokens = [now](const char *access, const char *refresh) {
                return QJsonDocument(QJsonObject{{"access_token", access}, {"refresh_token", refresh},
                                                 {"expires_in", 604800}, {"created_at", now}}).toJson();
            };
            QByteArray status = "200 OK", reply = "[]";
            if (path == "/oauth/device/code")
                reply = js("{'device_code':'dev1','user_code':'ABCD1234','verification_url':'https://trakt.tv/activate',"
                           "'expires_in':600,'interval':1}");
            else if (path == "/oauth/device/token") reply = tokens("a1", "r1");
            else if (path == "/oauth/token") reply = tokens("a2", "r2");
            else if (path == "/oauth/revoke") reply = "";
            else if (path.startsWith("/search/movie?")) reply = js("[{'movie':{'title':'Harbor Lights','ids':{'trakt':7}}}]");
            else if (path.startsWith("/scrobble/")) status = "201 Created";
            else if (path == "/users/settings") {
                if (turnAwayOnce) { turnAwayOnce = false; status = "401 Unauthorized"; reply = "{}"; }
                else reply = js("{'user':{'username':'viewer'}}");
            }
            sock->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply);
            sock->disconnectFromHost();
        });
    }
};

class TestSources : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("RelayTests"));
        QCoreApplication::setApplicationName(QStringLiteral("RelayTests"));
        QSettings().clear();
    }

    // ---- M3U ----

    void m3u_basic()
    {
        const QByteArray data =
            "\xEF\xBB\xBF#EXTM3U url-tvg=\"http://epg.example/guide.xml\"\r\n"
            "#EXTINF:-1 tvg-id=\"news.us\" tvg-name=\"News HD\" tvg-logo=\"http://l/n.png\" "
            "group-title=\"News, Weather\",News HD\r\n"
            "http://s/1.ts\r\n"
            "#EXTINF:-1 tvg-chno=\"42\" group-title=\"Sports\",Sports One\n"
            "#EXTVLCOPT:http-user-agent=CustomUA/1.0\n"
            "#EXTVLCOPT:http-referrer=http://ref.example/\n"
            "http://s/2.m3u8\n";
        const PlaylistResult r = M3uParser::parse(data);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.epgUrl, QStringLiteral("http://epg.example/guide.xml"));
        QCOMPARE(r.channels.size(), 2);

        const Channel &a = r.channels[0];
        QCOMPARE(a.name, QStringLiteral("News HD"));
        QCOMPARE(a.group, QStringLiteral("News, Weather"));   // comma inside quotes
        QCOMPARE(a.tvgId, QStringLiteral("news.us"));
        QCOMPARE(a.logo, QStringLiteral("http://l/n.png"));
        QCOMPARE(a.url, QStringLiteral("http://s/1.ts"));
        QCOMPARE(a.number, 1);

        const Channel &b = r.channels[1];
        QCOMPARE(b.number, 42);
        QCOMPARE(b.userAgent, QStringLiteral("CustomUA/1.0"));
        QCOMPARE(b.referrer, QStringLiteral("http://ref.example/"));
    }

    void m3u_extgrp_and_missing_name()
    {
        const QByteArray data =
            "#EXTM3U\n"
            "#EXTINF:0 tvg-name=\"From Attr\",\n"
            "#EXTGRP:Movies\n"
            "http://s/movie.ts\n"
            "http://s/bare/stream.m3u8\n";
        const PlaylistResult r = M3uParser::parse(data);
        QCOMPARE(r.channels.size(), 2);
        QCOMPARE(r.channels[0].name, QStringLiteral("From Attr"));
        QCOMPARE(r.channels[0].group, QStringLiteral("Movies"));
        QCOMPARE(r.channels[1].name, QStringLiteral("stream.m3u8"));
        QCOMPARE(r.channels[1].group, QStringLiteral("Uncategorized"));
    }

    void m3u_catchup_attrs()
    {
        const QByteArray data =
            "#EXTM3U\n"
            "#EXTINF:-1 catchup=\"shift\" catchup-days=\"7\" group-title=Kids,Cartoons\n"
            "http://s/c.ts\n";
        const PlaylistResult r = M3uParser::parse(data);
        QCOMPARE(r.channels.size(), 1);
        QCOMPARE(r.channels[0].catchupType, QStringLiteral("shift"));
        QCOMPARE(r.channels[0].catchupDays, 7);
        QCOMPARE(r.channels[0].group, QStringLiteral("Kids"));   // unquoted value
        QCOMPARE(r.channels[0].name, QStringLiteral("Cartoons"));
    }

    void m3u_skips_movies_and_series()
    {
        const QByteArray data =
            "#EXTM3U\n"
            "#EXTINF:-1 group-title=\"News\",News One\nhttp://h:80/user/pass/101\n"
            "#EXTINF:-1 group-title=\"Sports\",Sports One\nhttp://h:80/live/user/pass/102.ts\n"
            "#EXTINF:-1 group-title=\"Movies\",Some Film (2020)\nhttp://h:80/movie/user/pass/9001.mkv\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E01\nhttp://h:80/series/user/pass/7001.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E02\nhttp://h:80/series/user/pass/7002.mp4\n"
            "#EXTINF:-1 group-title=\"Movies\",Other Film\nhttp://cdn.example/files/other.mp4?token=1\n"
            "#EXTINF:-1 group-title=\"HLS\",Live HLS\nhttp://h:80/live/user/pass/103.m3u8\n";
        const PlaylistResult r = M3uParser::parse(data);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.channels.size(), 3);
        QCOMPARE(r.channels[0].name, QStringLiteral("News One"));
        QCOMPARE(r.channels[2].name, QStringLiteral("Live HLS"));
        QCOMPARE(r.channels[2].number, 3);   // numbered after filtering
        QCOMPARE(r.skippedMovies, 2);
        QCOMPARE(r.skippedSeries, 2);
        QCOMPARE(r.vodMovies.size(), 2);     // kept for the Movies screen
        QCOMPARE(r.vodEpisodes.size(), 2);
    }

    void m3u_keeps_all_vod_playlist()
    {
        // A list of nothing but video files isn't emptied.
        const PlaylistResult r = M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1,Home Video\nfile:///videos/a.mp4\n"
            "#EXTINF:-1,Trip\nfile:///videos/b.mkv\n");
        QCOMPARE(r.channels.size(), 2);
        QCOMPARE(r.skippedMovies, 0);
    }

    void m3u_rejects_garbage()
    {
        QVERIFY(!M3uParser::parse("<html>Not found</html>").error.isEmpty());
        QVERIFY(!M3uParser::parse("#EXTM3U\n").error.isEmpty());
        QVERIFY(!M3uParser::parse("").error.isEmpty());
    }

    // ---- Xtream ----

    void xtream_urls()
    {
        XtreamParser::Credentials c{QStringLiteral("example.com:8080/"), QStringLiteral("u@1"),
                                    QStringLiteral("p&w"), QStringLiteral("ts")};
        QCOMPARE(XtreamParser::normalizeServer(c.server).toString(),
                 QStringLiteral("http://example.com:8080"));
        QCOMPARE(XtreamParser::normalizeServer(QStringLiteral("https://x.tv/player_api.php")).toString(),
                 QStringLiteral("https://x.tv"));
        const QUrl api = XtreamParser::apiUrl(c, QStringLiteral("get_live_streams"));
        QCOMPARE(api.path(), QStringLiteral("/player_api.php"));
        QVERIFY(api.query(QUrl::FullyEncoded).contains(QStringLiteral("action=get_live_streams")));
        QVERIFY(api.query(QUrl::FullyEncoded).contains(QStringLiteral("password=p%26w")));
    }

    void xtream_guide_from_m3u_link()
    {
        QCOMPARE(XtreamParser::guideUrlForPlaylist(QUrl(QStringLiteral(
                     "http://srv.example:8080/get.php?username=bob&password=p%26w&type=m3u_plus&output=ts"))),
                 QStringLiteral("http://srv.example:8080/xmltv.php?username=bob&password=p%26w"));
        QCOMPARE(XtreamParser::guideUrlForPlaylist(QUrl(QStringLiteral(
                     "https://x.tv/sub/get.php?username=a&password=b"))),
                 QStringLiteral("https://x.tv/sub/xmltv.php?username=a&password=b"));
        QVERIFY(XtreamParser::guideUrlForPlaylist(QUrl(QStringLiteral("http://x.tv/list.m3u"))).isEmpty());
        QVERIFY(XtreamParser::guideUrlForPlaylist(QUrl(QStringLiteral("http://x.tv/get.php?username=a"))).isEmpty());
    }

    void redact()
    {
        QCOMPARE(Log::redact(QStringLiteral("http://h/xmltv.php?username=u&password=secret")),
                 QStringLiteral("http://h/xmltv.php?username=~&password=***"));
        QCOMPARE(Log::redact(QStringLiteral("http://h:80/live/user/secret/101.ts")),
                 QStringLiteral("http://h:80/live/~/***/101.ts"));
        QCOMPARE(Log::redact(QStringLiteral("Playlist: loading http://name:pw@h/get.php?type=m3u")),
                 QStringLiteral("Playlist: loading http://~@h/get.php?type=m3u"));
        QCOMPARE(Log::redact(QStringLiteral("Tuned: 1 News HD")), QStringLiteral("Tuned: 1 News HD"));
    }

    void xtream_auth()
    {
        QVERIFY(XtreamParser::parseAuth("{\"user_info\":{\"auth\":1,\"status\":\"Active\"}}").ok);
        QVERIFY(!XtreamParser::parseAuth("{\"user_info\":{\"auth\":0}}").ok);
        QVERIFY(!XtreamParser::parseAuth("{\"user_info\":{\"auth\":1,\"status\":\"Expired\"}}").ok);
        QVERIFY(!XtreamParser::parseAuth("not json").ok);
    }

    void xtream_streams()
    {
        QStringList order;
        const auto cats = XtreamParser::parseCategories(
            "[{\"category_id\":\"2\",\"category_name\":\"Sports\"},{\"category_id\":1,\"category_name\":\"News\"}]",
            &order);
        QCOMPARE(order, (QStringList{QStringLiteral("Sports"), QStringLiteral("News")}));

        const QByteArray streams = "[\n"
            "            {\"num\":1,\"name\":\"CNN\",\"stream_id\":101,\"stream_icon\":\"http://l/cnn.png\",\n"
            "             \"epg_channel_id\":\"cnn.us\",\"category_id\":\"1\",\"tv_archive\":1,\"tv_archive_duration\":\"3\"},\n"
            "            {\"num\":\"2\",\"name\":\"ESPN\",\"stream_id\":\"202\",\"category_id\":\"2\"},\n"
            "            {\"num\":3,\"name\":\"Broken\"}\n"
            "        ]";
        XtreamParser::Credentials c{QStringLiteral("http://srv:80"), QStringLiteral("user"),
                                    QStringLiteral("pass"), QStringLiteral("m3u8")};
        const PlaylistResult r = XtreamParser::parseLiveStreams(streams, cats, c, order);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.channels.size(), 2);
        QCOMPARE(r.channels[0].url, QStringLiteral("http://srv:80/live/user/pass/101.m3u8"));
        QCOMPARE(r.channels[0].group, QStringLiteral("News"));
        QCOMPARE(r.channels[0].catchupDays, 3);
        QCOMPARE(r.channels[1].number, 2);
        QCOMPARE(r.channels[1].group, QStringLiteral("Sports"));
        QVERIFY(r.epgUrl.startsWith(QStringLiteral("http://srv:80/xmltv.php")));
        QCOMPARE(r.groupOrder, order);
    }

    // ---- Model + controller ----

    // Before a playlist loads, "All channels" is selected and empty (Relay
    // 0.2.1 read past the end of its group lists here and crashed on some PCs).
    void model_before_a_playlist()
    {
        ChannelModel model;
        QCOMPARE(model.currentGroup(), int(ChannelModel::AllGroup));
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.data(model.index(0), ChannelModel::NameRole).isValid());
        QCOMPARE(model.globalIndexAt(0), -1);
        QCOMPARE(model.rowOf(0), -1);
        QCOMPARE(model.findByNumber(1), -1);
        for (int g = 0; g < 3; ++g) model.setCurrentGroup(g);
        QCOMPARE(model.rowCount(), 0);
    }

    void model_groups_and_zapping()
    {
        ChannelModel model;
        model.setPlaylist(M3uParser::parse(sample()));
        // Favorites, "All channels", then groups in order of appearance.
        QCOMPARE(model.groups().size(), 5);
        QCOMPARE(model.groupName(3), QStringLiteral("News"));
        QCOMPARE(model.currentGroup(), int(ChannelModel::AllGroup));
        QCOMPARE(model.count(), 4);

        model.setCurrentGroup(4);   // Sports
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.globalIndexAt(0), 1);
        QCOMPARE(model.rowOf(0), -1);

        // Zapping wraps inside the group.
        QCOMPARE(model.step(1, +1, 4), 3);
        QCOMPARE(model.step(3, +1, 4), 1);
        QCOMPARE(model.step(1, -1, 4), 3);
        QCOMPARE(model.findByNumber(4), 3);
    }

    void controller_number_entry_and_previous()
    {
        ChannelModel model;
        AppSettings settings;
        settings.saveM3u(QStringLiteral("http://test/list.m3u"));   // a playlist to remember the channel in
        model.setPlaylist(M3uParser::parse(sample()));
        PlayerController player(&model, &settings);
        QSignalSpy plays(&player, &PlayerController::playRequested);
        QSignalSpy missing(&player, &PlayerController::numberNotFound);

        player.restoreAfterLoad();
        QCOMPARE(player.currentIndex(), 0);
        QVERIFY(!player.hasPrevious());

        player.tune(1, 4);          // Sports One, picked from the Sports group
        player.channelUp();         // stays in Sports -> Sports Two (index 3)
        QCOMPARE(player.currentIndex(), 3);
        player.previousChannel();
        QCOMPARE(player.currentIndex(), 1);

        player.typeDigit(3);
        QCOMPARE(player.pendingNumber(), QStringLiteral("3"));
        player.commitNumber();
        QCOMPARE(player.currentIndex(), 2);

        player.typeDigit(9);
        player.typeDigit(9);
        player.commitNumber();
        QCOMPARE(missing.count(), 1);
        QCOMPARE(player.currentIndex(), 2);
        QCOMPARE(plays.count(), 5);

        // Last channel is remembered across loads.
        PlayerController again(&model, &settings);
        again.restoreAfterLoad();
        QCOMPARE(again.currentIndex(), 2);
    }

    void channel_keys()
    {
        Channel a; a.url = QStringLiteral("http://h:80/user/pass/12345");
        Channel b; b.url = QStringLiteral("http://other-domain:8080/live/user/newpass/12345.ts");
        Channel c; c.url = QStringLiteral("http://cdn.example/news/index.m3u8");
        QCOMPARE(ChannelModel::channelKey(a), QStringLiteral("s:12345"));
        QCOMPARE(ChannelModel::channelKey(b), ChannelModel::channelKey(a));   // survives domain/password changes
        QCOMPARE(ChannelModel::channelKey(c), QStringLiteral("u:http://cdn.example/news/index.m3u8"));
    }

    void favorites_and_hiding()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("library.json"));
        {
            UserLibrary library(path);
            ChannelModel model;
            model.setLibrary(&library);
            model.setPlaylist(M3uParser::parse(sample()));
            AppSettings settings;
            PlayerController player(&model, &settings);

            QCOMPARE(model.toggleFavorite(3), true);   // Sports Two
            QCOMPARE(model.toggleFavorite(0), true);   // News One
            model.setCurrentGroup(ChannelModel::FavoritesGroup);
            QCOMPARE(model.count(), 2);
            QCOMPARE(model.globalIndexAt(0), 3);       // order added
            QVERIFY(model.isFavorite(3));

            // Zapping inside Favorites survives a group being hidden (indexes shift).
            player.tune(3, ChannelModel::FavoritesGroup);
            model.hideGroup(QStringLiteral("News"));
            QCOMPARE(model.groupIndex(QStringLiteral("News")), -1);
            QCOMPARE(model.groupIndex(QStringLiteral("Sports")), 3);
            QCOMPARE(model.count(), 2);                // favorites still listed
            player.channelUp();
            QCOMPARE(player.currentIndex(), 0);        // News One, via Favorites

            // Hidden group's channels leave All channels and number entry.
            model.setCurrentGroup(ChannelModel::AllGroup);
            QCOMPARE(model.count(), 2);
            QCOMPARE(model.findByNumber(1), -1);
            QCOMPARE(model.hiddenGroupsList().size(), 1);

            // Hiding a channel removes it from favorites too.
            model.hideChannel(3);
            QCOMPARE(model.count(), 1);
            QVERIFY(!model.isFavorite(3));
            QCOMPARE(model.hiddenChannelsList().size(), 1);
            library.saveNow();
        }
        {
            // Everything comes back after a restart / playlist refresh.
            UserLibrary library(path);
            ChannelModel model;
            model.setLibrary(&library);
            model.setPlaylist(M3uParser::parse(sample()));
            QCOMPARE(model.groupIndex(QStringLiteral("News")), -1);
            QCOMPARE(model.count(), 1);                // All channels: Sports One
            QVERIFY(model.isFavorite(0));
            QVERIFY(!model.isFavorite(3));

            model.unhideGroup(QStringLiteral("News"));
            model.unhideChannel(model.hiddenChannelsList().first().toMap().value("key").toString());
            QCOMPARE(model.count(), 4);
            QCOMPARE(model.groupIndex(QStringLiteral("News")), 3);
        }
    }

    void rename_reorder_and_search()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("library.json"));
        const QByteArray playlist =
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-chno=\"1\" group-title=\"News\",News One\nhttp://h/u/p/101\n"
            "#EXTINF:-1 tvg-chno=\"2\" group-title=\"Sports\",Sports Zeta\nhttp://h/u/p/102\n"
            "#EXTINF:-1 tvg-chno=\"3\" group-title=\"News\",Télé Matin\nhttp://h/u/p/103\n"
            "#EXTINF:-1 tvg-chno=\"4\" group-title=\"Sports\",Sports Alpha\nhttp://h/u/p/104\n"
            "#EXTINF:-1 tvg-chno=\"5\" group-title=\"Kids\",Cartoons\nhttp://h/u/p/105\n";
        {
            UserLibrary library(path);
            ChannelModel model;
            model.setLibrary(&library);
            model.setPlaylist(M3uParser::parse(playlist));

            // Search: accents/case ignored, all words must match, numbers rank first.
            QCOMPARE(model.searchChannels(QStringLiteral("tele")).size(), 1);
            QCOMPARE(model.searchChannels(QStringLiteral("SPORTS al")).size(), 1);
            QCOMPARE(model.searchChannels(QStringLiteral("sports")).size(), 2);
            const QVariantList byNumber = model.searchChannels(QStringLiteral("4"));
            QCOMPARE(byNumber.first().toMap().value("name").toString(), QStringLiteral("Sports Alpha"));

            // Rename a channel and a group; ids stay the same for hiding/zapping.
            model.renameChannel(0, QStringLiteral("Morning News"));
            QCOMPARE(model.channel(0)->name, QStringLiteral("Morning News"));
            QCOMPARE(model.originalChannels()[0].name, QStringLiteral("News One"));
            QCOMPARE(model.searchChannels(QStringLiteral("morning")).size(), 1);
            const int sports = model.groupIndex(QStringLiteral("Sports"));
            model.renameGroup(sports, QStringLiteral("Live Sport"));
            QCOMPARE(model.groupLabel(sports), QStringLiteral("Live Sport"));
            QCOMPARE(model.groupName(sports), QStringLiteral("Sports"));
            QCOMPARE(model.groups()[sports].toMap().value("name").toString(), QStringLiteral("Live Sport"));

            // Reorder groups: Kids up to the top of the playlist groups.
            int kids = model.groupIndex(QStringLiteral("Kids"));
            QCOMPARE(kids, 5);
            kids = model.moveGroup(kids, -1);
            kids = model.moveGroup(kids, -1);
            QCOMPARE(kids, 3);
            QCOMPARE(model.moveGroup(kids, -1), 3);   // can't pass All channels

            // Reorder channels inside Sports, then sort A–Z and reset.
            model.setCurrentGroup(model.groupIndex(QStringLiteral("Sports")));
            QCOMPARE(model.globalIndexAt(0), 1);      // Sports Zeta first (playlist order)
            QCOMPARE(model.moveChannel(1, -1), 0);
            QCOMPARE(model.globalIndexAt(0), 3);
            model.resetGroupOrder(model.currentGroup());
            QCOMPARE(model.globalIndexAt(0), 1);
            model.sortGroup(model.currentGroup());
            QCOMPARE(model.globalIndexAt(0), 3);      // Alpha before Zeta

            // Favorites order.
            model.toggleFavorite(4);
            model.toggleFavorite(2);
            model.setCurrentGroup(ChannelModel::FavoritesGroup);
            QCOMPARE(model.globalIndexAt(0), 4);
            QCOMPARE(model.moveChannel(1, -1), 0);
            QCOMPARE(model.globalIndexAt(0), 2);

            // All channels can't be reordered.
            model.setCurrentGroup(ChannelModel::AllGroup);
            QVERIFY(!model.canMoveChannels());
            QCOMPARE(model.moveChannel(0, 1), 0);

            // Recently watched: newest first, no duplicates; while zapping
            // inside it the order on screen waits for refreshRecent().
            model.noteWatched(1, false);
            model.noteWatched(4, false);
            model.noteWatched(1, false);
            model.setCurrentGroup(ChannelModel::RecentGroup);
            QCOMPARE(model.count(), 2);
            QCOMPARE(model.globalIndexAt(0), 1);
            QVERIFY(!model.canMoveChannels());
            model.noteWatched(4, true);
            QCOMPARE(model.globalIndexAt(0), 1);
            model.refreshRecent();
            QCOMPARE(model.globalIndexAt(0), 4);
            library.saveNow();
        }
        {
            // Everything survives a restart / playlist refresh.
            UserLibrary library(path);
            ChannelModel model;
            model.setLibrary(&library);
            model.setPlaylist(M3uParser::parse(playlist));
            QCOMPARE(model.channel(0)->name, QStringLiteral("Morning News"));
            QCOMPARE(model.groupIndex(QStringLiteral("Kids")), 3);
            model.setCurrentGroup(ChannelModel::RecentGroup);
            QCOMPARE(model.globalIndexAt(0), 4);       // remembered
            QCOMPARE(model.groupLabel(model.groupIndex(QStringLiteral("Sports"))), QStringLiteral("Live Sport"));
            model.setCurrentGroup(model.groupIndex(QStringLiteral("Sports")));
            QCOMPARE(model.globalIndexAt(0), 3);
            model.setCurrentGroup(ChannelModel::FavoritesGroup);
            QCOMPARE(model.globalIndexAt(0), 2);

            // Clearing a name restores the original.
            model.renameChannel(0, QString());
            QCOMPARE(model.channel(0)->name, QStringLiteral("News One"));
        }
    }
    void xtream_credentials_from_m3u_link()
    {
        XtreamParser::Credentials c;
        QVERIFY(XtreamParser::credentialsFromPlaylistUrl(
            QUrl(QStringLiteral("http://tv.example:8080/get.php?username=me&password=p%40ss&type=m3u_plus&output=ts")), &c));
        QCOMPARE(c.server, QStringLiteral("http://tv.example:8080"));
        QCOMPARE(c.username, QStringLiteral("me"));
        QCOMPARE(c.password, QStringLiteral("p@ss"));
        QCOMPARE(XtreamParser::apiUrl(c, QStringLiteral("get_vod_streams")).toString(),
                 QStringLiteral("http://tv.example:8080/player_api.php?username=me&password=p@ss&action=get_vod_streams"));
        QVERIFY(!XtreamParser::credentialsFromPlaylistUrl(QUrl(QStringLiteral("http://x/list.m3u")), &c));
        QVERIFY(!XtreamParser::credentialsFromPlaylistUrl(QUrl(QStringLiteral("file:///get.php?username=a&password=b")), &c));
    }

    void vod_lists()
    {
        const QByteArray cats = "[{\"category_id\":\"5\",\"category_name\":\"Action\"},"
                                "{\"category_id\":6,\"category_name\":\"Drama\"},{\"category_id\":\"5\",\"category_name\":\"dup\"}]";
        const auto c = VodParser::parseCategories(cats);
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[1].id, QStringLiteral("6"));
        QVERIFY(VodParser::isList("[]"));
        QVERIFY(!VodParser::isList("{\"user_info\":{}}"));
        QVERIFY(!VodParser::isList("<html>"));

        const QByteArray movies =
            "[{\"num\":1,\"name\":\"Big Film (2019)\",\"stream_id\":101,\"stream_icon\":\"http://i/1.jpg\","
            "\"rating\":\"7.4\",\"added\":\"1700000000\",\"category_id\":\"5\",\"container_extension\":\"mkv\"},"
            "{\"name\":\"No id\"},"
            "{\"name\":\"Second\",\"stream_id\":\"102\",\"rating\":null,\"year\":\"2021\",\"category_id\":6}]";
        const auto m = VodParser::parseMovies(movies);
        QCOMPARE(m.size(), 2);
        QCOMPARE(m[0].id, QStringLiteral("101"));
        QCOMPARE(m[0].year, QStringLiteral("2019"));
        QCOMPARE(m[0].ext, QStringLiteral("mkv"));
        QCOMPARE(m[0].added, qint64(1700000000));
        QVERIFY(qAbs(m[0].rating - 7.4) < 0.001);
        QCOMPARE(m[1].year, QStringLiteral("2021"));
        QCOMPARE(m[1].categoryId, QStringLiteral("6"));

        const auto s = VodParser::parseSeries(
            "[{\"name\":\"Show\",\"series_id\":9,\"cover\":\"http://i/s.jpg\",\"releaseDate\":\"2015-02-01\",\"category_id\":\"3\"}]");
        QCOMPARE(s.size(), 1);
        QCOMPARE(s[0].poster, QStringLiteral("http://i/s.jpg"));
        QCOMPARE(s[0].year, QStringLiteral("2015"));
    }

    void vod_details()
    {
        XtreamParser::Credentials c{QStringLiteral("http://h:80/"), QStringLiteral("u"), QStringLiteral("p")};
        VodItem film;
        film.id = QStringLiteral("101");
        film.title = QStringLiteral("Big Film");
        film.ext = QStringLiteral("mkv");
        const QVariantMap d = VodParser::parseMovieInfo(
            "{\"info\":{\"plot\":\"A plot.\",\"genre\":\"Action\",\"duration_secs\":6300,\"rating\":\"8\","
            "\"cast\":\"A, B\",\"backdrop_path\":[\"http://i/b.jpg\"],\"releasedate\":\"2019-05-03\"},"
            "\"movie_data\":{\"stream_id\":101,\"container_extension\":\"mp4\"}}", c, film);
        QCOMPARE(d.value("plot").toString(), QStringLiteral("A plot."));
        QCOMPARE(d.value("duration").toString(), QStringLiteral("1 h 45 min"));
        QCOMPARE(d.value("backdrop").toString(), QStringLiteral("http://i/b.jpg"));
        QCOMPARE(d.value("year").toString(), QStringLiteral("2019"));
        QCOMPARE(d.value("url").toString(), QStringLiteral("http://h:80/movie/u/p/101.mp4"));
        // Missing info still gives a playable address.
        QCOMPARE(VodParser::parseMovieInfo("not json", c, film).value("url").toString(),
                 QStringLiteral("http://h:80/movie/u/p/101.mkv"));

        VodItem show;
        show.id = QStringLiteral("9");
        show.title = QStringLiteral("Show");
        // Episodes keyed by season (the usual shape), out of order.
        const QVariantMap s = VodParser::parseSeriesInfo(
            "{\"seasons\":[{\"season_number\":2,\"name\":\"Second Season\"}],\"info\":{\"plot\":\"P\"},"
            "\"episodes\":{\"2\":[{\"id\":\"21\",\"episode_num\":1,\"title\":\"Two-one\",\"container_extension\":\"mkv\"}],"
            "\"1\":[{\"id\":\"12\",\"episode_num\":2,\"title\":\"One-two\",\"container_extension\":\"mp4\",\"info\":{\"duration\":\"00:42:00\"}},"
            "{\"id\":\"11\",\"episode_num\":1,\"title\":\"One-one\",\"container_extension\":\"mp4\"}]}}", c, show);
        const QVariantList eps = s.value("episodes").toList();
        QCOMPARE(eps.size(), 3);
        QCOMPARE(eps[0].toMap().value("id").toString(), QStringLiteral("11"));
        QCOMPARE(eps[1].toMap().value("duration").toString(), QStringLiteral("42 min"));
        QCOMPARE(eps[2].toMap().value("url").toString(), QStringLiteral("http://h:80/series/u/p/21.mkv"));
        // "Show - S01E02 - Title" keeps just the title; other names are left alone.
        const QVariantMap named = VodParser::parseSeriesInfo(
            "{\"episodes\":{\"1\":[{\"id\":\"1\",\"episode_num\":1,\"title\":\"Show - S01E01 - Pilot\"},"
            "{\"id\":\"2\",\"episode_num\":2,\"title\":\"S01 E02\"},"
            "{\"id\":\"3\",\"episode_num\":3,\"title\":\"The Return\"}]}}", c, show);
        const QVariantList ne = named.value("episodes").toList();
        QCOMPARE(ne[0].toMap().value("title").toString(), QStringLiteral("Pilot"));
        QCOMPARE(ne[1].toMap().value("title").toString(), QString());
        QCOMPARE(ne[2].toMap().value("title").toString(), QStringLiteral("The Return"));
        const QVariantList seasons = s.value("seasons").toList();
        QCOMPARE(seasons.size(), 2);
        QCOMPARE(seasons[0].toMap().value("name").toString(), QStringLiteral("Season 1"));
        QCOMPARE(seasons[1].toMap().value("name").toString(), QStringLiteral("Second Season"));
        // Some panels send a flat array.
        const QVariantMap flat = VodParser::parseSeriesInfo(
            "{\"episodes\":[{\"id\":\"5\",\"season\":3,\"episode_num\":4}]}", c, show);
        QCOMPARE(flat.value("episodes").toList().first().toMap().value("season").toInt(), 3);
    }

    void vod_m3u_catalogue()
    {
        const PlaylistResult r = M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1,Live\nhttp://h/live/u/p/1.ts\n"
            "#EXTINF:-1 group-title=\"Films\" tvg-logo=\"http://i/f.jpg\",Film (2020)\nhttp://h/movie/u/p/50.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",My Show S01 E02\nhttp://h/series/u/p/62.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",My Show S01E01 - Pilot\nhttp://h/series/u/p/61.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",My Show S02 E01\nhttp://h/series/u/p/63.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Other.Show.S01E01\nhttp://h/series/u/p/70.mp4\n");
        const auto cat = VodParser::buildM3uCatalogue(r.vodMovies, r.vodEpisodes);
        QCOMPARE(cat.movies.size(), 1);
        QCOMPARE(cat.movies[0].year, QStringLiteral("2020"));
        QCOMPARE(cat.movies[0].id, QStringLiteral("u:http://h/movie/u/p/50.mp4"));
        QCOMPARE(cat.series.size(), 2);
        QCOMPARE(cat.series[0].title, QStringLiteral("My Show"));
        QCOMPARE(cat.series[1].title, QStringLiteral("Other.Show"));
        const QVariantList eps = cat.episodes.value(cat.series[0].id);
        QCOMPARE(eps.size(), 3);
        QCOMPARE(eps[1].toMap().value("title").toString(), QStringLiteral("Pilot"));
        QCOMPARE(eps[1].toMap().value("episode").toInt(), 1);
        QCOMPARE(cat.seriesCategories, QStringList{QStringLiteral("Shows")});
    }

    void vod_combined_sources()
    {
        QCOMPARE(VodService::qualityOf("Alpha (2020) [4K]"), 4);
        QCOMPARE(VodService::qualityOf("EN - Beta FHD"), 3);
        QCOMPARE(VodService::qualityOf("Gamma 720p"), 2);
        QCOMPARE(VodService::qualityOf("Hidden Figures"), 0);

        QTemporaryDir dir;
        UserLibrary lib(dir.filePath("library.json"));
        VodService vod(&lib, dir.filePath("vod"));
        vod.setCombine(true);
        vod.setOwnSource("p1", "Home");
        // This playlist: Alpha in HD, Beta twice (HD and FHD), a show with two episodes.
        vod.setSource(M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1,Live\nhttp://h/live/u/p/1.ts\n"
            "#EXTINF:-1 group-title=\"Films\",Alpha (2020)\nhttp://one/alpha.mp4\n"
            "#EXTINF:-1 group-title=\"Films\",Beta\nhttp://one/beta.mp4\n"
            "#EXTINF:-1 group-title=\"Films FHD\",Beta FHD\nhttp://one/beta-fhd.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E01\nhttp://one/series/u/p/11.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E02\nhttp://one/series/u/p/12.mp4\n"));
        vod.ensureLoaded();
        QTRY_VERIFY(vod.loaded());
        QTRY_COMPARE(vod.movieCount(), 2);                     // Beta once (combined in the background)
        // Another playlist: Alpha in 4K, Delta, and the show with a third episode.
        vod.addExtraSource("p2", "Other", M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1,Live\nhttp://h2/live/u/p/1.ts\n"
            "#EXTINF:-1 group-title=\"Movies\",EN - Alpha (2020) [4K]\nhttp://two/alpha4k.mp4\n"
            "#EXTINF:-1 group-title=\"Movies\",Delta\nhttp://two/delta.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E01\nhttp://two/series/u/p/21.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E02\nhttp://two/series/u/p/22.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E03\nhttp://two/series/u/p/23.mp4\n"));
        vod.ensureLoaded();
        QTRY_COMPARE(vod.movieCount(), 3);                     // Alpha, Beta, Delta
        QCOMPARE(vod.seriesCount(), 1);

        vod.setSort(2);
        QString alpha, delta;
        for (int i = 0; i < vod.items()->count(); ++i) {
            const QVariantMap r = vod.rowAt(i);
            if (r.value("title").toString().startsWith("Alpha")) alpha = r.value("id").toString();
            if (r.value("title").toString() == "Delta") delta = r.value("id").toString();
        }
        QVERIFY(!alpha.startsWith('@'));                       // this playlist's title
        QVERIFY(delta.startsWith("@p2:"));                     // only the other one has it
        const QVariantList src = vod.sources("movie", alpha);
        QCOMPARE(src.size(), 2);
        QCOMPARE(src[1].toMap().value("quality").toString(), QString("4K"));
        QVERIFY(src[1].toMap().value("best").toBool());
        QCOMPARE(src[1].toMap().value("name").toString(), QString("Other"));

        // Plays from the best source (4K, the other playlist), or the one asked for.
        QSignalSpy details(&vod, &VodService::detailsReady);
        QSignalSpy ready(&vod, &VodService::playbackReady);
        vod.requestDetails("movie", alpha);
        QTRY_COMPARE(details.count(), 1);
        const QVariantMap d = details.takeFirst().first().toMap();
        vod.startPlayback(d);
        QTRY_COMPARE(ready.count(), 1);
        QVariantMap play = ready.takeFirst().first().toMap();
        QCOMPARE(play.value("url").toString(), QString("http://two/alpha4k.mp4"));
        QCOMPARE(play.value("sourceIndex").toInt(), 1);
        QCOMPARE(play.value("sourceCount").toInt(), 2);
        QCOMPARE(play.value("key").toString(), "movie:" + alpha);   // its place belongs to the title
        vod.startPlayback(d, -1, false, 0);
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.takeFirst().first().toMap().value("url").toString(), QString("http://one/alpha.mp4"));
        // Switching carries on where it was.
        vod.switchSource(play, 0, 321);
        QTRY_COMPARE(ready.count(), 1);
        play = ready.takeFirst().first().toMap();
        QCOMPARE(play.value("url").toString(), QString("http://one/alpha.mp4"));
        QCOMPARE(play.value("start").toDouble(), 321.0);

        // A title only the other playlist has: its page and its address.
        vod.requestDetails("movie", delta);
        QTRY_COMPARE(details.count(), 1);
        const QVariantMap dd = details.takeFirst().first().toMap();
        QCOMPARE(dd.value("id").toString(), delta);
        QCOMPARE(dd.value("url").toString(), QString("http://two/delta.mp4"));
        QCOMPARE(dd.value("key").toString(), "movie:" + delta);

        // The same episode from the other source.
        const QString show = vod.shelves(10).isEmpty() ? QString() : QString();
        Q_UNUSED(show);
        vod.setSection(1);
        const QString series = vod.rowAt(0).value("id").toString();
        QCOMPARE(vod.sources("series", series).size(), 2);
        vod.requestDetails("series", series);
        QTRY_COMPARE(details.count(), 1);
        const QVariantMap sd = details.takeFirst().first().toMap();
        vod.startPlayback(sd, 1, false, 1);
        QTRY_COMPARE(ready.count(), 1);
        play = ready.takeFirst().first().toMap();
        QCOMPARE(play.value("url").toString(), QString("http://two/series/u/p/22.mp4"));
        QCOMPARE(play.value("episode").toInt(), 2);

        // Off: just this playlist (Beta twice again).
        vod.setCombine(false);
        vod.setSection(0);
        QCOMPARE(vod.movieCount(), 3);
        QCOMPARE(vod.items()->count(), 3);
        QVERIFY(vod.sources("movie", alpha).isEmpty());
        vod.setCombine(true);
    }

    void vod_service_m3u_progress_and_favorites()
    {
        QTemporaryDir dir;
        UserLibrary lib(dir.filePath("library.json"));
        VodService vod(&lib, dir.filePath("vod"));
        const PlaylistResult r = M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1,Live\nhttp://h/live/u/p/1.ts\n"
            "#EXTINF:-1 group-title=\"Films\",Alpha Film\nhttp://cdn/a.mp4\n"
            "#EXTINF:-1 group-title=\"Films\",Beta Film\nhttp://cdn/b.mp4\n"
            "#EXTINF:-1 group-title=\"Other\",Gamma\nhttp://cdn/c.mkv\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E01\nhttp://h/series/u/p/1.mp4\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E02\nhttp://h/series/u/p/2.mp4\n");
        vod.setSource(r);
        QVERIFY(vod.available());
        QSignalSpy state(&vod, &VodService::stateChanged);
        vod.ensureLoaded();
        QTRY_VERIFY(vod.loaded());
        QCOMPARE(vod.movieCount(), 3);
        QCOMPARE(vod.seriesCount(), 1);
        QCOMPARE(vod.items()->count(), 3);

        vod.setFilter(QStringLiteral("film"));
        QCOMPARE(vod.items()->count(), 2);
        vod.setFilter(QString());
        vod.setCategory(QStringLiteral("c:Other"));
        QCOMPARE(vod.items()->count(), 1);
        vod.setCategory(QStringLiteral("all"));
        vod.setSort(2);
        QCOMPARE(vod.rowAt(0).value("title").toString(), QStringLiteral("Alpha Film"));

        // Movie: details, playback, progress -> Continue watching.
        QSignalSpy details(&vod, &VodService::detailsReady);
        const QString alpha = vod.rowAt(0).value("id").toString();
        vod.requestDetails(QStringLiteral("movie"), alpha);
        QTRY_COMPARE(details.count(), 1);
        QVariantMap d = details.takeFirst().first().toMap();
        QCOMPARE(d.value("url").toString(), QStringLiteral("http://cdn/a.mp4"));
        QVariantMap play = vod.playback(d);
        QCOMPARE(play.value("start").toDouble(), 0.0);
        vod.saveProgress(play, 600, 6000);
        bool hasContinue = false;
        for (const QVariant &c : vod.categories())
            if (c.toMap().value("id") == QLatin1String("continue")) hasContinue = c.toMap().value("count").toInt() == 1;
        QVERIFY(hasContinue);
        vod.setCategory(QStringLiteral("continue"));
        QCOMPARE(vod.items()->count(), 1);
        QCOMPARE(vod.rowAt(0).value("subtitle").toString(), QStringLiteral("90 min left"));
        vod.requestDetails(QStringLiteral("movie"), alpha);
        QTRY_COMPARE(details.count(), 1);
        d = details.takeFirst().first().toMap();
        QCOMPARE(vod.playback(d).value("start").toDouble(), 595.0);     // resumes a little earlier
        QCOMPARE(vod.playback(d, -1, true).value("start").toDouble(), 0.0);
        vod.saveProgress(play, 5500, 6000);                            // >= 90 %: watched
        QCOMPARE(vod.items()->count(), 0);

        // Series: episodes, next episode, "up next" in Continue watching.
        vod.setSection(1);
        vod.setCategory(QStringLiteral("all"));
        QCOMPARE(vod.items()->count(), 1);
        const QString show = vod.rowAt(0).value("id").toString();
        vod.requestDetails(QStringLiteral("series"), show);
        QTRY_COMPARE(details.count(), 1);
        d = details.takeFirst().first().toMap();
        QCOMPARE(d.value("episodes").toList().size(), 2);
        QCOMPARE(d.value("resumeIndex").toInt(), 0);
        const QVariantMap ep1 = vod.playback(d);
        QCOMPARE(ep1.value("subtitle").toString(), QStringLiteral("S1 E1"));
        const QVariantMap ep2 = vod.nextEpisode(ep1);
        QCOMPARE(ep2.value("episode").toInt(), 2);
        QVERIFY(vod.nextEpisode(ep2).isEmpty());
        vod.saveProgress(ep1, 1300, 1400);   // finished: E2 is queued
        vod.setCategory(QStringLiteral("continue"));
        QCOMPARE(vod.items()->count(), 1);
        QCOMPARE(vod.rowAt(0).value("subtitle").toString(), QStringLiteral("Next: S1 E2"));
        vod.requestDetails(QStringLiteral("series"), show);
        QTRY_COMPARE(details.count(), 1);
        QCOMPARE(details.takeFirst().first().toMap().value("resumeIndex").toInt(), 1);
        vod.forgetProgress(QStringLiteral("series:") + show);
        QCOMPARE(vod.items()->count(), 0);

        // Favorites survive a restart of the library.
        QVERIFY(vod.toggleFavorite(QStringLiteral("series"), show));
        vod.setCategory(QStringLiteral("favorites"));
        QCOMPARE(vod.items()->count(), 1);
        QVERIFY(vod.items()->data(vod.items()->index(0), VodModel::FavoriteRole).toBool());
        lib.saveNow();
        UserLibrary again(dir.filePath("library.json"));
        QVERIFY(again.isVodFavorite(QStringLiteral("series:") + show));
        QCOMPARE(again.vodFavorite(QStringLiteral("series:") + show).value("title").toString(), QStringLiteral("Show"));
        QVERIFY(!again.vodProgress(QStringLiteral("movie:") + alpha).isEmpty());
    }

    void catchup_urls()
    {
        Catchup::Account acc;
        acc.server = QStringLiteral("http://tv.example:8080");
        acc.username = QStringLiteral("me");
        acc.password = QStringLiteral("p@ss");
        acc.zone = QTimeZone("Europe/London");
        QCOMPARE(Catchup::streamIdFromUrl(QStringLiteral("http://h:80/live/u/p/1234.ts")), QStringLiteral("1234"));
        QCOMPARE(Catchup::streamIdFromUrl(QStringLiteral("http://h:80/u/p/987")), QStringLiteral("987"));
        QCOMPARE(Catchup::streamIdFromUrl(QStringLiteral("http://h/live/news.m3u8")), QString());

        Channel ch;
        ch.url = QStringLiteral("http://tv.example:8080/live/me/p%40ss/1234.ts");
        ch.catchupType = QStringLiteral("xc");
        ch.catchupDays = 3;
        // 2026-07-01 18:00:30 UTC = 19:00 in London (summer time), 60 min show.
        const qint64 start = QDateTime(QDate(2026, 7, 1), QTime(18, 0, 30), QTimeZone::utc()).toSecsSinceEpoch();
        int adjust = -1;
        QCOMPARE(Catchup::url(ch, start, start + 3570, start + 7200, acc, &adjust),
                 QStringLiteral("http://tv.example:8080/timeshift/me/p%40ss/60/2026-07-01:19-00/1234.ts"));
        QCOMPARE(adjust, 30);
        // In winter London is UTC+0.
        const qint64 winter = QDateTime(QDate(2026, 1, 10), QTime(20, 15), QTimeZone::utc()).toSecsSinceEpoch();
        QVERIFY(Catchup::url(ch, winter, winter + 1800, winter + 3600, acc).contains(QStringLiteral("/30/2026-01-10:20-15/")));
        // No zone name: use the server clock's offset.
        Catchup::Account off = acc;
        off.zone = QTimeZone();
        off.offsetSecs = -5 * 3600;
        QVERIFY(Catchup::url(ch, winter, winter + 1800, winter + 3600, off).contains(QStringLiteral("2026-01-10:15-15")));
        // Without an account, "xc" isn't usable.
        QVERIFY(!Catchup::supported(ch, Catchup::Account{}));

        // M3U templates
        Channel m;
        m.url = QStringLiteral("http://cdn/ch/index.m3u8");
        m.catchupDays = 2;
        m.catchupType = QStringLiteral("default");
        m.catchupSource = QStringLiteral("http://cdn/ch/archive-{utc}-{duration}.m3u8?end=${end}&d={Y}{m}{d}{H}{M}&now={lutc}&x={unknown}");
        QCOMPARE(Catchup::url(m, 1000, 4600, 9000, Catchup::Account{}),
                 QStringLiteral("http://cdn/ch/archive-1000-3600.m3u8?end=4600&d=197001010016&now=9000&x={unknown}"));
        m.catchupType = QStringLiteral("append");
        m.catchupSource = QStringLiteral("?utc={utc}&lutc={lutc}&min={duration:60}");
        QCOMPARE(Catchup::url(m, 1000, 4600, 9000, Catchup::Account{}),
                 QStringLiteral("http://cdn/ch/index.m3u8?utc=1000&lutc=9000&min=60"));
        m.catchupType = QStringLiteral("shift");
        QCOMPARE(Catchup::url(m, 1000, 4600, 9000, Catchup::Account{}),
                 QStringLiteral("http://cdn/ch/index.m3u8?utc=1000&lutc=9000"));
        m.catchupType = QStringLiteral("flussonic");
        QCOMPARE(Catchup::url(m, 1000, 4600, 9000, Catchup::Account{}),
                 QStringLiteral("http://cdn/ch/index-1000-3600.m3u8"));
        m.url = QStringLiteral("http://cdn/ch/mpegts?token=1");
        QCOMPARE(Catchup::url(m, 1000, 4600, 9000, Catchup::Account{}),
                 QStringLiteral("http://cdn/ch/timeshift_abs-1000.ts?token=1"));
        m.catchupType = QStringLiteral("vod");   // not a type we know
        QVERIFY(Catchup::url(m, 1000, 4600, 9000, Catchup::Account{}).isEmpty());
        QCOMPARE(Catchup::expand(QStringLiteral("{utc:Y-m-d H:M:S}"), 0, 0, 0), QStringLiteral("1970-01-01 00:00:00"));
    }

    void catchup_server_info()
    {
        const auto a = XtreamParser::parseAuth(
            "{\"user_info\":{\"auth\":1,\"status\":\"Active\"},"
            "\"server_info\":{\"timezone\":\"America/New_York\",\"time_now\":\"2026-11-09 16:20:11\"}}");
        QVERIFY(a.ok);
        QCOMPARE(a.timezone, QStringLiteral("America/New_York"));
        QCOMPARE(a.timeNow, QStringLiteral("2026-11-09 16:20:11"));
        const auto days = XtreamParser::parseArchiveDays(
            "[{\"stream_id\":1,\"tv_archive\":1,\"tv_archive_duration\":\"5\"},"
            "{\"stream_id\":\"2\",\"tv_archive\":0,\"tv_archive_duration\":3},"
            "{\"stream_id\":3,\"tv_archive\":\"1\",\"tv_archive_duration\":0}]");
        QCOMPARE(days.size(), 1);
        QCOMPARE(days.value(QStringLiteral("1")), 5);
    }

    void catchup_model_and_player()
    {
        AppSettings settings;
        settings.clearSource();
        ChannelModel model;
        // get.php style playlist: no catch-up attributes in the entries.
        model.setPlaylist(M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1 group-title=\"A\",One\nhttp://tv:80/u/p/11\n"
            "#EXTINF:-1 group-title=\"A\",Two\nhttp://tv:80/u/p/22\n"));
        CatchupService catchup(&model);
        PlayerController player(&model, &settings);
        player.setCatchup(&catchup);
        const double now = double(QDateTime::currentMSecsSinceEpoch());
        QVERIFY(!catchup.available(0, now - 3600000));

        QCOMPARE(model.applyArchiveDays({{QStringLiteral("11"), 2}}), 1);
        // Days known, but no account yet: still unavailable.
        QVERIFY(!catchup.available(0, now - 3600000));
        Catchup::Account acc;
        acc.server = QStringLiteral("http://tv:80");
        acc.username = QStringLiteral("u");
        acc.password = QStringLiteral("p");
        acc.zone = QTimeZone::utc();
        catchup.setAccount(acc);
        QCOMPARE(catchup.channelCount(), 1);
        QVERIFY(catchup.available(0, now - 3600000));
        QVERIFY(!catchup.available(0, now - 3 * 86400000.0));   // older than the archive
        QVERIFY(!catchup.available(0, now + 60000));             // not aired yet
        QVERIFY(!catchup.available(1, now - 3600000));           // no archive
        QCOMPARE(model.data(model.index(0), ChannelModel::CatchupDaysRole).toInt(), 2);
        QCOMPARE(model.data(model.index(1), ChannelModel::CatchupDaysRole).toInt(), 0);

        QSignalSpy vod(&player, &PlayerController::vodRequested);
        const double start = now - 2 * 3600000.0 - 15000;   // started 2 h ago (15 s past a minute or so)
        const QVariantMap show{{"title", QStringLiteral("News")}, {"start", start}, {"stop", start + 3600000.0}};
        QVERIFY(player.playCatchup(0, show, 600));
        QCOMPARE(vod.count(), 1);
        QVERIFY(player.vodActive());
        QCOMPARE(player.currentIndex(), 0);
        const QString url = vod.takeFirst().first().toString();
        QVERIFY(url.startsWith(QStringLiteral("http://tv:80/timeshift/u/p/")));
        QVERIFY(url.endsWith(QStringLiteral("/11.ts")));
        QCOMPARE(player.vod().value("programmeStart").toDouble(), start);
        QCOMPARE(player.vod().value("offset").toDouble(), 600.0);
        // Seeking asks again from the new time, within the same show.
        player.seekCatchup(1200);
        QCOMPARE(vod.count(), 1);
        QCOMPARE(player.vod().value("offset").toDouble(), 1200.0);
        QCOMPARE(player.vod().value("programmeStop").toDouble(), start + 3600000.0);
        // No archive on channel 2.
        QVERIFY(!player.playCatchup(1, show, 0));
        player.stopVod();
        QVERIFY(!player.vodActive());
    }

    void hls_playlists()
    {
        const auto master = Hls::parse(
            "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow/index.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1920x1080\nhigh/index.m3u8\n",
            QUrl(QStringLiteral("http://cdn/ch/master.m3u8")));
        QVERIFY(master.valid && master.master);
        QCOMPARE(master.variants.first(), QUrl(QStringLiteral("http://cdn/ch/high/index.m3u8")));
        const auto media = Hls::parse(
            "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:120\n"
            "#EXTINF:4.0,\nseg120.ts\n#EXTINF:4.0,\nhttp://other/seg121.ts\n",
            QUrl(QStringLiteral("http://cdn/ch/high/index.m3u8")));
        QVERIFY(!media.master);
        QCOMPARE(media.segments.size(), 2);
        QCOMPARE(media.segments[0].sequence, qint64(120));
        QCOMPARE(media.segments[0].url, QUrl(QStringLiteral("http://cdn/ch/high/seg120.ts")));
        QCOMPARE(media.segments[1].url, QUrl(QStringLiteral("http://other/seg121.ts")));
        QCOMPARE(media.targetDuration, 4.0);
        QVERIFY(Hls::parse("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n", QUrl()).encrypted);
        QVERIFY(!Hls::parse("#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n", QUrl()).encrypted);
        QVERIFY(!Hls::parse("<html>", QUrl()).valid);
        QVERIFY(Hls::looksLikeHls(QUrl(QStringLiteral("http://h/a/index.m3u8?t=1"))));
        QVERIFY(!Hls::looksLikeHls(QUrl(QStringLiteral("http://h/live/u/p/1.ts"))));
    }

    void recorder_ts_and_hls()
    {
        StreamServer server;
        QNetworkAccessManager net;
        QTemporaryDir dir;

        Recorder ts(&net, QUrl(server.base() + QStringLiteral("/live.ts")), {}, {}, dir.filePath("a.ts"));
        ts.start();
        QTRY_VERIFY_WITH_TIMEOUT(ts.bytes() > 200000, 5000);
        QCOMPARE(ts.state(), Recorder::State::Recording);
        ts.stop();
        QCOMPARE(ts.state(), Recorder::State::Finished);
        QCOMPARE(QFileInfo(dir.filePath("a.ts")).size(), ts.bytes());

        // HLS: starts near live (last 3 segments), then follows new ones.
        Recorder hls(&net, QUrl(server.base() + QStringLiteral("/hls.m3u8")), {}, {}, dir.filePath("b.ts"));
        hls.start();
        QTRY_VERIFY_WITH_TIMEOUT(hls.bytes() >= 5000, 8000);
        hls.stop();
        QCOMPARE(hls.state(), Recorder::State::Finished);
        QCOMPARE(QFileInfo(dir.filePath("b.ts")).size() % 1000, 0);

        // Nothing there: retries, then fails on stop with nothing saved.
        Recorder bad(&net, QUrl(server.base() + QStringLiteral("/missing.ts")), {}, {}, dir.filePath("c.ts"));
        bad.start();
        QTRY_COMPARE_WITH_TIMEOUT(bad.state(), Recorder::State::Retrying, 3000);
        bad.stop();
        QCOMPARE(bad.state(), Recorder::State::Failed);
    }

    void schedule_reminders_and_recordings()
    {
        StreamServer server;
        QTemporaryDir dir;
        ChannelModel model;
        model.setPlaylist(M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1 group-title=\"A\",One\n" + (server.base() + QStringLiteral("/live.ts")).toUtf8() + "\n"
            "#EXTINF:-1 group-title=\"A\",Two\nhttp://127.0.0.1:1/none.ts\n"));
        const double now = double(QDateTime::currentMSecsSinceEpoch());
        {
            ScheduleService schedule(&model, nullptr, dir.path(), dir.filePath("Recordings"));
            QSignalSpy due(&schedule, &ScheduleService::reminderDue);

            // Reminder for a show starting in 30 s: due now (fires a minute early).
            const QVariantMap soon{{"title", QStringLiteral("Soon Show")}, {"start", now + 30000}, {"stop", now + 1830000}};
            QVERIFY(!schedule.addReminder(0, soon).isEmpty());
            QCOMPARE(due.count(), 1);
            QCOMPARE(due.first().first().toMap().value("title").toString(), QStringLiteral("Soon Show"));
            QVERIFY(schedule.items().isEmpty());
            // Reminder for later stays, and is shown in the guide.
            const QVariantMap later{{"title", QStringLiteral("Later")}, {"start", now + 7200000}, {"stop", now + 9000000}};
            const QString rid = schedule.addReminder(1, later);
            QCOMPARE(schedule.markFor(1, later.value("start").toDouble()), QStringLiteral("reminder"));
            QCOMPARE(schedule.addReminder(1, later), rid);   // no duplicates
            // No reminders for shows that have started.
            QVERIFY(schedule.addReminder(0, {{"title", "x"}, {"start", now - 1000}, {"stop", now + 1000}}).isEmpty());

            // Recording a show that's on: starts straight away.
            const QVariantMap on{{"title", QStringLiteral("News: Live?")}, {"start", now - 600000}, {"stop", now + 600000}};
            const QString id = schedule.addRecording(0, on);
            QVERIFY(!id.isEmpty());
            QCOMPARE(schedule.recordingCount(), 1);
            QCOMPARE(schedule.markFor(0, on.value("start").toDouble()), QStringLiteral("recording"));
            QTRY_VERIFY_WITH_TIMEOUT(schedule.item(id).value("bytes").toDouble() > 100000, 5000);
            const QString file = schedule.item(id).value("file").toString();
            QVERIFY(file.contains(QStringLiteral("News_ Live_ - One - ")));
            schedule.stopRecording(id);
            QCOMPARE(schedule.recordingCount(), 0);
            QCOMPARE(schedule.item(id).value("status").toString(), QStringLiteral("done"));
            QVERIFY(QFileInfo(file).size() > 100000);

            // A show already over can't be recorded; a future one waits.
            QVERIFY(schedule.addRecording(0, {{"title", "old"}, {"start", now - 7200000}, {"stop", now - 3600000}}).isEmpty());
            const QString fut = schedule.addRecording(1, later);
            QCOMPARE(schedule.item(fut).value("status").toString(), QStringLiteral("scheduled"));
            QCOMPARE(schedule.recordingCount(), 0);
        }
        // Everything survives a restart; deleting removes the file.
        ScheduleService again(&model, nullptr, dir.path(), dir.filePath("Recordings"));
        QCOMPARE(again.items().size(), 3);   // reminder, finished recording, scheduled recording
        QString doneId, doneFile;
        for (const QVariant &v : again.items()) {
            const QVariantMap m = v.toMap();
            if (m.value("status") == QLatin1String("done")) { doneId = m.value("id").toString(); doneFile = m.value("file").toString(); }
        }
        QVERIFY(QFileInfo::exists(doneFile));
        again.remove(doneId, true);
        QVERIFY(!QFileInfo::exists(doneFile));
        QCOMPARE(again.items().size(), 2);
    }

    void parental_pin_and_locks()
    {
        QSettings().remove("parental");
        QTemporaryDir dir;
        AppSettings settings;
        settings.clearSource();
        UserLibrary lib(dir.filePath("library.json"));
        ChannelModel model;
        model.setLibrary(&lib);
        model.setPlaylist(M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1 tvg-chno=\"1\" group-title=\"News\",News One\nhttp://s/1\n"
            "#EXTINF:-1 tvg-chno=\"2\" group-title=\"Adult\",Late One\nhttp://s/2\n"
            "#EXTINF:-1 tvg-chno=\"3\" group-title=\"Adult\",Late Two\nhttp://s/3\n"
            "#EXTINF:-1 tvg-chno=\"4\" group-title=\"News\",News Two\nhttp://s/4\n"));
        Parental parental(&model, &lib);
        PlayerController player(&model, &settings);
        player.setParental(&parental);

        QVERIFY(!parental.hasPin());
        QVERIFY(parental.unlocked());
        QVERIFY(!Parental::validPin(QStringLiteral("12a4")));
        QVERIFY(!parental.setPin({}, QStringLiteral("123")));
        QVERIFY(parental.setPin({}, QStringLiteral("2468")));
        QVERIFY(parental.hasPin());
        QVERIFY(parental.unlocked());   // whoever set it can carry on

        // Lock the "Adult" group (index 3: Favorites, All, News, Adult).
        const int adult = model.groupIndex(QStringLiteral("Adult"));
        QVERIFY(adult > ChannelModel::AllGroup);
        QVERIFY(parental.toggleGroupLock(adult));
        QVERIFY(parental.isChannelLocked(1));
        QVERIFY(!parental.blocked(1));        // still unlocked this session
        parental.lock();
        QVERIFY(!parental.unlocked());
        QVERIFY(parental.blocked(1));
        QVERIFY(parental.blocked(2));
        QVERIFY(!parental.blocked(0));
        QVERIFY(model.groups()[adult].toMap().value("locked").toBool());

        // Zapping skips locked channels; search leaves them out.
        QCOMPARE(model.step(0, 1, ChannelModel::AllGroup), 3);
        QCOMPARE(model.step(3, -1, ChannelModel::AllGroup), 0);
        QCOMPARE(model.searchChannels(QStringLiteral("late")).size(), 0);
        QCOMPARE(model.searchChannels(QStringLiteral("one")).size(), 1);

        // Tuning a locked channel asks for the PIN, then carries on.
        QSignalSpy pin(&player, &PlayerController::pinRequired);
        QSignalSpy play(&player, &PlayerController::playRequested);
        player.tune(1);
        QCOMPARE(pin.count(), 1);
        QCOMPARE(play.count(), 0);
        QVERIFY(!parental.enterPin(QStringLiteral("0000")));
        QVERIFY(parental.enterPin(QStringLiteral("2468")));
        player.resumePending();
        QCOMPARE(play.count(), 1);
        QCOMPARE(player.currentIndex(), 1);
        QCOMPARE(model.searchChannels(QStringLiteral("late")).size(), 2);

        // Five wrong tries: a pause before the next one counts.
        parental.lock();
        for (int i = 0; i < 5; ++i) QVERIFY(!parental.enterPin(QStringLiteral("1111")));
        QVERIFY(parental.retryWait() > 0);
        QVERIFY(!parental.enterPin(QStringLiteral("2468")));   // even the right PIN waits

        // Locks are remembered; a single channel can be locked too.
        Parental again(&model, &lib);
        QVERIFY(again.isChannelLocked(2));
        QVERIFY(again.blocked(2));
        QVERIFY(!again.isChannelLocked(3));
        lib.setChannelLocked(model.keyAt(3), true);
        QVERIFY(again.blocked(3));
        // Movies: category locks.
        QVERIFY(again.toggleCategoryLock(0, QStringLiteral("44")));
        QVERIFY(again.categoryBlocked(0, QStringLiteral("44")));
        QVERIFY(!again.categoryBlocked(1, QStringLiteral("44")));

        QSettings().remove("parental");
        model.setBlockedCheck({});
    }

    void secrets_stay_out_of_settings_where_possible()
    {
        AppSettings settings;
        settings.saveM3u(QStringLiteral("http://h/get.php?username=u&password=secret"));
        QCOMPARE(settings.m3uLocation(), QStringLiteral("http://h/get.php?username=u&password=secret"));
        AppSettings reread;   // a new instance reads it back
        QCOMPARE(reread.m3uLocation(), QStringLiteral("http://h/get.php?username=u&password=secret"));
        settings.saveXtream(QStringLiteral("h"), QStringLiteral("u"), QStringLiteral("pw"), QStringLiteral("ts"));
        QCOMPARE(settings.xtreamPassword(), QStringLiteral("pw"));
        settings.clearSource();
        QVERIFY(settings.m3uLocation().isEmpty());
        QVERIFY(settings.xtreamPassword().isEmpty());
    }

    void playlists_list_and_migration()
    {
        QSettings raw;
        raw.remove("playlists");
        // Settings from before multiple playlists move into the list.
        raw.setValue("source/type", "m3u");
        raw.setValue("source/m3u", "http://www.provider.example/get.php?username=u&password=p");
        raw.setValue("guide/url", "http://guide.example/epg.xml");
        raw.setValue("player/lastChannelUrl", "s:42");
        raw.sync();
        {
            AppSettings s;
            QCOMPARE(s.playlistIds(), QStringList{QStringLiteral("p1")});
            QCOMPARE(s.activeId(), QStringLiteral("p1"));
            QCOMPARE(s.activeName(), QStringLiteral("provider.example"));
            QCOMPARE(s.m3uLocation(), QStringLiteral("http://www.provider.example/get.php?username=u&password=p"));
            QCOMPARE(s.epgOverride(), QStringLiteral("http://guide.example/epg.xml"));
            QCOMPARE(s.lastChannelUrl(), QStringLiteral("s:42"));
            QVERIFY(!QSettings().contains("source/type"));

            // A second playlist, its own settings.
            const QString id = s.createPlaylist(QStringLiteral("Backup"));
            QCOMPARE(id, QStringLiteral("p2"));
            s.setActive(id);
            QVERIFY(!s.hasSource());
            s.saveXtream(QStringLiteral("http://x.example:8080"), QStringLiteral("me"), QStringLiteral("pw"), QStringLiteral("ts"));
            QCOMPARE(s.sourceType(), QStringLiteral("xtream"));
            QCOMPARE(s.xtreamPassword(), QStringLiteral("pw"));
            QVERIFY(s.epgOverride().isEmpty());
            s.setLastChannelUrl(QStringLiteral("s:7"));
            s.setActive(QStringLiteral("p1"));
            QCOMPARE(s.sourceType(), QStringLiteral("m3u"));
            QCOMPARE(s.lastChannelUrl(), QStringLiteral("s:42"));
            const QVariantList list = s.playlists();
            QCOMPARE(list.size(), 2);
            QVERIFY(list[0].toMap().value("active").toBool());
            QCOMPARE(list[1].toMap().value("name").toString(), QStringLiteral("Backup"));
            s.renamePlaylist(QStringLiteral("p2"), QStringLiteral("Spare"));
            s.movePlaylist(QStringLiteral("p2"), -1);
            QCOMPARE(s.playlistIds(), (QStringList{QStringLiteral("p2"), QStringLiteral("p1")}));
            QCOMPARE(s.playlistName(QStringLiteral("p2")), QStringLiteral("Spare"));
            // Removing the active one leaves none active.
            s.removePlaylist(QStringLiteral("p1"));
            QVERIFY(s.activeId().isEmpty());
            QCOMPARE(s.playlistIds(), QStringList{QStringLiteral("p2")});
        }
        AppSettings again;   // opens on the remaining playlist
        QCOMPARE(again.activeId(), QStringLiteral("p2"));
        QCOMPARE(again.xtreamUser(), QStringLiteral("me"));
        QSettings().remove("playlists");
    }

    void schedule_belongs_to_playlist()
    {
        QTemporaryDir dir;
        ChannelModel model;
        model.setPlaylist(M3uParser::parse("#EXTM3U\n#EXTINF:-1,One\nhttp://127.0.0.1:1/a.ts\n"));
        ScheduleService schedule(&model, nullptr, dir.path(), dir.filePath("R"), QStringLiteral("p1"));
        QSignalSpy due(&schedule, &ScheduleService::reminderDue);
        const double now = double(QDateTime::currentMSecsSinceEpoch());
        const QVariantMap later{{"title", QStringLiteral("Later")}, {"start", now + 7200000}, {"stop", now + 9000000}};
        schedule.addReminder(0, later);
        QCOMPARE(schedule.markFor(0, later.value("start").toDouble()), QStringLiteral("reminder"));
        schedule.setPlaylist(QStringLiteral("p2"));   // same key, other playlist: not marked
        QVERIFY(schedule.markFor(0, later.value("start").toDouble()).isEmpty());
        QCOMPARE(schedule.items().first().toMap().value("globalIndex").toInt(), -1);
        // A recording due while another playlist is open is missed.
        schedule.setPlaylist(QStringLiteral("p1"));
        const QVariantMap soon{{"title", QStringLiteral("Soon")}, {"start", now + 30000}, {"stop", now + 600000}};
        const QString id = schedule.addRecording(0, soon);
        QCOMPARE(schedule.item(id).value("status").toString(), QStringLiteral("recording"));
        schedule.stopRecording(id);
        const QVariantMap next{{"title", QStringLiteral("Next")}, {"start", now + 3600000}, {"stop", now + 5400000}};
        const QString id2 = schedule.addRecording(0, next);
        schedule.setPlaylist(QStringLiteral("p2"));
        schedule.tick();
        QCOMPARE(schedule.item(id2).value("status").toString(), QStringLiteral("scheduled"));   // not due yet
    }

    void other_playlists_for_multiview()
    {
        QSettings().remove("playlists");
        QTemporaryDir dir;
        QFile m3u(dir.filePath("second.m3u"));
        QVERIFY(m3u.open(QIODevice::WriteOnly));
        m3u.write("#EXTM3U\n"
                  "#EXTINF:-1 tvg-chno=\"1\" group-title=\"News\",Second News\nhttp://two/1.ts\n"
                  "#EXTINF:-1 tvg-chno=\"2\" group-title=\"Adult\",Late\nhttp://two/2.ts\n"
                  "#EXTINF:-1 tvg-chno=\"3\" group-title=\"News\",Hidden One\nhttp://two/3.ts\n"
                  "#EXTINF:-1 tvg-chno=\"4\" group-title=\"Sport\",Second Sport\nhttp://two/4.ts\n");
        m3u.close();

        AppSettings settings;
        settings.saveM3u(QStringLiteral("http://one/list.m3u"));          // p1, being watched
        const QString p2 = settings.createPlaylist(QStringLiteral("Second"));
        const QString p1 = settings.activeId();
        settings.setActive(p2);
        settings.saveM3u(dir.filePath("second.m3u"));
        settings.setActive(p1);
        // The second playlist's own choices: one hidden channel, a locked group, a renamed channel.
        QDir().mkpath(Session::storageDir(dir.path(), p2));
        QFile lib(Session::storageDir(dir.path(), p2) + "/library.json");
        QVERIFY(lib.open(QIODevice::WriteOnly));
        {
            Channel c3, c4;
            c3.url = QStringLiteral("http://two/3.ts");
            c4.url = QStringLiteral("http://two/4.ts");
            QJsonObject o;
            o.insert("hiddenChannels", QJsonObject{{ChannelModel::channelKey(c3), "Hidden One"}});
            o.insert("lockedGroups", QJsonArray{"Adult"});
            o.insert("channelNames", QJsonObject{{ChannelModel::channelKey(c4), "Sport HD"}});
            lib.write(QJsonDocument(o).toJson());
        }
        lib.close();

        auto keyOf = [](const char *url) { Channel c; c.url = QString::fromLatin1(url); return ChannelModel::channelKey(c); };
        OtherPlaylists others(&settings, dir.path());
        others.ensure(p1);                         // the one being watched: nothing to do
        QVERIFY(others.state(p1).isEmpty());
        others.ensure(p2);
        QTRY_COMPARE(others.state(p2), QStringLiteral("ready"));
        const QVariantList all = others.channels(p2, QString());
        QCOMPARE(all.size(), 3);                   // hidden one left out
        QCOMPARE(all[2].toMap().value("name").toString(), QStringLiteral("Sport HD"));
        QCOMPARE(others.channels(p2, QStringLiteral("sport")).size(), 1);
        QCOMPARE(others.channels(p2, QStringLiteral("2")).first().toMap().value("name").toString(), QStringLiteral("Late"));
        const QVariantMap late = others.info(p2, keyOf("http://two/2.ts"));
        QCOMPARE(late.value("url").toString(), QStringLiteral("http://two/2.ts"));
        QVERIFY(late.value("locked").toBool());
        QVERIFY(others.isLocked(p2, keyOf("http://two/2.ts")));
        // Zapping skips shown and (when asked) locked channels.
        QCOMPARE(others.step(p2, keyOf("http://two/1.ts"), 1, {}, true), keyOf("http://two/4.ts"));
        QCOMPARE(others.step(p2, keyOf("http://two/1.ts"), 1, {}, false), keyOf("http://two/2.ts"));
        QCOMPARE(others.step(p2, keyOf("http://two/4.ts"), 1, {keyOf("http://two/1.ts")}, true), QString());
        // Removing the playlist forgets its channels.
        settings.removePlaylist(p2);
        QVERIFY(others.state(p2).isEmpty());
        QSettings().remove("playlists");
    }

    void backup_round_trip()
    {
        QTemporaryDir dir;
        const QString data = dir.path() + QStringLiteral("/data");
        QDir().mkpath(data);
        QString file;
        {
            QSettings().clear();
            AppSettings settings;
            settings.saveM3u(QStringLiteral("http://provider.example/get.php?username=a&password=b"));
            settings.setPictureShape(QStringLiteral("4:3"));
            settings.setUserAgent(QStringLiteral("MyBox/1.0"));
            const QString id = settings.activeId();
            QDir().mkpath(data + QStringLiteral("/playlists/") + id);
            UserLibrary library(data + QStringLiteral("/playlists/") + id + QStringLiteral("/library.json"));
            library.setFavorite(QStringLiteral("s:7"), true);
            library.addRecent(QStringLiteral("s:9"));
            QFile sched(data + QStringLiteral("/schedule.json"));
            QVERIFY(sched.open(QIODevice::WriteOnly));
            sched.write("{\"items\":[]}");
            sched.close();

            Backup backup(&settings, &library, data);
            file = dir.path() + QStringLiteral("/b");
            QCOMPARE(backup.saveTo(QUrl::fromLocalFile(file).toString()), QString());
            file += QStringLiteral(".relaybackup");
            QVERIFY(QFile::exists(file));
            const QVariantMap d = backup.describe(file);
            QVERIFY(d.value("ok").toBool());
            QCOMPARE(d.value("playlists").toStringList().size(), 1);
            // Not a backup: refused.
            QFile junk(dir.path() + QStringLiteral("/junk.json"));
            QVERIFY(junk.open(QIODevice::WriteOnly));
            junk.write("{\"settings\":{}}");
            junk.close();
            QVERIFY(!backup.restoreFrom(junk.fileName()).isEmpty());
            QCOMPARE(backup.restoreFrom(file), QString());
        }
        // Everything changes before the restart…
        QSettings().clear();
        QFile::remove(data + QStringLiteral("/schedule.json"));
        QDir(data + QStringLiteral("/playlists")).removeRecursively();
        // …and comes back when it's applied.
        QVERIFY(Backup::applyPending(data));
        QVERIFY(!Backup::applyPending(data));          // only once
        AppSettings settings;
        QCOMPARE(settings.playlistIds().size(), 1);
        QCOMPARE(settings.m3uLocation(), QStringLiteral("http://provider.example/get.php?username=a&password=b"));
        QCOMPARE(settings.pictureShape(), QStringLiteral("4:3"));
        QCOMPARE(settings.userAgent(), QStringLiteral("MyBox/1.0"));
        QCOMPARE(AppSettings::effectiveUserAgent(), QStringLiteral("MyBox/1.0"));
        UserLibrary library(data + QStringLiteral("/playlists/") + settings.activeId() + QStringLiteral("/library.json"));
        QVERIFY(library.isFavorite(QStringLiteral("s:7")));
        QCOMPARE(library.recent().value(0), QStringLiteral("s:9"));
        QVERIFY(QFile::exists(data + QStringLiteral("/schedule.json")));
        settings.clearSource();
        QSettings().clear();
    }

    void favorites_lists()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("library.json"));
        QString sportsId;
        {
            UserLibrary library(path);
            ChannelModel model;
            model.setLibrary(&library);
            model.setPlaylist(M3uParser::parse(sample()));
            const int firstPlaylistGroup = ChannelModel::AllGroup + 1;
            const QString firstName = model.groupName(firstPlaylistGroup);

            const int sports = model.createList(QStringLiteral("Sports"));
            QCOMPARE(sports, ChannelModel::AllGroup + 1);          // right after All channels
            QVERIFY(model.isListGroup(sports));
            QCOMPARE(model.groupLabel(sports), QStringLiteral("Sports"));
            QCOMPARE(model.groupName(sports + 1), firstName);      // playlist groups follow
            QCOMPARE(model.createList(QStringLiteral("  ")), -1);
            sportsId = model.groupName(sports);
            const int kids = model.createList(QStringLiteral("Kids"));
            QCOMPARE(kids, sports + 1);

            model.setInList(3, sportsId, true);
            model.setInList(0, sportsId, true);
            model.setInList(3, sportsId, true);                    // twice: still once
            QCOMPARE(model.groupCounts().at(sports).toInt(), 2);
            QCOMPARE(model.listsFor(3).at(0).toMap().value("member").toBool(), true);
            QCOMPARE(model.listsFor(3).at(1).toMap().value("member").toBool(), false);
            QCOMPARE(model.listsFor(1).at(0).toMap().value("member").toBool(), false);
            QVERIFY(!model.isFavorite(3));                         // separate from Favorites

            // Order inside the list, and zapping stays inside it.
            model.setCurrentGroup(sports);
            QCOMPARE(model.globalIndexAt(0), 3);
            QCOMPARE(model.moveChannel(0, 1), 1);
            QCOMPARE(model.globalIndexAt(0), 0);
            QCOMPARE(model.step(0, 1, sports), 3);
            QCOMPARE(model.step(3, 1, sports), 0);

            // Lists move among themselves only; hiding and locking don't apply.
            QCOMPARE(model.moveGroup(kids, -1), sports);
            QCOMPARE(model.groupLabel(sports), QStringLiteral("Kids"));
            QCOMPARE(model.moveGroup(sports + 1, 1), sports + 1);  // can't go below a playlist group
            model.hideGroup(sportsId);
            QVERIFY(model.groupIndex(sportsId) > 0);
            model.renameGroup(model.groupIndex(sportsId), QStringLiteral("Football"));
            QCOMPARE(model.groupLabel(model.groupIndex(sportsId)), QStringLiteral("Football"));
            model.renameGroup(model.groupIndex(sportsId), QString());   // a list needs a name
            QCOMPARE(model.groupLabel(model.groupIndex(sportsId)), QStringLiteral("Football"));

            // A hidden channel leaves the list (and comes back when unhidden).
            model.hideChannel(3);
            QCOMPARE(model.groupCounts().at(model.groupIndex(sportsId)).toInt(), 1);
            model.unhideChannel(model.keyAt(3));
            QCOMPARE(model.groupCounts().at(model.groupIndex(sportsId)).toInt(), 2);
        }
        {
            UserLibrary library(path);
            ChannelModel model;
            model.setLibrary(&library);
            model.setPlaylist(M3uParser::parse(sample()));
            const int g = model.groupIndex(sportsId);
            QCOMPARE(g, ChannelModel::AllGroup + 2);               // Kids first, as moved
            QCOMPARE(model.groupLabel(g), QStringLiteral("Football"));
            model.setCurrentGroup(g);
            QCOMPARE(model.count(), 2);
            QCOMPARE(model.globalIndexAt(0), 0);
            model.setInList(0, sportsId, false);
            QCOMPARE(model.count(), 1);
            model.deleteList(g);
            QCOMPARE(model.groupIndex(sportsId), -1);
            QCOMPARE(model.currentGroup(), int(ChannelModel::AllGroup));
            model.deleteList(ChannelModel::AllGroup + 2);          // a playlist group: nothing happens
            QVERIFY(model.groups().size() > 4);
        }
    }

    void portal_parsing()
    {
        QCOMPARE(PortalClient::normalizeMac(QStringLiteral("00:1a:79:ab:cd:ef")), QStringLiteral("00:1A:79:AB:CD:EF"));
        QCOMPARE(PortalClient::normalizeMac(QStringLiteral(" 001A79ABCDEF ")), QStringLiteral("00:1A:79:AB:CD:EF"));
        QCOMPARE(PortalClient::normalizeMac(QStringLiteral("00-1A-79-AB-CD-EF")), QStringLiteral("00:1A:79:AB:CD:EF"));
        QCOMPARE(PortalClient::normalizeMac(QStringLiteral("00:1A:79:AB:CD")), QString());
        QCOMPARE(PortalClient::normalizeMac(QStringLiteral("user@example")), QString());

        auto first = [](const QString &server) { return PortalClient::endpoints(server).value(0).toString(); };
        QCOMPARE(first(QStringLiteral("http://tv.example:8080/c/")), QStringLiteral("http://tv.example:8080/portal.php"));
        QCOMPARE(first(QStringLiteral("tv.example:8080")), QStringLiteral("http://tv.example:8080/portal.php"));
        QCOMPARE(first(QStringLiteral("http://tv.example/stalker_portal/c/index.html")),
                 QStringLiteral("http://tv.example/stalker_portal/server/load.php"));
        QCOMPARE(first(QStringLiteral("https://tv.example/stalker_portal")),
                 QStringLiteral("https://tv.example/stalker_portal/server/load.php"));
        QCOMPARE(first(QStringLiteral("http://tv.example/custom/load.php?x=1")), QStringLiteral("http://tv.example/custom/load.php"));
        QCOMPARE(PortalClient::endpoints(QStringLiteral("http://tv.example/c")).size(), 3);
        QVERIFY(PortalClient::endpoints(QStringLiteral("   ")).isEmpty());

        const QByteArray shake = js("{'js':{'token':'ABC123','random':'x'}}");
        QCOMPARE(PortalClient::parseToken(shake), QStringLiteral("ABC123"));
        QCOMPARE(PortalClient::parseToken("<html>not a portal</html>"), QString());

        QStringList order;
        const auto genres = PortalClient::parseGenres(
            js("{'js':[{'id':'*','title':'All'},{'id':'1','title':'News'},{'id':2,'title':'Sports'}]}"), &order);
        QCOMPARE(genres.size(), 2);
        QCOMPARE(order, QStringList({QStringLiteral("News"), QStringLiteral("Sports")}));

        const QString account = PortalClient::accountId(QStringLiteral("http://tv.example/c/"), QStringLiteral("00:1A:79:AB:CD:EF"));
        const PlaylistResult r = PortalClient::parseChannels(
            js("{'js':{'total_items':3,'data':["
"{'id':'101','name':'News One','number':'5','tv_genre_id':'1','logo':'http://l/1.png','cmd':'ffmpeg http://localhost/ch/101_'},"
"{'id':102,'name':'Match TV','number':'','tv_genre_id':2,'cmds':[{'url':'ffmpeg http://localhost/ch/102_'}]},"
"{'id':'103','name':'','cmd':'x'},"
"{'id':'104','name':'Unsorted','tv_genre_id':'9','cmd':'auto http://direct/104.ts'}]}}"),
            genres, order, account);
        QVERIFY(r.error.isEmpty());
        QCOMPARE(r.channels.size(), 3);
        QCOMPARE(r.channels[0].number, 5);
        QCOMPARE(r.channels[0].group, QStringLiteral("News"));
        QCOMPARE(r.channels[0].tvgId, QStringLiteral("101"));
        QCOMPARE(r.channels[1].group, QStringLiteral("Sports"));
        QCOMPARE(r.channels[1].number, 2);
        QCOMPARE(r.channels[2].group, QStringLiteral("Other"));
        QVERIFY(PortalClient::isPortalUrl(r.channels[0].url));
        QVERIFY(!r.channels[0].url.contains(QLatin1Char(' ')));
        QCOMPARE(QUrl(r.channels[0].url).host(), account);
        // The key (favorites, last channel) is the portal's channel id.
        QCOMPARE(ChannelModel::channelKey(r.channels[0]), QStringLiteral("s:101"));
        QCOMPARE(ChannelModel::channelKey(r.channels[1]), QStringLiteral("s:102"));
        const QByteArray none = js("{'js':{'data':[]}}");
        QVERIFY(!PortalClient::parseChannels(none, genres, order, account).error.isEmpty());

        const QByteArray link1 = js("{'js':{'id':'101','cmd':'ffmpeg http://cdn.example/play/live.php?mac=00:1A&stream=101&token=xyz'}}");
        const QByteArray link2 = js("{'js':{'cmd':'http://cdn.example/101.m3u8'}}");
        const QByteArray link3 = js("{'js':[]}");
        QCOMPARE(PortalClient::parseLink(link1), QStringLiteral("http://cdn.example/play/live.php?mac=00:1A&stream=101&token=xyz"));
        QCOMPARE(PortalClient::parseLink(link2), QStringLiteral("http://cdn.example/101.m3u8"));
        QCOMPARE(PortalClient::parseLink(link3), QString());

        int count = 0;
        const QByteArray xml = PortalClient::xmltvFromEpg(
            js("{'js':{'data':{'101':[{'name':'Morning <News> & more','descr':'Headlines.','start_timestamp':'1700000000','stop_timestamp':1700003600},"
"{'name':'','start_timestamp':'1700003600','stop_timestamp':'1700007200'}],"
"'102':[{'name':'The Match','start_timestamp':1700000000,'stop_timestamp':1700007200}]}}}"), &count);
        QCOMPARE(count, 2);
        QVERIFY(xml.contains("<channel id=\"101\">"));
        QVERIFY(xml.contains("start=\"20231114221320 +0000\""));
        QVERIFY(xml.contains("Morning &lt;News> &amp; more") || xml.contains("Morning &lt;News&gt; &amp; more"));
        const QByteArray empty = js("{'js':{'data':{}}}");
        QVERIFY(PortalClient::xmltvFromEpg(empty).isEmpty());
    }

    void portal_movies_and_series()
    {
        const QString account = QStringLiteral("acc1");
        const QUrl base(QStringLiteral("http://tv.example:8080"));
        const VodParser::PortalPage page = VodParser::parsePortalPage(js(
            "{'js':{'total_items':30,'max_page_items':14,'data':["
            "{'id':'501','name':'Big Film','year':'2019-05-01','rating_imdb':'7.4','description':'Plot.','director':'D','actors':'A, B',"
            "'genres_str':'Action','time':'125','screenshot_uri':'/stalker_portal/screenshots/1.jpg','cmd':'/media/file_501.mpg',"
            "'added':'2026-01-02 10:00:00','is_series':'0','series':[]},"
            "{'id':'800','name':'Old Show','is_series':'1','series':[1,2,'3'],'cmd':'/media/800.mpg'},"
            "{'id':'802','name':'Quiet Show','is_series':0,'series':[4]},"
            "{'id':'','name':'No id'}]}}"), QStringLiteral("vod"), QStringLiteral("v10"), account, base);
        QCOMPARE(page.pages, 3);
        QCOMPARE(page.movies.size(), 1);
        QCOMPARE(page.series.size(), 2);   // flagged, or with episode numbers
        const VodItem &m = page.movies[0];
        QCOMPARE(m.id, QStringLiteral("501"));
        QCOMPARE(m.year, QStringLiteral("2019"));
        QCOMPARE(m.rating, 7.4);
        QCOMPARE(m.categoryId, QStringLiteral("v10"));
        QCOMPARE(m.poster, QStringLiteral("http://tv.example:8080/stalker_portal/screenshots/1.jpg"));
        QVERIFY(m.added > 0);
        QVERIFY(PortalClient::isPortalUrl(m.url));
        QCOMPARE(QUrl(m.url).path(), QStringLiteral("/vod"));
        const QVariantMap info = page.info.value(QStringLiteral("movie:501"));
        QCOMPARE(info.value("duration").toString(), QStringLiteral("2 h 5 min"));
        QCOMPARE(info.value("cast").toString(), QStringLiteral("A, B"));
        QCOMPARE(page.info.value(QStringLiteral("series:800")).value("numbers").toList().size(), 3);
        QVERIFY(page.series[0].url.isEmpty());

        // The series section's ids get an "s" so they can't clash with the movie section's.
        const VodParser::PortalPage shows = VodParser::parsePortalPage(js("{'js':{'total_items':1,'max_page_items':14,"
            "'data':[{'id':'900','name':'Harbor Lights'}]}}"), QStringLiteral("series"), QStringLiteral("s20"), account, base);
        QCOMPARE(shows.series.size(), 1);
        QCOMPARE(shows.series[0].id, QStringLiteral("s900"));
        QCOMPARE(shows.info.value(QStringLiteral("series:s900")).value("movieId").toString(), QStringLiteral("900"));

        // Seasons carrying their episode numbers, and seasons listing episodes.
        int pages = 0;
        const QVariantList seasons = VodParser::parsePortalSeasonList(js("{'js':{'total_items':2,'max_page_items':14,'data':["
            "{'id':'900:2','name':'Season 2','series':[1,2],'cmd':'/media/900_2.mpg'},"
            "{'id':'900:1','name':'Сезон 1','series':[],'cmd':''}]}}"), &pages);
        QCOMPARE(pages, 1);
        QCOMPARE(seasons.size(), 2);
        QCOMPARE(seasons[0].toMap().value("number").toInt(), 2);
        QCOMPARE(seasons[1].toMap().value("number").toInt(), 1);
        const QVariantList fromNumbers = VodParser::portalEpisodes(account, QStringLiteral("s900"), 2,
            seasons[0].toMap().value("cmd").toString(), seasons[0].toMap().value("numbers").toList());
        QCOMPARE(fromNumbers.size(), 2);
        const QVariantMap e1 = fromNumbers[0].toMap();
        QCOMPARE(e1.value("id").toString(), QStringLiteral("s900:2:1"));
        QCOMPARE(e1.value("season").toInt(), 2);
        QCOMPARE(QUrlQuery(QUrl(e1.value("url").toString())).queryItemValue("series"), QStringLiteral("1"));

        const QVariantList listed = VodParser::parsePortalSeasonList(js("{'js':{'total_items':2,'max_page_items':14,'data':["
            "{'id':'e2','name':'Episode 2','series_number':2,'cmd':'/media/ep2.mpg'},"
            "{'id':'e1','name':'Pilot','series_number':1,'cmd':'/media/ep1.mpg','description':'It begins.'}]}}"));
        const QVariantList eps = VodParser::portalEpisodes(account, QStringLiteral("s901"), 1, QString(), listed);
        QCOMPARE(eps.size(), 2);
        QCOMPARE(eps[0].toMap().value("title").toString(), QStringLiteral("Pilot"));       // in episode order
        QCOMPARE(eps[0].toMap().value("plot").toString(), QStringLiteral("It begins."));
        QCOMPARE(eps[1].toMap().value("title").toString(), QString());                      // "Episode 2" says nothing
        const QUrl ep1(eps[0].toMap().value("url").toString());
        QCOMPARE(QUrl::fromPercentEncoding(QUrlQuery(ep1).queryItemValue("cmd", QUrl::FullyEncoded).toLatin1()), QStringLiteral("/media/ep1.mpg"));
        QVERIFY(!QUrlQuery(ep1).hasQueryItem("series"));
    }

    void portal_catchup()
    {
        // Channels the portal archives get catch-up.
        QStringList order;
        const QString account = QStringLiteral("acc1");
        const PlaylistResult r = PortalClient::parseChannels(js("{'js':{'data':["
            "{'id':'101','name':'News','tv_archive':'1','tv_archive_duration':'168','cmd':'ffmpeg http://localhost/ch/101_'},"
            "{'id':'102','name':'Sport','tv_archive':1,'cmd':'ffmpeg http://localhost/ch/102_'},"
            "{'id':'103','name':'Kids','tv_archive':'0','cmd':'ffmpeg http://localhost/ch/103_'}]}}"), {}, order, account);
        QCOMPARE(r.channels.size(), 3);
        QCOMPARE(r.channels[0].catchupType, QStringLiteral("portal"));
        QCOMPARE(r.channels[0].catchupDays, 7);
        QCOMPARE(r.channels[1].catchupDays, 3);   // length not given: three days
        QCOMPARE(r.channels[2].catchupDays, 0);
        QVERIFY(Catchup::supported(r.channels[0], {}));
        QVERIFY(!Catchup::supported(r.channels[2], {}));
        const QString url = Catchup::url(r.channels[0], 1700000000, 1700003600, 1700100000, {});
        QVERIFY(PortalClient::isPortalUrl(url));
        QCOMPARE(QUrl(url).path(), QStringLiteral("/archive/101"));
        QCOMPARE(QUrlQuery(QUrl(url)).queryItemValue("from"), QStringLiteral("1700000000"));
        QCOMPARE(QUrl(url).host(), account);

        // Programme ids from the guide, and from a day's table.
        auto known = PortalClient::archiveIdsFromEpg(js("{'js':{'data':{'101':["
            "{'id':'p1','start_timestamp':'1700000000'},{'id':'p2','start_timestamp':1700003600}]}}}"));
        QCOMPARE(known.value("101").size(), 2);
        qint64 start = 0;
        QCOMPARE(PortalClient::archiveProgramme(known, QStringLiteral("101"), 1700000600, &start), QStringLiteral("p1"));
        QCOMPARE(start, qint64(1700000000));
        QCOMPARE(PortalClient::archiveProgramme(known, QStringLiteral("101"), 1700003600), QStringLiteral("p2"));
        QCOMPARE(PortalClient::archiveProgramme(known, QStringLiteral("101"), 1699999999), QString());
        QCOMPARE(PortalClient::archiveProgramme(known, QStringLiteral("102"), 1700000600), QString());
        int pages = 0;
        const auto table = PortalClient::parseArchiveTable(js("{'js':{'total_items':25,'max_page_items':10,'data':["
            "{'id':'x1','ch_id':'102','start_timestamp':'1700000000'}]}}"), &pages);
        QCOMPARE(pages, 3);
        QCOMPARE(table.value("102").value(1700000000), QStringLiteral("x1"));
    }

    void tmdb_logos_and_ratings()
    {
        // The logo: a PNG, in English (or without words) when there's none in the viewer's language; best voted.
        const QJsonArray logos = QJsonDocument::fromJson(R"([
            {"file_path":"/fr.png","iso_639_1":"fr","vote_average":9},
            {"file_path":"/en-low.png","iso_639_1":"en","vote_average":2},
            {"file_path":"/en.svg","iso_639_1":"en","vote_average":10},
            {"file_path":"/en-best.png","iso_639_1":"en","vote_average":5.3},
            {"file_path":"/none.png","iso_639_1":null,"vote_average":8}])").array();
        if (TmdbClient::languageCode() == QLatin1String("en")) QCOMPARE(TmdbClient::logoPath(logos), QStringLiteral("/en-best.png"));
        QCOMPARE(TmdbClient::logoPath(QJsonDocument::fromJson(R"([{"file_path":"/x.png","iso_639_1":null}])").array()), QStringLiteral("/x.png"));
        QVERIFY(TmdbClient::logoPath({}).isEmpty());
        // Age ratings: a film's from its release dates, a series' from its content ratings (the US one when the viewer's isn't there).
        const QJsonObject movie = QJsonDocument::fromJson(R"({"release_dates":{"results":[
            {"iso_3166_1":"ZZ","release_dates":[{"certification":"18"}]},
            {"iso_3166_1":"US","release_dates":[{"certification":""},{"certification":"PG-13"}]}]}})").object();
        QCOMPARE(TmdbClient::certification(movie, false), QStringLiteral("PG-13"));
        const QJsonObject tv = QJsonDocument::fromJson(R"({"content_ratings":{"results":[{"iso_3166_1":"US","rating":"TV-MA"}]}})").object();
        QCOMPARE(TmdbClient::certification(tv, true), QStringLiteral("TV-MA"));
        QVERIFY(TmdbClient::certification(QJsonObject(), true).isEmpty());
    }

    void saved_playlist()
    {
        // The channels saved for a quick start: no login in the file, a new
        // password still works, another source's list or a damaged file isn't used.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("channels.saved"));
        const QString user = QStringLiteral("viewer42"), pass = QStringLiteral("s3cretPass");
        PlaylistResult r;
        r.epgUrl = QStringLiteral("http://example.com:8080/xmltv.php?username=%1&password=%2").arg(user, pass);
        r.groupOrder = {QStringLiteral("News"), QStringLiteral("Sport")};
        r.skippedMovies = 12;
        r.skippedSeries = 34;
        for (int i = 1; i <= 3; ++i) {
            Channel c;
            c.number = i;
            c.name = QStringLiteral("Channel %1 é").arg(i);
            c.group = i == 3 ? QStringLiteral("Sport") : QStringLiteral("News");
            c.logo = QStringLiteral("http://img.example.com/%1.png").arg(i);
            c.url = QStringLiteral("http://example.com:8080/live/%1/%2/%3.ts").arg(user, pass).arg(1000 + i);
            c.tvgId = QStringLiteral("ch%1.uk").arg(i);
            c.tvgShiftMinutes = i * 30;
            c.catchupType = QStringLiteral("xc");
            c.catchupDays = i;
            r.channels << c;
        }
        const QStringList secrets{pass, user};
        const QByteArray key = SavedPlaylist::sourceKey(QStringLiteral("xtream\nhttp://example.com:8080\nts"), secrets);
        QVERIFY(SavedPlaylist::save(path, r, key, secrets));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        file.close();
        QVERIFY(!bytes.contains(pass.toUtf8()));
        QVERIFY(!bytes.contains(user.toUtf8()));

        PlaylistResult back;
        QVERIFY(SavedPlaylist::read(path, key, secrets, &back));
        QCOMPARE(back.channels.size(), 3);
        QCOMPARE(back.channels[2].url, r.channels[2].url);
        QCOMPARE(back.channels[2].name, r.channels[2].name);
        QCOMPARE(back.channels[1].tvgShiftMinutes, 60);
        QCOMPARE(back.channels[2].catchupDays, 3);
        QCOMPARE(back.epgUrl, r.epgUrl);
        QCOMPARE(back.groupOrder, r.groupOrder);
        QCOMPARE(back.skippedSeries, 34);
        QVERIFY(back.savedAt.isValid());
        QCOMPARE(back.digest, SavedPlaylist::digest(r));   // the same channels as loaded

        // A new password: the saved channels carry it.
        PlaylistResult renewed;
        QVERIFY(SavedPlaylist::read(path, key, {QStringLiteral("n3wPass"), user}, &renewed));
        QCOMPARE(renewed.channels[0].url, QStringLiteral("http://example.com:8080/live/viewer42/n3wPass/1001.ts"));
        // ...and the key doesn't change with it.
        QCOMPARE(SavedPlaylist::sourceKey(QStringLiteral("m3u\nhttp://h/get.php?username=u1&password=p1"),
                                          {QStringLiteral("p1"), QStringLiteral("u1")}),
                 SavedPlaylist::sourceKey(QStringLiteral("m3u\nhttp://h/get.php?username=u1&password=p2"),
                                          {QStringLiteral("p2"), QStringLiteral("u1")}));

        // Another server's list isn't used.
        PlaylistResult other;
        QVERIFY(!SavedPlaylist::read(path, SavedPlaylist::sourceKey(QStringLiteral("xtream\nhttp://other.example\nts"), secrets),
                                     secrets, &other));

        // A changed channel changes the fingerprint.
        PlaylistResult changed = r;
        changed.channels[1].name = QStringLiteral("Renamed");
        QVERIFY(SavedPlaylist::digest(changed) != SavedPlaylist::digest(r));

        // A damaged or cut-off file isn't used, and neither is a missing one.
        QByteArray damaged = bytes;
        damaged[damaged.size() / 2] = char(damaged[damaged.size() / 2] ^ 0x5a);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(damaged);
        file.close();
        PlaylistResult broken;
        QVERIFY(!SavedPlaylist::read(path, key, secrets, &broken));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes.left(bytes.size() - 7));
        file.close();
        QVERIFY(!SavedPlaylist::read(path, key, secrets, &broken));
        QVERIFY(!SavedPlaylist::read(dir.filePath(QStringLiteral("missing")), key, secrets, &broken));
        QVERIFY(broken.channels.isEmpty());

        // A password inside the username (or the reverse) still comes back exactly.
        PlaylistResult tricky;
        Channel t;
        t.url = QStringLiteral("http://h/live/abcdef/abc/7.ts");
        tricky.channels << t;
        const QStringList overlapping{QStringLiteral("abc"), QStringLiteral("abcdef")};
        QVERIFY(SavedPlaylist::save(path, tricky, key, overlapping));
        PlaylistResult trickyBack;
        QVERIFY(SavedPlaylist::read(path, key, overlapping, &trickyBack));
        QCOMPARE(trickyBack.channels[0].url, t.url);
        const QStringList reversed{QStringLiteral("abcdef"), QStringLiteral("abc")};
        QVERIFY(SavedPlaylist::save(path, tricky, key, reversed));
        QVERIFY(SavedPlaylist::read(path, key, reversed, &trickyBack));
        QCOMPARE(trickyBack.channels[0].url, t.url);
    }

    void tmdb_picture_sizes()
    {
        // Providers list posters at 600×900; a card 287 wide gets TMDB's 300×450, a grid's 170 wide its 220×330.
        const QString base = QStringLiteral("https://image.tmdb.org/t/p/");
        const QString big = base + QStringLiteral("w600_and_h900_bestv2/aBc123.jpg");
        QCOMPARE(TmdbClient::imageAtWidth(big, 287), base + QStringLiteral("w300_and_h450_bestv2/aBc123.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(big, 300), base + QStringLiteral("w300_and_h450_bestv2/aBc123.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(big, 170), base + QStringLiteral("w220_and_h330_bestv2/aBc123.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(big, 430), big);    // nothing smaller covers it
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("w600_and_h900_face/a.jpg"), 200), base + QStringLiteral("w220_and_h330_face/a.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("w300_and_h450_bestv2/a.jpg"), 287), base + QStringLiteral("w300_and_h450_bestv2/a.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("w533_and_h300_bestv2/a.jpg"), 200), base + QStringLiteral("w533_and_h300_bestv2/a.jpg"));   // not a pair known here
        // Plain widths and "original": the next width that covers it.
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("w1280/b.jpg"), 764), base + QStringLiteral("w780/b.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("original/b.png"), 300), base + QStringLiteral("w342/b.png"));
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("w780/b.jpg"), 780), base + QStringLiteral("w780/b.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("w154/s.jpg"), 300), base + QStringLiteral("w154/s.jpg"));     // already smaller
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("original/b.jpg"), 1900), base + QStringLiteral("original/b.jpg"));
        // Left alone: other hosts, other kinds of file, no width.
        QCOMPARE(TmdbClient::imageAtWidth(QStringLiteral("https://example.com/t/p/w600_and_h900_bestv2/a.jpg"), 287),
                 QStringLiteral("https://example.com/t/p/w600_and_h900_bestv2/a.jpg"));
        QCOMPARE(TmdbClient::imageAtWidth(base + QStringLiteral("original/logo.svg"), 300), base + QStringLiteral("original/logo.svg"));
        QCOMPARE(TmdbClient::imageAtWidth(big, 0), big);
        QCOMPARE(TmdbClient::imageAtWidth(QString(), 287), QString());
    }

    void tmdb_titles()
    {
        auto clean = [](const char *t, const char *y = "") { return TmdbClient::clean(QString::fromUtf8(t), QString::fromUtf8(y)); };
        QCOMPARE(clean("EN - The Matrix (1999) [4K]").title, QStringLiteral("The Matrix"));
        QCOMPARE(clean("EN - The Matrix (1999) [4K]").year, QStringLiteral("1999"));
        QCOMPARE(clean("|EN| Arrival 2016 FHD").title, QStringLiteral("Arrival"));
        QCOMPARE(clean("|EN| Arrival 2016 FHD").year, QStringLiteral("2016"));
        QCOMPARE(clean("[FR] Amélie MULTI HEVC").title, QStringLiteral("Amélie"));
        QCOMPARE(clean("4K-NF: Glass Onion").title, QStringLiteral("Glass Onion"));
        QCOMPARE(clean("The.Dark.Knight.2008.1080p.BluRay.x264").title, QStringLiteral("The Dark Knight"));
        QCOMPARE(clean("The.Dark.Knight.2008.1080p.BluRay.x264").year, QStringLiteral("2008"));
        QCOMPARE(clean("Dune: Part Two", "2024").year, QStringLiteral("2024"));
        QCOMPARE(clean("Dune: Part Two", "2024").title, QStringLiteral("Dune: Part Two"));
        const auto br = clean("Blade Runner 2049");
        QCOMPARE(br.title, QStringLiteral("Blade Runner"));
        QCOMPARE(br.withNumber, QStringLiteral("Blade Runner 2049"));
        QCOMPARE(clean("1917").title, QStringLiteral("1917"));          // a year-like title on its own stays
        QCOMPARE(clean("  ").title, QString());

        const QJsonObject answer = QJsonDocument::fromJson(js("{'results':["
            "{'title':'The Matrix Resurrections','release_date':'2021-12-16'},"
            "{'title':'The Matrix','release_date':'1999-03-30'},"
            "{'title':'The Matrix','release_date':'2021-01-01'}]}")).object();
        QCOMPARE(TmdbClient::bestMatch(answer, {QStringLiteral("The Matrix"), QStringLiteral("1999"), {}}, false), 1);
        QCOMPARE(TmdbClient::bestMatch(answer, {QStringLiteral("The Matrix"), QStringLiteral("2020"), {}}, false), 2);   // a year out
        QCOMPARE(TmdbClient::bestMatch(answer, {QStringLiteral("the matrix"), {}, {}}, false), 1);
        QCOMPARE(TmdbClient::bestMatch(answer, {QStringLiteral("Matrix Something"), {}, {}}, false), 0);
        QCOMPARE(TmdbClient::bestMatch(QJsonObject(), {QStringLiteral("X"), {}, {}}, false), -1);
        const QJsonObject shows = QJsonDocument::fromJson(js("{'results':[{'name':'Other','original_name':'Harbor Lights','first_air_date':'2022-01-01'}]}")).object();
        QCOMPARE(TmdbClient::bestMatch(shows, {QStringLiteral("Harbor Lights"), {}, {}}, true), 0);
    }

    void ts_to_hls()
    {
        // A synthetic stream: PAT, PMT (H.264 on PID 0x100, AAC on 0x101), then
        // one video packet every 40 ms carrying the clock, a key frame every second.
        auto pkt = [](int pid, bool start, int cc, const QByteArray &adapt, const QByteArray &payload) {
            QByteArray p(188, char(0xFF));
            p[0] = 0x47;
            p[1] = char((start ? 0x40 : 0) | ((pid >> 8) & 0x1F));
            p[2] = char(pid & 0xFF);
            int pos = 4;
            if (!adapt.isEmpty()) {
                p[3] = char(0x30 | (cc & 0xF));
                p[4] = char(adapt.size());
                for (int i = 0; i < adapt.size(); ++i) p[5 + i] = adapt[i];
                pos = 5 + int(adapt.size());
            } else {
                p[3] = char(0x10 | (cc & 0xF));
            }
            for (int i = 0; i < payload.size() && pos + i < 188; ++i) p[pos + i] = payload[i];
            return p;
        };
        const QByteArray pat = QByteArray::fromHex("00" "00b00d0001c100000001f0000000000000");
        const QByteArray pmt = QByteArray::fromHex("00" "02b0170001c10000e100f000" "1be100f000" "0fe101f000" "00000000");
        auto pcrField = [](qint64 ticks90k, bool key) {
            QByteArray a(7, '\0');
            a[0] = char(0x10 | (key ? 0x40 : 0));
            a[1] = char(ticks90k >> 25); a[2] = char(ticks90k >> 17); a[3] = char(ticks90k >> 9);
            a[4] = char(ticks90k >> 1); a[5] = char(((ticks90k & 1) << 7) | 0x7E); a[6] = 0;
            return a;
        };
        TsSegmenter seg(4.0, 6);
        QByteArray stream;
        stream += pkt(0, true, 0, {}, pat);
        stream += pkt(0x100, false, 0, {}, QByteArray(10, 'x'));   // before the PMT: dropped
        stream += pkt(0x1000, true, 0, {}, pmt);
        for (int i = 0; i < 25 * 30; ++i) {                          // 30 seconds
            const bool key = i % 25 == 0;
            stream += pkt(0x100, true, i, pcrField(qint64(i) * 3600, key), QByteArray(20, 'v'));
            stream += pkt(0x101, true, i, {}, QByteArray(20, 'a'));
        }
        // In awkward pieces, as from the network.
        for (int at = 0; at < stream.size(); at += 1000) seg.feed(stream.mid(at, 1000));
        QCOMPARE(seg.videoPid(), 0x100);
        QCOMPARE(seg.videoStreamType(), 0x1B);
        QVERIFY(seg.segments().size() >= 5);
        QCOMPARE(seg.segments().size(), 6);                          // only the newest kept
        for (const auto &s : seg.segments()) {
            QVERIFY(s.duration >= 3.9 && s.duration <= 5.1);         // cut at the next key frame after 4 s
            QCOMPARE(s.data.size() % 188, 0);
            QCOMPARE(s.data.left(188), pkt(0, true, 0, {}, pat));    // starts with PAT, PMT
            QCOMPARE(int(uchar(s.data[188 + 1]) & 0x1F) << 8 | uchar(s.data[188 + 2]), 0x1000);
            QCOMPARE(int(uchar(s.data[376 + 5])) & 0x40, 0x40);       // then a key frame
        }
        const QByteArray list = seg.playlist(QStringLiteral("seg/"));
        QVERIFY(list.startsWith("#EXTM3U"));
        QVERIFY(list.contains("#EXT-X-TARGETDURATION:4"));
        QVERIFY(list.contains("#EXT-X-MEDIA-SEQUENCE:" + QByteArray::number(seg.segments().first().sequence)));
        QVERIFY(list.contains("seg/" + QByteArray::number(seg.segments().last().sequence) + ".ts"));
        QVERIFY(!list.contains("#EXT-X-ENDLIST"));
        QCOMPARE(seg.segment(seg.segments().first().sequence), seg.segments().first().data);
        QVERIFY(seg.segment(0).isEmpty());                           // dropped long ago
        QVERIFY(TsSegmenter().playlist(QString()).isEmpty());
    }

    // A synthetic H.264 stream whose broadcaster never sets the random-access
    // flag: key frames only show in the video (SPS + IDR), every `gop` frames.
    static QByteArray h264Stream(int frames, int gop, int firstFrame = 0, qint64 clockStart = 0)
    {
        auto pkt = [](int pid, bool start, int cc, const QByteArray &adapt, const QByteArray &payload) {
            QByteArray p(188, char(0xFF));
            p[0] = 0x47;
            p[1] = char((start ? 0x40 : 0) | ((pid >> 8) & 0x1F));
            p[2] = char(pid & 0xFF);
            int pos = 4;
            if (!adapt.isEmpty()) {
                p[3] = char(0x30 | (cc & 0xF));
                p[4] = char(adapt.size());
                for (int i = 0; i < adapt.size(); ++i) p[5 + i] = adapt[i];
                pos = 5 + int(adapt.size());
            } else {
                p[3] = char(0x10 | (cc & 0xF));
            }
            for (int i = 0; i < payload.size() && pos + i < 188; ++i) p[pos + i] = payload[i];
            return p;
        };
        auto pcrField = [](qint64 t) {
            QByteArray a(7, '\0');
            a[0] = 0x10;   // PCR, no random-access flag
            a[1] = char(t >> 25); a[2] = char(t >> 17); a[3] = char(t >> 9);
            a[4] = char(t >> 1); a[5] = char(((t & 1) << 7) | 0x7E); a[6] = 0;
            return a;
        };
        const QByteArray pat = QByteArray::fromHex("00" "00b00d0001c100000001f0000000000000");
        const QByteArray pmt = QByteArray::fromHex("00" "02b0170001c10000e100f000" "1be100f000" "0fe101f000" "00000000");
        const QByteArray pes = QByteArray::fromHex("000001e000008080052100010001");   // PES header with a PTS
        QByteArray out;
        for (int i = firstFrame; i < firstFrame + frames; ++i) {
            if (i % 25 == firstFrame % 25) { out += pkt(0, true, i, {}, pat); out += pkt(0x1000, true, i, {}, pmt); }
            const bool key = i % gop == 0;
            const QByteArray nal = key ? QByteArray::fromHex("0000000109f0" "00000001674d4028" "0000000168ee3c80" "0000000165888400")
                                       : QByteArray::fromHex("0000000109f0" "00000001419a0000");
            out += pkt(0x100, true, i, pcrField(clockStart + qint64(i - firstFrame) * 3600), pes + nal);
            out += pkt(0x101, true, i, {}, QByteArray(20, 'a'));
        }
        return out;
    }

    void ts_key_frames_and_breaks()
    {
        // Key frames found in the video itself; the first segment waits for one.
        const QByteArray sps = QByteArray::fromHex("67");
        TsSegmenter seg(4.0, 8);
        const QByteArray stream = h264Stream(25 * 20, 50, 13);   // starts mid-GOP, a key frame every 2 s
        for (int at = 0; at < stream.size(); at += 777) seg.feed(stream.mid(at, 777));
        QVERIFY(seg.segments().size() >= 3);
        for (const auto &s : seg.segments()) {
            QVERIFY2(s.duration >= 3.9 && s.duration <= 4.1, qPrintable(QString::number(s.duration)));   // whole GOPs
            QVERIFY(TsSegmenter::keyFrameInPayload(s.data.constData() + 376, 0x1B));   // after PAT, PMT: a key frame
            QVERIFY(!s.discontinuity);
        }
        QVERIFY(!seg.playlist(QString()).contains("DISCONTINUITY"));
        QVERIFY(seg.joinData().startsWith(seg.segments().last().data.left(188)));

        // The provider drops and a new connection starts its clock elsewhere:
        // the break is marked, and the next segment starts on a key frame again.
        const int before = int(seg.segments().size());
        seg.feed(QByteArray(100, 0x47));   // half a packet from the old connection
        seg.discontinuity();
        const QByteArray again = h264Stream(25 * 12, 50, 7, 90000LL * 5000);
        for (int at = 0; at < again.size(); at += 1000) seg.feed(again.mid(at, 1000));
        const auto &list = seg.segments();
        int breaks = 0;
        for (const auto &s : list) {
            if (s.discontinuity) ++breaks;
            QVERIFY(TsSegmenter::keyFrameInPayload(s.data.constData() + 376, 0x1B));
        }
        QCOMPARE(breaks, 1);
        QVERIFY(list.size() > 1 && int(list.size()) <= 8);
        QVERIFY(seg.playlist(QString()).contains("#EXT-X-DISCONTINUITY\n"));
        Q_UNUSED(before);
        // Once the break has scrolled off, the playlist counts it.
        const QByteArray more = h264Stream(25 * 40, 50, 0, 90000LL * 9000);
        for (int at = 0; at < more.size(); at += 1000) seg.feed(more.mid(at, 1000));
        const QByteArray later = seg.playlist(QString());
        QVERIFY(!later.contains("#EXT-X-DISCONTINUITY\n"));
        QVERIFY(later.contains("#EXT-X-DISCONTINUITY-SEQUENCE:1\n"));

        // Not key frames: other slices, and data that isn't a frame start.
        const QByteArray s1 = h264Stream(2, 50, 1);                       // frame 1: a P slice
        QVERIFY(!TsSegmenter::keyFrameInPayload(s1.constData() + 2 * 188, 0x1B));
        Q_UNUSED(sps);
    }

    void cast_shared_live()
    {
        // A live channel for a TV: one provider connection however many times
        // the TV asks, each answer starting on a key frame, and when the
        // provider drops the channel, Relay connects again and the TV carries on.
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        int connections = 0;
        connect(&provider, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = provider.nextPendingConnection();
            ++connections;
            const int n = connections;
            connect(s, &QTcpSocket::readyRead, s, [s, n] {
                if (!s->readAll().contains("\r\n\r\n") || s->property("going").toBool()) return;
                s->setProperty("going", true);
                s->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n");
                // Eight seconds of video, sent quickly, a bit at a time.
                auto data = std::make_shared<QByteArray>(h264Stream(25 * 8, 50, n * 3, qint64(n) * 90000 * 1000));
                auto *t = new QTimer(s);
                QObject::connect(t, &QTimer::timeout, s, [s, data, t, n] {
                    s->write(data->left(20 * 188));
                    data->remove(0, 20 * 188);
                    if (data->isEmpty()) { t->stop(); if (n == 1) s->disconnectFromHost(); }   // the first one drops
                });
                t->start(5);
            });
        });
        CastRelay relay;
        CastRelay::Source tv;
        tv.url = QStringLiteral("http://127.0.0.1:%1/live/u/p/9.ts").arg(provider.serverPort());
        tv.contentType = QStringLiteral("video/vnd.dlna.mpeg-tts");
        tv.forceType = true;
        tv.dlnaFeatures = DlnaRenderer::features(true);
        tv.shareLive = true;
        const QString path = relay.add(tv);
        QVERIFY(path.endsWith(".ts"));
        QTRY_COMPARE_WITH_TIMEOUT(connections, 1, 3000);      // started before the TV asks

        QNetworkAccessManager net;
        const QUrl url(relay.urlFor(path, QHostAddress::LocalHost));
        QNetworkRequest head(url);
        QNetworkReply *h = net.head(head);
        QTRY_VERIFY_WITH_TIMEOUT(h->isFinished(), 5000);
        QCOMPARE(h->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(h->header(QNetworkRequest::ContentTypeHeader).toString(), QStringLiteral("video/vnd.dlna.mpeg-tts"));
        h->deleteLater();

        QTest::qWait(400);   // a segment's worth arrives
        auto got1 = std::make_shared<QByteArray>(), got2 = std::make_shared<QByteArray>();
        QNetworkReply *a = net.get(QNetworkRequest(url));
        connect(a, &QNetworkReply::readyRead, a, [a, got1] { got1->append(a->readAll()); });
        QTest::qWait(100);
        QNetworkReply *b = net.get(QNetworkRequest(url));                   // the TV asks again, overlapping
        connect(b, &QNetworkReply::readyRead, b, [b, got2] { got2->append(b->readAll()); });
        QTRY_VERIFY_WITH_TIMEOUT(got1->size() > 188 * 3 && got2->size() > 188 * 3, 5000);
        QCOMPARE(connections, 1);                                           // still one connection to the provider
        // Both start with PAT, PMT and a key frame.
        for (const auto &g : {got1, got2}) {
            QCOMPARE(uchar(g->at(0)), uchar(0x47));
            QCOMPARE((uchar(g->at(1)) & 0x1F) << 8 | uchar(g->at(2)), 0);
            QVERIFY(TsSegmenter::keyFrameInPayload(g->constData() + 376, 0x1B));
        }
        // The provider drops the channel: Relay connects again, the TV's answer goes on.
        QTRY_COMPARE_WITH_TIMEOUT(connections, 2, 8000);
        const qint64 sizeAtReconnect = got1->size();
        QTRY_VERIFY_WITH_TIMEOUT(got1->size() > sizeAtReconnect + 188 * 50, 5000);
        QVERIFY(a->isRunning());
        relay.clear();
        QTRY_VERIFY_WITH_TIMEOUT(a->isFinished() && b->isFinished(), 3000);
        a->deleteLater();
        b->deleteLater();
    }

    void languages_and_track_choice()
    {
        // Tags in either code form, names in titles, nothing from an "undefined" tag.
        QVERIFY(Languages::matches("en", "eng", ""));
        QVERIFY(Languages::matches("en", "en-US", ""));
        QVERIFY(!Languages::matches("en", "tur", "English"));          // the tag decides
        QVERIFY(Languages::matches("en", "", "English 5.1"));
        QVERIFY(Languages::matches("en", "und", "English"));
        QVERIFY(Languages::matches("tr", "", "Türkçe"));
        QVERIFY(Languages::matches("es", "", "Spanish (Latino)"));
        QVERIFY(!Languages::matches("en", "", "Englishman"));          // whole words only
        QCOMPARE(Languages::mpvList("en"), QStringLiteral("en,eng"));
        QCOMPARE(Languages::mpvList(""), QString());
#ifdef RELAY_HAVE_LIBAV
        const QList<Remuxer::Track> audio = { {"tur", "Türkçe", true}, {"eng", "English", false} };
        QCOMPARE(Remuxer::receiverDefault(audio), 0);
        QCOMPARE(Remuxer::choose(audio, 1, "eng", "English", ""), 1);          // what was picked
        QCOMPARE(Remuxer::choose(audio, 0, "eng", "English", ""), 1);          // same labels, another place
        QCOMPARE(Remuxer::choose(audio, -1, "", "", "en"), 1);                 // Settings' language
        QCOMPARE(Remuxer::choose(audio, -1, "", "", "fr"), -1);                // not there: the TV's own pick
        QCOMPARE(Remuxer::choose(audio, 1, "fre", "Français", "tr"), 0);       // another episode: Settings, then place
#endif
    }

#ifdef RELAY_HAVE_LIBAV
    void cast_remux_chosen_audio()
    {
        // A movie with Turkish first and English second, cast with English:
        // the receiver gets the picture and the English track only.
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
        if (ffmpeg.isEmpty() || ffprobe.isEmpty()) QSKIP("ffmpeg and ffprobe are needed to make a test movie");
        QTemporaryDir dir;
        const QString movie = dir.filePath("movie.mkv");
        QProcess make;
        make.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25",
                            "-f", "lavfi", "-i", "sine=frequency=440", "-f", "lavfi", "-i", "sine=frequency=880",
                            "-t", "60", "-map", "0", "-map", "1", "-map", "2",
                            "-c:v", "mpeg2video", "-g", "25", "-c:a", "aac",
                            "-metadata:s:a:0", "language=tur", "-metadata:s:a:0", "title=Türkçe",
                            "-metadata:s:a:1", "language=eng", "-metadata:s:a:1", "title=English",
                            "-disposition:a:0", "default", "-disposition:a:1", "0", movie});
        QVERIFY(make.waitForFinished(60000));
        QCOMPARE(make.exitCode(), 0);

        const Remuxer::Probe p = Remuxer::probe(movie, QString(), QString());
        QVERIFY2(p.error.isEmpty(), qPrintable(p.error));
        QCOMPARE(p.audio.size(), 2);
        QCOMPARE(p.audio[1].lang, QStringLiteral("eng"));
        QVERIFY(p.duration > 59 && p.duration < 61);

        auto audioOf = [&](const QByteArray &ts) {
            const QString f = dir.filePath(QStringLiteral("out%1.ts").arg(QRandomGenerator::global()->generate()));
            QFile out(f);
            out.open(QIODevice::WriteOnly);
            out.write(ts);
            out.close();
            QProcess pr;
            pr.start(ffprobe, {"-v", "error", "-show_streams", "-of", "json", f});
            pr.waitForFinished(30000);
            QStringList found;   // "video", "audio,eng"
            for (const QJsonValue &v : QJsonDocument::fromJson(pr.readAllStandardOutput()).object().value("streams").toArray()) {
                const QJsonObject o = v.toObject();
                const QString lang = o.value("tags").toObject().value("language").toString();
                found << o.value("codec_type").toString() + (lang.isEmpty() ? QString() : "," + lang);
            }
            return found;
        };

        CastRelay relay;
        QNetworkAccessManager net;
        auto fetch = [&](const QUrl &url) -> QByteArray {
            QNetworkReply *r = net.get(QNetworkRequest(url));
            QElapsedTimer t;
            t.start();
            while (!r->isFinished() && t.elapsed() < 20000) QTest::qWait(10);
            const QByteArray body = r->readAll();
            r->deleteLater();
            return body;
        };

        // Chromecast: HLS made from it, to the end of the movie.
        CastRelay::Source cc;
        cc.url = movie;
        cc.remux = true;
        cc.remuxAudio = 1;
        cc.asHls = true;
        const QString hlsPath = relay.add(cc);
        QVERIFY(hlsPath.endsWith("live.m3u8"));
        const QUrl list(relay.urlFor(hlsPath, QHostAddress::LocalHost));
        // It makes only a little ahead of the receiver, which hasn't asked yet.
        QTest::qWait(1500);
        QByteArray playlist = fetch(list);
        QVERIFY(playlist.count("#EXTINF") <= 8);
        QVERIFY(!playlist.contains("#EXT-X-ENDLIST"));
        QByteArray movieTs;
        int next = 0;
        for (int round = 0; round < 60 && !playlist.contains("#EXT-X-ENDLIST"); ++round) {
            playlist = fetch(list);
            for (const QByteArray &line : playlist.split('\n')) {
                if (line.isEmpty() || line.startsWith('#')) continue;
                const int seq = line.left(line.indexOf('.')).toInt();
                if (seq < next) continue;
                movieTs += fetch(list.resolved(QUrl(QString::fromUtf8(line))));
                next = seq + 1;
            }
            QTest::qWait(50);
        }
        QVERIFY(playlist.contains("#EXT-X-ENDLIST"));        // the whole movie, waiting for the receiver
        const QStringList streams = audioOf(movieTs);
        QVERIFY2(streams.size() == 2, qPrintable(streams.join(" / ")));
        QVERIFY(streams.contains("video"));
        QVERIFY(streams.contains("audio,eng"));
        relay.remove(hlsPath);

        // A TV: one MPEG-TS stream, from 10 s in.
        CastRelay::Source tv = cc;
        tv.asHls = false;
        tv.remuxStart = 10;
        tv.contentType = QStringLiteral("video/mp2t");
        const QString tsPath = relay.add(tv);
        const QByteArray stream = fetch(QUrl(relay.urlFor(tsPath, QHostAddress::LocalHost)));
        QVERIFY(stream.size() > 188 * 100);
        QVERIFY(audioOf(stream).contains("audio,eng"));
        QVERIFY(stream.size() < movieTs.size() * 0.9);        // it started part way in
    }
#endif

    void cast_devices()
    {
        // mDNS: a Chromecast answering the _googlecast._tcp question.
        auto name = [](const QStringList &labels) {
            QByteArray out;
            for (const QString &l : labels) { out += char(l.toUtf8().size()); out += l.toUtf8(); }
            return out + '\0';
        };
        auto rr = [](const QByteArray &owner, int type, const QByteArray &data) {
            QByteArray r = owner;
            r += char(type >> 8); r += char(type & 0xFF);
            r += QByteArray::fromHex("8001" "00000078");
            r += char(data.size() >> 8); r += char(data.size() & 0xFF);
            return r + data;
        };
        const QByteArray service = name({"_googlecast", "_tcp", "local"});
        const QByteArray instance = name({"Chromecast-abc123", "_googlecast", "_tcp", "local"});
        const QByteArray host = name({"abc123", "local"});
        QByteArray txt;
        for (const QByteArray kv : {QByteArray("id=abc123"), QByteArray("md=Chromecast Ultra"), QByteArray("fn=Living Room")}) { txt += char(kv.size()); txt += kv; }
        QByteArray packet = QByteArray::fromHex("0000840000000004" "00000000");
        packet[7] = 1; packet[9] = 0; packet[11] = 3;   // 1 answer, 3 additional
        packet += rr(service, 12, instance);
        packet += rr(instance, 33, QByteArray::fromHex("00000000" "1f49") + host);
        packet += rr(instance, 16, txt);
        packet += rr(host, 1, QByteArray::fromHex("c0a8010f"));
        const auto found = CastDiscovery::parseMdns(packet, QHostAddress(QStringLiteral("192.168.1.99")));
        QCOMPARE(found.size(), 1);
        QCOMPARE(found[0].name, QStringLiteral("Living Room"));
        QCOMPARE(found[0].model, QStringLiteral("Chromecast Ultra"));
        QCOMPARE(found[0].address.toString(), QStringLiteral("192.168.1.15"));
        QCOMPARE(found[0].port, quint16(8009));
        QCOMPARE(found[0].kind, QStringLiteral("chromecast"));
        QVERIFY(CastDiscovery::parseMdns("short", QHostAddress()).isEmpty());
        const QByteArray q = CastDiscovery::mdnsQuery("_googlecast._tcp.local");
        QVERIFY(q.contains("\x0b_googlecast\x04_tcp\x05local"));

        // SSDP and the TV's description.
        QCOMPARE(CastDiscovery::ssdpLocation("HTTP/1.1 200 OK\r\nST: x\r\nLocation: http://192.168.1.20:9197/dmr\r\n\r\n"),
                 QUrl(QStringLiteral("http://192.168.1.20:9197/dmr")));
        QVERIFY(CastDiscovery::ssdpSearch().contains("AVTransport:1"));
        const CastDevice tv = CastDiscovery::parseDescription(
            "<root><device><friendlyName>[TV] Samsung Q80</friendlyName><modelName>QE55</modelName><serviceList>"
            "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><controlURL>/rc</controlURL></service>"
            "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>upnp/control/AVTransport1</controlURL></service>"
            "</serviceList></device></root>", QUrl(QStringLiteral("http://192.168.1.20:9197/dmr")));
        QCOMPARE(tv.name, QStringLiteral("[TV] Samsung Q80"));
        QCOMPARE(tv.controlUrl, QUrl(QStringLiteral("http://192.168.1.20:9197/upnp/control/AVTransport1")));
        QCOMPARE(tv.kind, QStringLiteral("dlna"));
        QVERIFY(!CastDiscovery::parseDescription("<root><device><friendlyName>Speaker</friendlyName></device></root>",
                                                 QUrl(QStringLiteral("http://h/d"))).controlUrl.isValid());

        // DLNA requests carry escaped addresses and metadata.
        const QByteArray didl = DlnaRenderer::didl(QStringLiteral("http://h:1/s/a?x=1&y=2"), QStringLiteral("News <HD> & more"),
                                                   QStringLiteral("video/mp2t"), true);
        QVERIFY(didl.contains("http://h:1/s/a?x=1&amp;y=2"));
        QVERIFY(didl.contains("News &lt;HD&gt; &amp; more"));
        QVERIFY(didl.contains("http-get:*:video/mp2t:DLNA.ORG_OP=00"));
        QVERIFY(DlnaRenderer::soapBody(QStringLiteral("Play"), QStringLiteral("<Speed>1</Speed>")).contains("<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"><InstanceID>0</InstanceID><Speed>1</Speed></u:Play>"));

        // Cast protocol messages there and back, two at once and one in pieces.
        QByteArray buffer = ChromecastClient::frame(QStringLiteral("sender-0"), QStringLiteral("receiver-0"),
                                                    QStringLiteral("urn:x-cast:com.google.cast.tp.heartbeat"), "{\"type\":\"PING\"}");
        const QByteArray second = ChromecastClient::frame(QStringLiteral("receiver-0"), QStringLiteral("sender-0"),
                                                          QStringLiteral("urn:x-cast:com.google.cast.media"), QByteArray(300, 'x'));
        buffer += second.left(50);
        auto msgs = ChromecastClient::unframe(buffer);
        QCOMPARE(msgs.size(), 1);
        QCOMPARE(msgs[0].source, QStringLiteral("sender-0"));
        QCOMPARE(msgs[0].destination, QStringLiteral("receiver-0"));
        QCOMPARE(msgs[0].ns, QStringLiteral("urn:x-cast:com.google.cast.tp.heartbeat"));
        QCOMPARE(msgs[0].payload, QByteArray("{\"type\":\"PING\"}"));
        buffer += second.mid(50);
        msgs = ChromecastClient::unframe(buffer);
        QCOMPARE(msgs.size(), 1);
        QCOMPARE(msgs[0].payload.size(), 300);   // a long payload (two-byte length)
        QVERIFY(buffer.isEmpty());

        QCOMPARE(CastService::mimeFor(QStringLiteral("http://p/live/u/p/1.ts"), true), QStringLiteral("video/mp2t"));
        QCOMPARE(CastService::mimeFor(QStringLiteral("http://p/live/u/p/1"), true), QStringLiteral("video/mp2t"));
        QCOMPARE(CastService::mimeFor(QStringLiteral("http://p/x/index.m3u8?t=1"), true), QStringLiteral("application/x-mpegurl"));
        QCOMPARE(CastService::mimeFor(QStringLiteral("http://p/movie/u/p/5.MKV"), false), QStringLiteral("video/x-matroska"));
        QCOMPARE(CastService::mimeFor(QStringLiteral("http://p/movie/u/p/5"), false), QStringLiteral("video/mp4"));
    }

    // Samsung TVs name MPEG-TS their own way and give up on streams labelled
    // otherwise; Relay asks the TV and uses its name.
    void cast_tv_types()
    {
        const QByteArray desc = js("<?xml version='1.0'?><root><URLBase>http://192.168.1.30:9197/</URLBase><device>"
            "<friendlyName>[TV] Samsung 7 Series (50)</friendlyName><modelName>UN50TU7000</modelName><serviceList>"
            "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/cm</controlURL></service>"
            "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>/avt</controlURL></service>"
            "</serviceList></device></root>");
        const CastDevice d = CastDiscovery::parseDescription(desc, QUrl("http://192.168.1.30:9197/dmr"));
        QCOMPARE(d.controlUrl, QUrl("http://192.168.1.30:9197/avt"));
        QCOMPARE(d.connectionUrl, QUrl("http://192.168.1.30:9197/cm"));

        const QString samsung = "http-get:*:video/vnd.dlna.mpeg-tts:DLNA.ORG_PN=AVC_TS_HD_NA;DLNA.ORG_FLAGS=ED100000000000000000000000000000,"
                                "http-get:*:video/mpeg:*,http-get:*:video/mp4:*,http-get:*:video/x-mkv:*";
        QCOMPARE(DlnaRenderer::pickType(samsung, "video/mp2t"), QString("video/vnd.dlna.mpeg-tts"));
        QCOMPARE(DlnaRenderer::pickType(samsung, "video/x-matroska"), QString("video/x-mkv"));
        QCOMPARE(DlnaRenderer::pickType(samsung, "video/mp4"), QString("video/mp4"));
        // A TV that knows video/mp2t keeps it; one that says nothing gets what it is.
        QCOMPARE(DlnaRenderer::pickType("http-get:*:video/mpeg:*,http-get:*:video/mp2t:*", "video/mp2t"), QString("video/mp2t"));
        QCOMPARE(DlnaRenderer::pickType("", "video/mp2t"), QString("video/mp2t"));
        QCOMPARE(DlnaRenderer::pickType("http-get:*:audio/mpeg:*", "video/mp2t"), QString("video/mp2t"));
        // HLS (the second try for a live channel), under the TV's name for it.
        QCOMPARE(DlnaRenderer::pickType("http-get:*:application/x-mpegURL:*", "application/vnd.apple.mpegurl"),
                 QString("application/x-mpegURL"));

        // Asking ConnectionManager takes no InstanceID; AVTransport does.
        const QByteArray ask = DlnaRenderer::soapBody("GetProtocolInfo", QString(), "urn:schemas-upnp-org:service:ConnectionManager:1");
        QVERIFY(ask.contains("<u:GetProtocolInfo xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\"></u:GetProtocolInfo>"));
        QVERIFY(DlnaRenderer::soapBody("Play", "<Speed>1</Speed>").contains("<InstanceID>0</InstanceID><Speed>1</Speed>"));

        // The same seek flags in the metadata and on the stream: live can't be sought.
        QVERIFY(DlnaRenderer::didl("http://x/s", "T", "video/vnd.dlna.mpeg-tts", true)
                    .contains("http-get:*:video/vnd.dlna.mpeg-tts:" + DlnaRenderer::features(true).toUtf8()));
        QVERIFY(DlnaRenderer::features(true).startsWith("DLNA.ORG_OP=00"));
        QVERIFY(DlnaRenderer::features(false).startsWith("DLNA.ORG_OP=01"));
    }

    // A Samsung TV plays MPEG-TS but not HLS: an HLS channel goes to it as one
    // continuous TS stream made of the channel's segments.
    void cast_hls_to_ts()
    {
        const QUrl base("http://p/live/ch/index.m3u8");
        auto master = HlsJoiner::parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000\nlow/i.m3u8\n"
                                       "#EXT-X-STREAM-INF:BANDWIDTH=4000000,RESOLUTION=1920x1080\nhigh/i.m3u8\n", base);
        QVERIFY(master.master);
        QCOMPARE(HlsJoiner::pickVariant(master), QUrl("http://p/live/ch/high/i.m3u8"));
        auto media = HlsJoiner::parse("#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:120\n"
                                      "#EXTINF:4.0,\n120.ts\n#EXTINF:4.0,\n/abs/121.ts\n", base);
        QVERIFY(!media.master);
        QCOMPARE(media.sequence, 120);
        QCOMPARE(media.targetDuration, 4.0);
        QCOMPARE(media.segments, (QList<QUrl>{QUrl("http://p/live/ch/120.ts"), QUrl("http://p/abs/121.ts")}));
        QVERIFY(HlsJoiner::parse("#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n1.ts\n", base).encrypted);
        QVERIFY(!HlsJoiner::parse("#EXT-X-KEY:METHOD=NONE\n1.ts\n", base).encrypted);
        QVERIFY(HlsJoiner::parse("#EXT-X-MAP:URI=\"init.mp4\"\n1.m4s\n", base).fmp4);

        // The whole way: master -> media playlist -> the last 3 segments, in order.
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        QStringList asked;
        connect(&provider, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = provider.nextPendingConnection();
            auto buf = std::make_shared<QByteArray>();
            connect(s, &QTcpSocket::readyRead, s, [s, buf, &asked] {
                *buf += s->readAll();
                if (!buf->contains("\r\n\r\n")) return;
                const QByteArray path = buf->split(' ').value(1);
                asked << QString::fromLatin1(path);
                QByteArray body, type = "application/vnd.apple.mpegurl";
                if (path == "/live/ch.m3u8") body = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nhd.m3u8\n";
                else if (path == "/live/hd.m3u8")
                    body = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:7\n"
                           "#EXTINF:2,\ns7.ts\n#EXTINF:2,\ns8.ts\n#EXTINF:2,\ns9.ts\n#EXTINF:2,\ns10.ts\n#EXT-X-ENDLIST\n";
                else if (path.startsWith("/live/s")) { type = "video/mp2t"; body = QByteArray(188, char(0x47)) + path.mid(6); }
                s->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size())
                         + "\r\nConnection: close\r\n\r\n" + body);
                s->disconnectFromHost();
            });
        });
        CastRelay relay;
        CastRelay::Source src;
        src.url = QStringLiteral("http://127.0.0.1:%1/live/ch.m3u8").arg(provider.serverPort());
        src.contentType = QStringLiteral("video/vnd.dlna.mpeg-tts");
        src.forceType = true;
        src.dlnaFeatures = DlnaRenderer::features(true);
        src.joinHls = true;
        const QString path = relay.add(src);
        QVERIFY(path.endsWith(".ts"));
        QNetworkAccessManager net;
        QNetworkRequest req{QUrl(relay.urlFor(path, QHostAddress::LocalHost))};
        req.setRawHeader("getcontentFeatures.dlna.org", "1");
        // HEAD first, the way the TV does: headers, no body.
        QNetworkReply *head = net.head(req);
        QTRY_VERIFY_WITH_TIMEOUT(head->isFinished(), 5000);
        QCOMPARE(head->header(QNetworkRequest::ContentTypeHeader).toString(), QStringLiteral("video/vnd.dlna.mpeg-tts"));
        QCOMPARE(head->rawHeader("transferMode.dlna.org"), QByteArray("Streaming"));
        head->deleteLater();
        QNetworkReply *r = net.get(req);
        QTRY_VERIFY_WITH_TIMEOUT(r->isFinished(), 10000);
        const QByteArray got = r->readAll();
        // The playlist ended (a short test), so from the start: s7..s10, in order.
        const QByteArray want = QByteArray(188, char(0x47)) + "s7.ts" + QByteArray(188, char(0x47)) + "s8.ts"
                              + QByteArray(188, char(0x47)) + "s9.ts" + QByteArray(188, char(0x47)) + "s10.ts";
        QCOMPARE(got, want);
        QVERIFY2(asked.mid(0, 2) == (QStringList{"/live/ch.m3u8", "/live/hd.m3u8"}), qPrintable(asked.join(", ")));
        r->deleteLater();
    }

    // A movie through the relay to a player that reads slowly (the TV's, a
    // Chromecast): Relay reads from the provider only as fast as the player
    // takes it. Reading ahead without limit filled the TV's memory and
    // restarted it within half a minute.
    void cast_movie_reads_at_players_pace()
    {
        const qint64 size = 2000LL * 1024 * 1024;   // a 2 GB movie, sent as fast as the network allows
        qint64 sent = 0;
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        connect(&provider, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = provider.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [&, s] {
                const QByteArray req = s->readAll();
                if (req.startsWith("GET /movie/u/p/1.mp4 ")) {   // the playlist's server sends it elsewhere
                    s->write("HTTP/1.1 302 Found\r\nLocation: /stream/1.mp4?token=t\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    s->disconnectFromHost();
                    return;
                }
                s->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nAccept-Ranges: bytes\r\nContent-Length: "
                         + QByteArray::number(size) + "\r\nConnection: close\r\n\r\n");
                auto more = [&, s] {
                    while (s->bytesToWrite() < 1024 * 1024 && sent < size) {
                        s->write(QByteArray(256 * 1024, 'M'));
                        sent += 256 * 1024;
                    }
                };
                connect(s, &QTcpSocket::bytesWritten, s, more);
                more();
            });
        });
        CastRelay relay;
        CastRelay::Source movie;
        movie.url = QStringLiteral("http://127.0.0.1:%1/movie/u/p/1.mp4").arg(provider.serverPort());
        const QString path = relay.add(movie);
        QVERIFY(!path.isEmpty());

        // The player: asks for the movie, takes 1 MB, then pauses (its buffer is full).
        QTcpSocket player;
        player.setReadBufferSize(64 * 1024);
        player.connectToHost(QHostAddress::LocalHost, relay.port());
        QVERIFY(player.waitForConnected(3000));
        player.write("GET " + path.toLatin1() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        qint64 got = 0;
        connect(&player, &QTcpSocket::readyRead, &player, [&] { if (got < 1024 * 1024) got += player.read(64 * 1024).size(); });
        QTRY_VERIFY_WITH_TIMEOUT(got >= 1024 * 1024, 5000);
        QTest::qWait(3000);
        // Relay's own buffers (2 MB from the provider, 4 MB to the player) plus
        // the system's socket buffers; not the movie.
        QVERIFY2(sent < 64LL * 1024 * 1024, qPrintable(QStringLiteral("the provider sent %1 MB while the player took 1 MB")
                                                         .arg(sent / (1024 * 1024))));
    }

    // A provider that doesn't answer a request at all: the relay asks again
    // from the first address, before the player gives up.
    void cast_asks_again_when_provider_hangs()
    {
        int requests = 0;
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        connect(&provider, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = provider.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [&, s] {
                const QByteArray req = s->readAll();
                if (!req.startsWith("GET ")) return;
                if (++requests == 1) return;   // the first one: silence
                if (req.startsWith("GET /movie/u/p/1.mkv ")) {
                    s->write("HTTP/1.1 302 Found\r\nLocation: /stream/1.mkv?token=t\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    QVERIFY(req.contains("\r\nRange: bytes=100-"));
                    s->write("HTTP/1.1 206 Partial Content\r\nContent-Type: video/x-matroska\r\nContent-Range: bytes 100-104/105\r\n"
                             "Content-Length: 5\r\nConnection: close\r\n\r\nmovie");
                }
                s->disconnectFromHost();
            });
        });
        CastRelay relay;
        relay.setAnswerTimeout(300);
        CastRelay::Source movie;
        movie.url = QStringLiteral("http://127.0.0.1:%1/movie/u/p/1.mkv").arg(provider.serverPort());
        const QString path = relay.add(movie);
        QNetworkAccessManager net;
        QNetworkRequest req{QUrl(relay.urlFor(path, QHostAddress::LocalHost))};
        req.setRawHeader("Range", "bytes=100-");
        QNetworkReply *r = net.get(req);
        QTRY_VERIFY_WITH_TIMEOUT(r->isFinished(), 5000);
        QCOMPARE(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 206);
        QCOMPARE(r->readAll(), QByteArray("movie"));
        QCOMPARE(requests, 3);   // the silent one, then the playlist's address again and its redirect
        r->deleteLater();
    }

    // A Fire TV (or anything else) added as a Chromecast: say what it is.
    void cast_not_a_chromecast()
    {
        QTcpServer fireTv;
        QVERIFY(fireTv.listen(QHostAddress::LocalHost));
        connect(&fireTv, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = fireTv.nextPendingConnection();
            s->disconnectFromHost();
        });
        CastDevice d;
        d.kind = QStringLiteral("chromecast");
        d.address = QHostAddress::LocalHost;
        d.port = fireTv.serverPort();
        ChromecastClient cc(d);
        QSignalSpy failed(&cc, &ChromecastClient::failed);
        cc.load(QStringLiteral("http://127.0.0.1/x"), QStringLiteral("T"), QStringLiteral("video/mp4"), false, 0);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY2(failed.first().first().toString().contains("doesn't answer like a Chromecast"),
                 qPrintable(failed.first().first().toString()));
    }

    // Remotes, controllers and the phone: one set of actions.
    void remote_actions()
    {
        QCOMPARE(RemoteInput::keyFor("ok"), int(Qt::Key_Return));
        QCOMPARE(RemoteInput::keyFor("back"), int(Qt::Key_Escape));
        QCOMPARE(RemoteInput::keyFor("chup"), int(Qt::Key_ChannelUp));
        QCOMPARE(RemoteInput::keyFor("rewind"), int(Qt::Key_AudioRewind));
        QCOMPARE(RemoteInput::keyFor("guide"), 0);   // the user's key: QML decides
        QCOMPARE(RemoteInput::actionForRemoteKey(Qt::Key_Back), QString("back"));
        QCOMPARE(RemoteInput::actionForRemoteKey(Qt::Key_HomePage), QString("home"));
        QCOMPARE(RemoteInput::actionForRemoteKey(Qt::Key_Menu), QString("menu"));
        QCOMPARE(RemoteInput::actionForRemoteKey(Qt::Key_A), QString());
        RemoteInput r;
        QSignalSpy spy(&r, &RemoteInput::action);
        r.trigger("guide");
        r.trigger("ok");   // a key, not an action()
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QString("guide"));
        QVERIFY(RemoteInput::actions().contains("home"));
    }

    // Controller buttons and sticks -> Relay's actions; a virtual pad end to end.
    void gamepad_actions()
    {
        QCOMPARE(Gamepads::actionForButton(0), QString("ok"));       // A / Cross
        QCOMPARE(Gamepads::actionForButton(1), QString("back"));     // B / Circle
        QCOMPARE(Gamepads::actionForButton(3), QString("guide"));    // Y / Triangle
        QCOMPARE(Gamepads::actionForButton(5), QString("home"));     // Xbox / PS button
        QCOMPARE(Gamepads::actionForButton(11), QString("up"));      // D-pad
        QCOMPARE(Gamepads::actionForAxis(1, -30000), QString("up"));
        QCOMPARE(Gamepads::actionForAxis(1, 3000), QString());       // resting: nothing
        QCOMPARE(Gamepads::actionForAxis(5, 30000), QString("pagedown"));
#ifdef RELAY_HAVE_SDL
        RemoteInput remote;
        Gamepads pads(&remote);
        QVERIFY(pads.available());
        pads.setRequireActive(false);
        QSignalSpy connected(&pads, &Gamepads::connectedChanged);
        QSignalSpy actions(&remote, &RemoteInput::action);
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
                                                    SDL_CONTROLLER_BUTTON_MAX, 0);
        QVERIFY2(index >= 0, SDL_GetError());
        SDL_Joystick *j = SDL_JoystickOpen(index);
        QVERIFY(j);
        QTRY_VERIFY(!pads.connected().isEmpty());
        SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_Y, 1);
        QTRY_COMPARE(actions.count(), 1);
        QCOMPARE(actions.first().first().toString(), QString("guide"));
        SDL_JoystickSetVirtualButton(j, SDL_CONTROLLER_BUTTON_Y, 0);
        SDL_JoystickClose(j);
        SDL_JoystickDetachVirtual(index);
        QTRY_VERIFY(pads.connected().isEmpty());
#endif
    }

    // The phone remote: page, pairing, buttons; unpaired phones can't press anything.
    void phone_remote()
    {
        QVERIFY(PhoneRemote::isLocal(QHostAddress("192.168.0.20")));
        QVERIFY(PhoneRemote::isLocal(QHostAddress("::ffff:10.1.2.3")));
        QVERIFY(PhoneRemote::isLocal(QHostAddress("127.0.0.1")));
        QVERIFY(!PhoneRemote::isLocal(QHostAddress("8.8.8.8")));
        const QVariantMap q = PhoneRemote::qr("http://192.168.0.231:8800");
        QVERIFY(q.value("size").toInt() >= 21);
        QCOMPARE(q.value("modules").toString().size(), q.value("size").toInt() * q.value("size").toInt());

        QSettings().remove("phone");
        RemoteInput remote;
        PhoneRemote phone(&remote);
        QVERIFY(!phone.running());   // off until it's turned on
        phone.setEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(phone.running(), 12000);   // 8800, or the next one after waiting for it
        QCOMPARE(phone.code().size(), 4);
        phone.setNowShowing("4791 · Take the Lead");
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(phone.port());
        QNetworkAccessManager net;
        auto call = [&](const QByteArray &method, const QString &path, const QByteArray &body = {}, const QByteArray &token = {}) {
            QNetworkRequest req{QUrl(base + path)};
            if (!token.isEmpty()) req.setRawHeader("X-Relay-Token", token);
            if (!body.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QNetworkReply *r = net.sendCustomRequest(req, method, body);
            QSignalSpy done(r, &QNetworkReply::finished);
            done.wait(5000);
            r->deleteLater();
            return qMakePair(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), r->readAll());
        };
        auto page = call("GET", "/");
        QCOMPARE(page.first, 200);
        QVERIFY(page.second.contains("Pair this phone"));
        QCOMPARE(call("POST", "/a/guide").first, 403);                       // not paired
        QCOMPARE(call("POST", "/pair", "{\"code\":\"nope\"}").first, 403);    // wrong code
        auto paired = call("POST", "/pair", QJsonDocument(QJsonObject{{"code", phone.code()}}).toJson());
        QCOMPARE(paired.first, 200);
        const QByteArray token = QJsonDocument::fromJson(paired.second).object().value("token").toString().toLatin1();
        QVERIFY(token.size() >= 32);
        QCOMPARE(phone.paired(), 1);
        auto state = call("GET", "/state", {}, token);
        QVERIFY(state.second.contains("\"paired\":true"));
        QVERIFY(state.second.contains("Take the Lead"));
        QSignalSpy actions(&remote, &RemoteInput::action);
        QCOMPARE(call("POST", "/a/guide", {}, token).first, 204);
        QCOMPARE(actions.count(), 1);
        QCOMPARE(actions.first().first().toString(), QString("guide"));
        QCOMPARE(call("POST", "/a/format-disk", {}, token).first, 400);     // only Relay's buttons
        // Forgetting phones: the token stops working, and the code changes.
        const QString oldCode = phone.code();
        phone.forgetPhones();
        QCOMPARE(call("POST", "/a/guide", {}, token).first, 403);
        QVERIFY(phone.code() != oldCode || phone.code().size() == 4);
        // Five wrong codes: a new code, and a minute's wait.
        for (int i = 0; i < 5; ++i) call("POST", "/pair", "{\"code\":\"x\"}");
        QCOMPARE(call("POST", "/pair", QJsonDocument(QJsonObject{{"code", phone.code()}}).toJson()).first, 429);
        phone.setEnabled(false);
        QVERIFY(!phone.running());
        QSettings().remove("phone");
    }

    // The browse home's rows, and trailers.
    void vod_browse_and_trailers()
    {
        QCOMPARE(Trailers::youtubeId("dQw4w9WgXcQ"), QString("dQw4w9WgXcQ"));
        QCOMPARE(Trailers::youtubeId("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=3"), QString("dQw4w9WgXcQ"));
        QCOMPARE(Trailers::youtubeId("https://youtu.be/dQw4w9WgXcQ"), QString("dQw4w9WgXcQ"));
        QCOMPARE(Trailers::youtubeId("https://www.youtube.com/embed/dQw4w9WgXcQ"), QString("dQw4w9WgXcQ"));
        QCOMPARE(Trailers::youtubeId("http://cdn/trailer.mp4"), QString());
        QCOMPARE(Trailers::pageUrl("dQw4w9WgXcQ"), QString("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
        const QJsonArray videos = QJsonDocument::fromJson(js("[{'site':'YouTube','type':'Teaser','key':'tease','official':true},"
            "{'site':'Vimeo','type':'Trailer','key':'vim'},{'site':'YouTube','type':'Trailer','key':'fan','official':false},"
            "{'site':'YouTube','type':'Trailer','key':'real','official':true}]")).array();
        QCOMPARE(TmdbClient::trailerKey(videos), QString("real"));
        QCOMPARE(TmdbClient::trailerKey(QJsonArray()), QString());

        // yt-dlp (a stand-in here): YouTube -> video and audio addresses, remembered.
        QTemporaryDir dir;
#ifndef Q_OS_WIN
        const QString tool = dir.filePath("yt-dlp");
        {
            QFile f(tool);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("#!/bin/sh\necho \"$@\" >> \"$0.calls\"\necho http://v/video.mp4\necho http://v/audio.m4a\n");
        }
        QFile::setPermissions(tool, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("RELAY_YTDLP", tool.toUtf8());
        {
            Trailers t;
            QVERIFY(t.available());
            QSignalSpy ok(&t, &Trailers::resolved);
            t.resolve("dQw4w9WgXcQ");
            QTRY_COMPARE(ok.count(), 1);
            QCOMPARE(ok.first().at(1).toString(), QString("http://v/video.mp4"));
            QCOMPARE(ok.first().at(2).toString(), QString("http://v/audio.m4a"));
            t.resolve("https://youtu.be/dQw4w9WgXcQ");   // the same one: no second run
            QCOMPARE(ok.count(), 2);
            QFile calls(tool + ".calls");
            QVERIFY(calls.open(QIODevice::ReadOnly));
            QCOMPARE(calls.readAll().count('\n'), 1);
            t.resolve("http://cdn/trailer.mp4");          // direct: as it is
            QCOMPARE(ok.count(), 3);
            QCOMPARE(ok.last().at(1).toString(), QString("http://cdn/trailer.mp4"));
        }
        qunsetenv("RELAY_YTDLP");
#endif

        // Rows: Continue watching and Favorites first, then the provider's categories.
        UserLibrary lib(dir.filePath("library.json"));
        VodService vod(&lib, dir.filePath("vod"));
        vod.setSource(M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1,Live\nhttp://h/live/u/p/1.ts\n"
            "#EXTINF:-1 group-title=\"Shows\",Show S01 E01\nhttp://h/series/u/p/1.mp4\n"
            "#EXTINF:-1 group-title=\"Films\",Alpha Film\nhttp://cdn/a.mp4\n"
            "#EXTINF:-1 group-title=\"Films\",Beta Film\nhttp://cdn/b.mp4\n"
            "#EXTINF:-1 group-title=\"Other\",Gamma\nhttp://cdn/c.mkv\n"));
        vod.ensureLoaded();
        QTRY_VERIFY2(vod.loaded(), qPrintable(vod.status() + " | " + vod.error()));
        auto ids = [&] { QStringList out; for (const QVariant &v : vod.shelves(24)) out << v.toMap().value("id").toString(); return out; };
        QCOMPARE(ids(), (QStringList{"c:Films", "c:Other"}));
        const QVariantMap films = vod.shelves(24).first().toMap();
        QCOMPARE(films.value("label").toString(), QString("Films"));
        QCOMPARE(films.value("items").toList().size(), 2);
        QCOMPARE(films.value("items").toList().first().toMap().value("progress").toDouble(), -1.0);

        QSignalSpy details(&vod, &VodService::detailsReady);
        const QString gamma = vod.shelves(24).last().toMap().value("items").toList().first().toMap().value("id").toString();
        vod.requestDetails("movie", gamma);
        QTRY_COMPARE(details.count(), 1);
        const QVariantMap d = details.takeFirst().first().toMap();
        vod.saveProgress(vod.playback(d), 300, 3000);
        vod.toggleFavorite("movie", gamma);
        QCOMPARE(ids(), (QStringList{"continue", "favorites", "c:Films", "c:Other"}));
        const QVariantMap cont = vod.shelves(24).first().toMap().value("items").toList().first().toMap();
        QCOMPARE(cont.value("title").toString(), QString("Gamma"));
        QCOMPARE(cont.value("progress").toDouble(), 0.1);
        // Details asked again (remembered): the favorite and progress as they are now.
        vod.requestDetails("movie", gamma);
        QTRY_COMPARE(details.count(), 1);
        const QVariantMap again = details.takeFirst().first().toMap();
        QVERIFY(again.value("favorite").toBool());
        QCOMPARE(again.value("pos").toDouble(), 300.0);
    }

    void cast_relay()
    {
        // Playlists are rewritten so every part comes through Relay.
        QStringList seen;
        const QByteArray out = CastRelay::rewritePlaylist(
            "#EXTM3U\r\n#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\"\r\n#EXTINF:4,\r\nseg1.ts?tok=a\r\n#EXTINF:4,\r\nhttp://cdn.example/x/seg2.ts\r\n",
            QUrl(QStringLiteral("http://p.example/live/chan/index.m3u8")),
            [&seen](const QString &target) { seen << target; return QStringLiteral("/u/T/%1.ts").arg(seen.size() - 1); });
        QCOMPARE(seen, QStringList({QStringLiteral("http://p.example/live/chan/key.bin"),
                                    QStringLiteral("http://p.example/live/chan/seg1.ts?tok=a"),
                                    QStringLiteral("http://cdn.example/x/seg2.ts")}));
        QVERIFY(out.contains("URI=\"/u/T/0.ts\""));
        QVERIFY(out.contains("\n/u/T/1.ts\n"));
        QVERIFY(out.contains("\n/u/T/2.ts\n"));
        QVERIFY(!out.contains('\r'));

        // The relay itself: a stream from a little server, through Relay, with
        // the channel's user agent going to the provider.
        QTcpServer provider;
        QVERIFY(provider.listen(QHostAddress::LocalHost));
        QByteArray askedWith;
        connect(&provider, &QTcpServer::newConnection, this, [&] {
            QTcpSocket *s = provider.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, &askedWith] {
                askedWith += s->readAll();
                if (!askedWith.contains("\r\n\r\n")) return;
                s->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 1880\r\nConnection: close\r\n\r\n");
                s->write(QByteArray(1880, 0x47));
                s->disconnectFromHost();
            });
        });
        CastRelay relay;
        const QString path = relay.add({QStringLiteral("http://127.0.0.1:%1/live/1.ts").arg(provider.serverPort()),
                                        QStringLiteral("TestAgent/1"), QString(), false, QStringLiteral("video/mp2t")});
        QVERIFY(path.startsWith(QLatin1String("/s/")));
        const QString url = relay.urlFor(path, QHostAddress::LocalHost);
        QVERIFY(url.startsWith(QLatin1String("http://127.0.0.1:")));
        QNetworkAccessManager net;
        QNetworkRequest req{QUrl(url)};
        req.setRawHeader("getcontentFeatures.dlna.org", "1");
        QNetworkReply *r = net.get(req);
        QTRY_VERIFY_WITH_TIMEOUT(r->isFinished(), 5000);
        QCOMPARE(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QCOMPARE(r->readAll(), QByteArray(1880, 0x47));
        QCOMPARE(r->header(QNetworkRequest::ContentTypeHeader).toString(), QStringLiteral("video/mp2t"));
        QVERIFY(r->hasRawHeader("contentFeatures.dlna.org"));
        QCOMPARE(r->rawHeader("Access-Control-Allow-Origin"), QByteArray("*"));
        QVERIFY(askedWith.contains("User-Agent: TestAgent/1"));
        r->deleteLater();

        // For a DLNA TV: the TV's name for the type, even over the provider's,
        // and the same live flags the TV was given.
        CastRelay::Source tv;
        tv.url = QStringLiteral("http://127.0.0.1:%1/live/2.ts").arg(provider.serverPort());
        tv.contentType = QStringLiteral("video/vnd.dlna.mpeg-tts");
        tv.forceType = true;
        tv.dlnaFeatures = DlnaRenderer::features(true);
        askedWith.clear();
        const QString tvPath = relay.add(tv);
        QVERIFY2(tvPath.endsWith(".ts"), qPrintable(tvPath));   // Samsung goes by the ending
        QNetworkRequest tvReq{QUrl(relay.urlFor(tvPath, QHostAddress::LocalHost))};
        tvReq.setRawHeader("getcontentFeatures.dlna.org", "1");
        QNetworkReply *tr = net.get(tvReq);
        QTRY_VERIFY_WITH_TIMEOUT(tr->isFinished(), 5000);
        QCOMPARE(tr->header(QNetworkRequest::ContentTypeHeader).toString(), QStringLiteral("video/vnd.dlna.mpeg-tts"));
        QCOMPARE(QString::fromLatin1(tr->rawHeader("contentFeatures.dlna.org")), DlnaRenderer::features(true));
        tr->deleteLater();

        // Unknown or stopped streams aren't served.
        QNetworkReply *bad = net.get(QNetworkRequest(QUrl(relay.urlFor(QStringLiteral("/s/nope/stream"), QHostAddress::LocalHost))));
        QTRY_VERIFY_WITH_TIMEOUT(bad->isFinished(), 5000);
        QCOMPARE(bad->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
        relay.remove(path);
        QNetworkReply *gone = net.get(QNetworkRequest(QUrl(url)));
        QTRY_VERIFY_WITH_TIMEOUT(gone->isFinished(), 5000);
        QCOMPARE(gone->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
        bad->deleteLater();
        gone->deleteLater();
    }


    void update_versions()
    {
        QCOMPARE(Updater::compareVersions("0.10.0", "0.9.9"), 1);
        QCOMPARE(Updater::compareVersions("v1.2", "1.2.0"), 0);
        QCOMPARE(Updater::compareVersions("1.2.0", "1.2.1"), -1);
        QCOMPARE(Updater::compareVersions("2.0.0-beta", "2.0.0"), 0);
        QCOMPARE(Updater::compareVersions("1.0", "0.99.99"), 1);

        const QByteArray release = js("{'tag_name':'v0.3.1','html_url':'https://h/r','body':'notes','draft':false,'prerelease':false,"
                                      "'assets':[{'name':'Relay-0.3.1-windows.zip','url':'u1','size':1},"
                                      "{'name':'Relay-0.3.1-setup.exe','url':'u2','size':42,'digest':'sha256:ABCD'}]}");
        QVariantMap r = Updater::parseRelease(release, "windows");
        QCOMPARE(r.value("version").toString(), QString("0.3.1"));
        QCOMPARE(r.value("assetUrl").toString(), QString("u2"));
        QCOMPARE(r.value("assetSize").toLongLong(), 42);
        QCOMPARE(r.value("sha256").toString(), QString("abcd"));
        QCOMPARE(r.value("page").toString(), QString("https://h/r"));
        // No installer for Linux: the version (and page) without a download.
        r = Updater::parseRelease(release, "linux");
        QCOMPARE(r.value("version").toString(), QString("0.3.1"));
        QVERIFY(!r.contains("assetUrl"));
        // Drafts, pre-releases and tags that aren't versions are ignored.
        QVERIFY(Updater::parseRelease(js("{'tag_name':'v9','draft':true}"), "windows").isEmpty());
        QVERIFY(Updater::parseRelease(js("{'tag_name':'v9','prerelease':true}"), "windows").isEmpty());
        QVERIFY(Updater::parseRelease(js("{'tag_name':'nightly'}"), "windows").isEmpty());
    }

    void update_check_and_download()
    {
        QSettings().remove("updates");
        GithubServer gh;
        gh.requireToken = "tok123";
        qputenv("RELAY_UPDATE_URL", gh.base().toUtf8());
        qputenv("RELAY_UPDATE_REPO", "me/relay");
        qputenv("RELAY_UPDATE_NO_INSTALL", "1");
        const QString version = QCoreApplication::applicationVersion();
        QCoreApplication::setApplicationVersion("0.8.2");

        Updater up;
        QCOMPARE(up.repository(), QString("me/relay"));
        // Private repository without a token: explained, nothing offered.
        up.check(true);
        QTRY_COMPARE(up.state(), QString("error"));
        QVERIFY(up.status().contains("token"));
        QVERIFY(up.needsToken());
        QVERIFY(!up.available());

        up.setToken("tok123");   // checks again
        QVERIFY(up.hasToken());
        QTRY_VERIFY(up.available());
        QVERIFY(!up.needsToken());
        QCOMPARE(up.latestVersion(), QString("0.9.0"));
        QVERIFY(up.offered());
        QCOMPARE(up.notes(), QString("Better things"));

        // Later: still available, but not offered again for this version.
        up.later();
        QVERIFY(up.available());
        QVERIFY(!up.offered());
        QSettings().remove("updates/skipped");

        // Download (with the checksum GitHub lists), on any system in the test.
        gh.digest = "sha256:" + QCryptographicHash::hash(gh.file, QCryptographicHash::Sha256).toHex();
        QVariantMap rel = Updater::parseRelease([&] {
            // The same release, for Windows, so there's an installer to fetch.
            QNetworkAccessManager nam; QNetworkRequest req(QUrl(gh.base() + "/repos/me/relay/releases/latest"));
            req.setRawHeader("Authorization", "Bearer tok123");
            QNetworkReply *r = nam.get(req);
            QSignalSpy done(r, &QNetworkReply::finished);
            done.wait(5000);
            const QByteArray b = r->readAll(); r->deleteLater(); return b;
        }(), "windows");
        QVERIFY(rel.contains("sha256"));
        QSignalSpy got(&up, &Updater::downloaded);
        up.setReleaseForTest(rel);
        up.updateNow();
        QTRY_COMPARE_WITH_TIMEOUT(got.count(), 1, 10000);
        QCOMPARE(up.state(), QString("installing"));
        const QString path = got.first().first().toString();
        {
            QFile f(path);   // closed again: Windows can't delete an open file
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), gh.file);
        }
        // The token went to the API, but not to the other host that had the file.
        bool assetWithToken = false, fileWithoutToken = false;
        for (const QByteArray &r : gh.requests) {
            if (r == "/repos/me/relay/releases/assets/7|Bearer tok123") assetWithToken = true;
            if (r == "/download/setup.exe|") fileWithoutToken = true;
        }
        QVERIFY2(assetWithToken && fileWithoutToken, gh.requests.join(", "));

        // A file that doesn't match the checksum isn't run.
        Updater up2;
        rel.insert("sha256", QString(64, '0'));
        up2.setReleaseForTest(rel);
        QSignalSpy got2(&up2, &Updater::downloaded);
        up2.updateNow();
        QTRY_COMPARE_WITH_TIMEOUT(up2.state(), QString("error"), 10000);
        QVERIFY(up2.status().contains("checksum"));
        QCOMPARE(got2.count(), 0);
        QVERIFY(!QFile::exists(path));

        up.setToken("");
        QVERIFY(!up.hasToken());
        QCoreApplication::setApplicationVersion(version);
        qunsetenv("RELAY_UPDATE_URL"); qunsetenv("RELAY_UPDATE_REPO"); qunsetenv("RELAY_UPDATE_NO_INSTALL");
        QSettings().remove("updates");
    }

    void trakt_watched()
    {
        const QJsonArray movies = QJsonDocument::fromJson(js("[{'plays':2,'movie':{'title':'Arrival','year':2016,'ids':{'trakt':1}}},"
                                                             "{'movie':{'title':''}}]")).array();
        const QJsonArray shows = QJsonDocument::fromJson(js("[{'show':{'title':'Harbor Lights','year':2022},'seasons':["
                                                            "{'number':1,'episodes':[{'number':1},{'number':2}]},{'number':2,'episodes':[{'number':5}]}]}]")).array();
        const QVariantMap w = TraktService::parseWatched(movies, shows);
        QCOMPARE(w.value("movies").toList().size(), 1);
        QCOMPARE(w.value("movies").toList()[0].toMap().value("year").toInt(), 2016);
        const QVariantList eps = w.value("episodes").toList();
        QCOMPARE(eps.size(), 3);
        QCOMPARE(eps[2].toMap().value("season").toInt(), 2);
        QCOMPARE(eps[2].toMap().value("episode").toInt(), 5);
        QCOMPARE(TraktService::titleKey(QStringLiteral("|EN| Arrival 2016 FHD")), QStringLiteral("arrival"));
        QCOMPARE(VodService::matchKey(QStringLiteral("EN - Harbor Lights [4K]")), QStringLiteral("harborlights"));
    }

    void trakt_sign_in_without_secret()
    {
        // Trakt apps made since its 2026 changes have no Client Secret:
        // signing in, refreshing and signing out send none, and no redirect
        // address. An older app's secret still goes along.
        TraktServer trakt;
        qputenv("RELAY_TRAKT_URL", trakt.base().toUtf8());
        QSettings().remove("trakt");
        {
            TraktService t;
            QVERIFY(!t.configured());
            t.signIn(QStringLiteral("client-1"), QString());
            QVERIFY(t.configured());
            QVERIFY(!t.hasSecret());
            QTRY_VERIFY(t.connected());
            QCOMPARE(trakt.last("/oauth/device/code").value("client_id").toString(), QStringLiteral("client-1"));
            const QJsonObject device = trakt.last("/oauth/device/token");
            QCOMPARE(device.value("code").toString(), QStringLiteral("dev1"));
            QCOMPARE(device.value("client_id").toString(), QStringLiteral("client-1"));
            QVERIFY(!device.contains("client_secret"));

            // The first call after signing in is turned away: Relay refreshes once and carries on.
            QTRY_COMPARE(t.user(), QStringLiteral("viewer"));
            const QJsonObject refresh = trakt.last("/oauth/token");
            QCOMPARE(refresh.value("grant_type").toString(), QStringLiteral("refresh_token"));
            QCOMPARE(refresh.value("refresh_token").toString(), QStringLiteral("r1"));
            QCOMPARE(refresh.value("client_id").toString(), QStringLiteral("client-1"));
            QVERIFY(!refresh.contains("client_secret"));
            QVERIFY(!refresh.contains("redirect_uri"));

            t.signOut();
            QVERIFY(!t.connected());
            QTRY_VERIFY(trakt.asked("/oauth/revoke"));
            QCOMPARE(trakt.last("/oauth/revoke").value("token").toString(), QStringLiteral("a2"));   // the refreshed one
            QVERIFY(!trakt.last("/oauth/revoke").contains("client_secret"));

            // An older app with a secret: it's sent. Another app's Client ID drops it.
            t.signIn(QStringLiteral("client-2"), QStringLiteral("older-secret"));
            QVERIFY(t.hasSecret());
            QTRY_VERIFY(t.connected());
            QCOMPARE(trakt.last("/oauth/device/token").value("client_secret").toString(), QStringLiteral("older-secret"));
            t.signOut();
            t.signIn(QStringLiteral("client-3"), QString());
            QVERIFY(!t.hasSecret());
            QTRY_VERIFY(t.connected());
            QCOMPARE(trakt.last("/oauth/device/token").value("client_id").toString(), QStringLiteral("client-3"));
            QVERIFY(!trakt.last("/oauth/device/token").contains("client_secret"));
            t.signOut();
        }
        QSettings().remove("trakt");
        qunsetenv("RELAY_TRAKT_URL");
    }

    // Trakt hears "start" when a movie starts and "stop" once when it's left
    // or finished, however often it's asked: a stop of 80% or more adds a play.
    void trakt_scrobbles_once()
    {
        TraktServer trakt;
        trakt.turnAwayOnce = false;
        qputenv("RELAY_TRAKT_URL", trakt.base().toUtf8());
        QSettings().remove("trakt");
        {
            TraktService t;
            t.signIn(QStringLiteral("client-1"), QString());
            QTRY_VERIFY(t.connected());
            const QVariantMap movie{{"kind", "movie"}, {"title", "Harbor Lights"}, {"year", "2024"}, {"key", "m1"}};
            auto count = [&](const QByteArray &path) { int n = 0; for (const auto &r : trakt.requests) n += r.first == path; return n; };
            t.scrobble(QStringLiteral("stop"), movie, 40);    // never started: nothing
            t.scrobble(QStringLiteral("start"), movie, 41);
            QTRY_COMPARE(count("/scrobble/start"), 1);
            t.scrobble(QStringLiteral("stop"), movie, 100);   // it finished
            t.scrobble(QStringLiteral("stop"), movie, 100);   // and was left
            t.scrobble(QStringLiteral("stop"), movie, 90);
            QTRY_COMPARE(count("/scrobble/stop"), 1);
            QTest::qWait(300);
            QCOMPARE(count("/scrobble/stop"), 1);
            QCOMPARE(trakt.last("/scrobble/stop").value("progress").toDouble(), 100.0);
            t.scrobble(QStringLiteral("start"), movie, 0);    // watched again
            t.scrobble(QStringLiteral("stop"), movie, 95);
            QTRY_COMPARE(count("/scrobble/stop"), 2);
            t.signOut();
        }
        QSettings().remove("trakt");
        qunsetenv("RELAY_TRAKT_URL");
    }

    void series_recording()
    {
        QTemporaryDir dir;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 base = now - now % 3600 + 7200;   // two hours from the top of this hour
        auto prog = [](qint64 start, const char *title, const char *episode) {
            auto stamp = [](qint64 t) { return QDateTime::fromSecsSinceEpoch(t).toUTC().toString(QStringLiteral("yyyyMMddHHmmss")); };
            return QStringLiteral("<programme channel=\"one\" start=\"%1 +0000\" stop=\"%2 +0000\"><title>%3</title>%4</programme>\n")
                .arg(stamp(start), stamp(start + 3600), QLatin1String(title),
                     *episode ? QStringLiteral("<episode-num system=\"onscreen\">%1</episode-num>").arg(QLatin1String(episode)) : QString());
        };
        const QString guide = dir.filePath(QStringLiteral("guide.xml"));
        {
            QFile f(guide);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(("<?xml version=\"1.0\"?><tv><channel id=\"one\"><display-name>One</display-name></channel>"
                     + prog(base, "Harbor Lights", "S1 E1") + prog(base + 3600, "Other Show", "")
                     + prog(base + 7200, "HARBOR LIGHTS", "S1 E2") + prog(base + 86400, "Harbor Lights", "S1 E1")   // a repeat
                     + prog(base + 90000, "Harbor Lights", "S1 E3") + "</tv>").toUtf8());
        }
        ChannelModel model;
        model.setPlaylist(M3uParser::parse(
            "#EXTM3U\n#EXTINF:-1 tvg-id=\"one\" group-title=\"A\",One\nhttp://127.0.0.1:1/one.ts\n"
            "#EXTINF:-1 group-title=\"A\",Two\nhttp://127.0.0.1:1/two.ts\n"));
        EpgService epg(dir.filePath(QStringLiteral("guide.sqlite")));
        QSignalSpy updated(&epg, &EpgService::guideUpdated);
        epg.refresh(model.originalChannels(), {guide}, true);
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), 1, 10000);
        model.setEpgIds(epg.resolve(model.originalChannels()));

        QString firstId;
        {
            ScheduleService schedule(&model, &epg, dir.path(), dir.filePath("R"), QStringLiteral("p1"));
            const QVariantMap show{{"title", QStringLiteral("Harbor Lights")}, {"start", double(base) * 1000}, {"stop", double(base + 3600) * 1000}};
            QCOMPARE(schedule.seriesFor(0, QStringLiteral("harbor lights")), QString());
            const QString id = schedule.addSeries(0, show);
            QVERIFY(!id.isEmpty());
            QCOMPARE(schedule.addSeries(0, show), id);                         // once per show and channel
            QCOMPARE(schedule.seriesFor(0, QStringLiteral("harbor lights")), id);
            QCOMPARE(schedule.seriesFor(1, QStringLiteral("Harbor Lights")), QString());
            // Three episodes; the repeat of S1 E1 and the other show are left out.
            QCOMPARE(schedule.items().size(), 3);
            QCOMPARE(schedule.series().at(0).toMap().value("upcoming").toInt(), 3);
            QCOMPARE(qint64(schedule.series().at(0).toMap().value("next").toDouble() / 1000), base);
            QCOMPARE(schedule.markFor(0, double(base + 7200) * 1000), QStringLiteral("recording"));
            QCOMPARE(schedule.markFor(0, double(base + 86400) * 1000), QString());
            QVERIFY(schedule.items().last().toMap().value("title").toString().contains(QStringLiteral("S1 E1")));
            QVERIFY(schedule.items().last().toMap().value("series").toBool());

            // Cancelling one airing sticks; the rule doesn't put it back.
            firstId = schedule.idFor(0, double(base) * 1000);
            schedule.remove(firstId, false);
            schedule.applySeries();
            QCOMPARE(schedule.items().size(), 3);                               // the repeat takes the cancelled episode's place
            QCOMPARE(schedule.markFor(0, double(base) * 1000), QString());
            QCOMPARE(schedule.markFor(0, double(base + 86400) * 1000), QStringLiteral("recording"));
            schedule.applySeries();
            QCOMPARE(schedule.items().size(), 3);
        }
        {
            // Rules and their recordings survive a restart.
            ScheduleService schedule(&model, &epg, dir.path(), dir.filePath("R"), QStringLiteral("p1"));
            QCOMPARE(schedule.series().size(), 1);
            QCOMPARE(schedule.items().size(), 3);
            schedule.applySeries();
            QCOMPARE(schedule.items().size(), 3);
            // A one-off recording isn't touched when the rule goes.
            QVERIFY(!schedule.addRecording(0, {{"title", QStringLiteral("Other Show")}, {"start", double(base + 3600) * 1000},
                                               {"stop", double(base + 7200) * 1000}}).isEmpty());
            schedule.removeSeries(schedule.series().at(0).toMap().value("id").toString());
            QCOMPARE(schedule.series().size(), 0);
            QCOMPARE(schedule.items().size(), 1);
            QCOMPARE(schedule.items().at(0).toMap().value("title").toString(), QStringLiteral("Other Show"));
            QCOMPARE(schedule.seriesFor(0, QStringLiteral("Harbor Lights")), QString());
        }
        {
            // Another playlist is open: its rules wait.
            ScheduleService schedule(&model, &epg, dir.path(), dir.filePath("R"), QStringLiteral("p2"));
            QVERIFY(!schedule.addSeries(0, {{"title", QStringLiteral("Harbor Lights")}}).isEmpty());
            QCOMPARE(schedule.series().size(), 1);
            schedule.setPlaylist(QStringLiteral("p1"));
            schedule.applySeries();
            QCOMPARE(schedule.seriesFor(0, QStringLiteral("Harbor Lights")), QString());
            QCOMPARE(schedule.items().size(), 4);   // p2's three + the one-off
        }
    }

    void changed_keys_are_saved()
    {
        QSettings().clear();
        {
            AppSettings settings;
            QSignalSpy changed(&settings, &AppSettings::keysChanged);
            QVERIFY(settings.keys().isEmpty());
            settings.setKey(QStringLiteral("guide"), Qt::Key_T);
            settings.setKey(QStringLiteral("mute"), Qt::Key_F2);
            QCOMPARE(changed.count(), 2);
            settings.setUiScale(130);
            settings.setUiScale(500);
            QCOMPARE(settings.uiScale(), 200);
            settings.setExternalPlayer(QStringLiteral("file:///opt/vlc/vlc"));
            QCOMPARE(settings.externalPlayer(), QStringLiteral("/opt/vlc/vlc"));
        }
        AppSettings settings;
        QCOMPARE(settings.keys().value("guide").toInt(), int(Qt::Key_T));
        QCOMPARE(settings.keys().size(), 2);
        settings.setKey(QStringLiteral("guide"), 0);
        QCOMPARE(settings.keys().size(), 1);
        settings.resetKeys();
        QVERIFY(settings.keys().isEmpty());
        QCOMPARE(settings.uiScale(), 200);
        QSettings().clear();
    }

    void guide_addresses_split()
    {
        QCOMPARE(AppSettings::splitAddresses(QStringLiteral("http://a/x.xml.gz, http://b/y.xml")),
                 QStringList({QStringLiteral("http://a/x.xml.gz"), QStringLiteral("http://b/y.xml")}));
        // A comma inside an address stays.
        QCOMPARE(AppSettings::splitAddresses(QStringLiteral("http://a/epg?ids=1,2;http://b/e")),
                 QStringList({QStringLiteral("http://a/epg?ids=1,2"), QStringLiteral("http://b/e")}));
        QCOMPARE(AppSettings::splitAddresses(QStringLiteral("  \nhttp://a\n\nC:\\guide.xml\n")),
                 QStringList({QStringLiteral("http://a"), QStringLiteral("C:\\guide.xml")}));
    }

private:
    static QByteArray sample()
    {
        return "#EXTM3U\n"
               "#EXTINF:-1 group-title=\"News\",News One\nhttp://s/1\n"
               "#EXTINF:-1 group-title=\"Sports\",Sports One\nhttp://s/2\n"
               "#EXTINF:-1 group-title=\"News\",News Two\nhttp://s/3\n"
               "#EXTINF:-1 group-title=\"Sports\",Sports Two\nhttp://s/4\n";
    }


};

QTEST_GUILESS_MAIN(TestSources)
#include "test_sources.moc"
