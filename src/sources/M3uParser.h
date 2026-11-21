#pragma once

#include "Channel.h"

#include <QByteArray>
#include <QHash>

// Parses extended M3U / M3U8 IPTV playlists.
//
// Handles the messy real-world variants: BOM, CRLF line endings, quoted and
// unquoted attributes, commas inside quoted values, #EXTGRP group lines,
// #EXTVLCOPT user-agent / referrer options, and missing #EXTM3U headers.
class M3uParser
{
public:
    // Returns live channels only. Movie and series entries (common in
    // Xtream "m3u_plus" playlists) are counted in skippedMovies/skippedSeries
    // and left out, unless the playlist has nothing else.
    static PlaylistResult parse(const QByteArray &data);

    enum class Kind { Live, Movie, Series };
    // Xtream VOD URLs look like /movie/user/pass/123.mkv and
    // /series/user/pass/456.mp4; a plain video file is treated as a movie.
    static Kind classify(const QString &url);

    // Exposed for tests: parses `key="value" key2=value2 ...` and returns the
    // display name that follows the first comma outside quotes.
    static QHash<QString, QString> parseAttributes(const QString &extinfBody,
                                                   QString *displayName);
};
