#pragma once

#include "Channel.h"

#include <QByteArray>
#include <QHash>
#include <QUrl>

// Pure helpers for the Xtream Codes player API. Networking lives in
// PlaylistService; everything here is synchronous and unit-testable.
class XtreamParser
{
public:
    struct Credentials {
        QString server;      // as typed by the user
        QString username;
        QString password;
        QString output = QStringLiteral("ts");   // "ts" or "m3u8"
    };

    struct AuthResult {
        bool ok = false;
        QString message;     // reason when !ok
        QString timezone;    // server_info.timezone, e.g. "Europe/London"
        QString timeNow;     // server_info.time_now, "yyyy-MM-dd HH:mm:ss" in that zone
    };

    // "example.com:8080/" -> "http://example.com:8080"
    static QUrl normalizeServer(const QString &server);

    // player_api.php URL, optionally with an action (get_live_streams, ...)
    static QUrl apiUrl(const Credentials &c, const QString &action = QString());

    // An M3U link from an Xtream-style server (".../get.php?username=..&password=..")
    // has a guide at ".../xmltv.php" with the same login, even when the playlist
    // doesn't say so. Returns "" for any other URL.
    static QString guideUrlForPlaylist(const QUrl &playlistUrl);

    // The login inside such a get.php link, so movies and series can be read
    // from the same server's API. False for any other URL.
    static bool credentialsFromPlaylistUrl(const QUrl &playlistUrl, Credentials *out);

    static AuthResult parseAuth(const QByteArray &json);
    // stream_id -> days of archive, for channels with tv_archive = 1.
    static QHash<QString, int> parseArchiveDays(const QByteArray &liveStreamsJson);

    static QHash<QString, QString> parseCategories(const QByteArray &json,
                                                   QStringList *orderedNames = nullptr);
    static PlaylistResult parseLiveStreams(const QByteArray &json,
                                           const QHash<QString, QString> &categories,
                                           const Credentials &c,
                                           const QStringList &groupOrder = {});
};
