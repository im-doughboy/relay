#pragma once

#include <QString>

// Writes all qDebug/qInfo/qWarning output to <dataDir>/relay.log (the previous
// run is kept as relay.previous.log) as well as stderr. The Windows build has
// no console, so this file is how problems get diagnosed.
namespace Log {

void install(const QString &dataDir);
QString filePath();

// Hides passwords in URLs before logging: query "password=" values and the
// credentials in Xtream stream paths (/live/user/pass/...).
QString redact(const QString &url);

} // namespace Log
