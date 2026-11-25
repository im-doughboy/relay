#pragma once

#include <QString>
#include <QStringList>

// The languages Settings offers for audio and subtitles, and how to tell
// whether a track is in one. Providers label tracks loosely: a proper code
// ("eng", "en"), a name in English or in the language itself ("English",
// "Türkçe"), or only a title like "Spanish (Latino) 5.1".
namespace Languages {

// Codes Settings offers, in the order shown ("en", "es", ...).
QStringList codes();
// mpv's alang/slang list for a code: "en" -> "en,eng". Empty for "".
QString mpvList(const QString &code);
// Is a track with this language tag and title in `code`?
bool matches(const QString &code, const QString &trackLang, const QString &trackTitle);

}
