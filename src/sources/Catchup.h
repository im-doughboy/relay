#pragma once

#include "Channel.h"

#include <QString>
#include <QTimeZone>

// Builds addresses for watching past shows ("catch-up" / archive). Pure
// functions, unit-tested.
//
// Supported:
//  - Xtream Codes archives ("xc"): /timeshift/user/pass/<minutes>/<YYYY-MM-DD:HH-MM>/<id>.ts,
//    with the start written in the server's own time zone.
//  - M3U catchup="default" / "append" with a catchup-source template
//    ({utc}, {utcend}, {lutc}, {duration}, {offset}, {Y}{m}{d}{H}{M}{S},
//    {utc:YmdHMS}, ${start}, ${end}, ${timestamp}, ${duration}, ...).
//  - catchup="shift" (?utc=<start>&lutc=<now>) and "flussonic" / "fs".
namespace Catchup {

// An Xtream account, for "xc" archives.
struct Account {
    QString server;     // http://host:port[/path]
    QString username;
    QString password;
    QTimeZone zone;     // server time zone, if the server named one
    int offsetSecs = 0; // otherwise: server clock minus UTC (from its time_now)
    bool valid() const { return !server.isEmpty() && !username.isEmpty(); }
};

// The Xtream stream id in a live URL (".../live/u/p/123.ts" or ".../u/p/123"),
// or "" if it doesn't look like one.
QString streamIdFromUrl(const QString &url);

// True if the channel's archive type is one we can build addresses for.
bool supported(const Channel &c, const Account &account);

// Address for the channel's archive from `start` (Unix seconds) to `stop`.
// `startAdjust` receives how many seconds into the returned stream `start`
// is (Xtream archives start on whole minutes). Returns "" if unsupported.
QString url(const Channel &c, qint64 start, qint64 stop, qint64 now,
            const Account &account, int *startAdjust = nullptr);

// Server's local time for `utc` (Unix seconds), formatted "yyyy-MM-dd:HH-mm".
QString serverTime(qint64 utc, const Account &account);

// Fills `{...}` / `${...}` placeholders in a catchup-source template.
QString expand(const QString &tmpl, qint64 start, qint64 stop, qint64 now);

} // namespace Catchup
