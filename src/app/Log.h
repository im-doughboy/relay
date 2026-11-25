#pragma once

#include <QString>

// Writes all qDebug/qInfo/qWarning output to <dataDir>/relay.log (the previous
// run is kept as relay.previous.log) as well as stderr. The Windows build has
// no console, so this file is how problems get diagnosed.
namespace Log {

void install(const QString &dataDir);
QString filePath();

// Hides logins in URLs: "password=" values become "***", "username=" (and
// user, login, mac) values "~", Xtream stream paths /live/~/***/..., and a
// login before the host (http://name:pass@host) "~@". Every log line passes
// through it.
QString redact(const QString &url);

} // namespace Log
