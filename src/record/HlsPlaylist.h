#pragma once

#include <QByteArray>
#include <QStringList>
#include <QUrl>
#include <QVector>

// Minimal HLS (.m3u8) reader, enough to record live channels: picks the best
// variant of a master playlist and lists a media playlist's segments.
namespace Hls {

struct Segment {
    QUrl url;
    double duration = 0;
    qint64 sequence = 0;
};

struct Playlist {
    bool valid = false;            // starts with #EXTM3U
    bool master = false;
    QVector<QUrl> variants;        // master: best (highest bandwidth) first
    QVector<Segment> segments;
    double targetDuration = 6;
    qint64 mediaSequence = 0;
    bool endList = false;
    bool encrypted = false;        // #EXT-X-KEY other than METHOD=NONE
    bool fragmentedMp4 = false;    // #EXT-X-MAP (fMP4 segments)
};

Playlist parse(const QByteArray &data, const QUrl &base);
bool looksLikeHls(const QUrl &url);

} // namespace Hls
