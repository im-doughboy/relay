#include "HlsPlaylist.h"

#include <QRegularExpression>

#include <algorithm>

namespace Hls {

bool looksLikeHls(const QUrl &url)
{
    return url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive)
        || url.path().endsWith(QLatin1String(".m3u"), Qt::CaseInsensitive);
}

Playlist parse(const QByteArray &data, const QUrl &base)
{
    Playlist p;
    QString text = QString::fromUtf8(data);
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty() || !lines.first().trimmed().startsWith(QLatin1String("#EXTM3U"))) return p;
    p.valid = true;

    static const QRegularExpression bandwidth(QStringLiteral("BANDWIDTH=(\\d+)"));
    QVector<QPair<qint64, QUrl>> variants;
    qint64 pendingBandwidth = -1;
    double pendingDuration = -1;
    qint64 seq = 0;
    bool seqSet = false;

    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF"))) {
            const auto m = bandwidth.match(line);
            pendingBandwidth = m.hasMatch() ? m.captured(1).toLongLong() : 0;
        } else if (line.startsWith(QLatin1String("#EXT-X-TARGETDURATION:"))) {
            p.targetDuration = line.mid(22).toDouble();
        } else if (line.startsWith(QLatin1String("#EXT-X-MEDIA-SEQUENCE:"))) {
            seq = line.mid(22).toLongLong();
            p.mediaSequence = seq;
            seqSet = true;
        } else if (line.startsWith(QLatin1String("#EXTINF:"))) {
            pendingDuration = line.mid(8).section(QLatin1Char(','), 0, 0).toDouble();
        } else if (line.startsWith(QLatin1String("#EXT-X-ENDLIST"))) {
            p.endList = true;
        } else if (line.startsWith(QLatin1String("#EXT-X-KEY"))) {
            if (!line.contains(QLatin1String("METHOD=NONE"))) p.encrypted = true;
        } else if (line.startsWith(QLatin1String("#EXT-X-MAP"))) {
            p.fragmentedMp4 = true;
        } else if (!line.startsWith(QLatin1Char('#'))) {
            const QUrl url = base.resolved(QUrl(line));
            if (pendingBandwidth >= 0) {
                variants.append({pendingBandwidth, url});
                pendingBandwidth = -1;
            } else if (pendingDuration >= 0) {
                p.segments.append({url, pendingDuration, seq++});
                pendingDuration = -1;
            }
        }
    }
    Q_UNUSED(seqSet)
    if (!variants.isEmpty()) {
        p.master = true;
        std::stable_sort(variants.begin(), variants.end(),
                         [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &v : variants) p.variants.append(v.second);
    }
    if (p.targetDuration <= 0) p.targetDuration = 6;
    return p;
}

} // namespace Hls
