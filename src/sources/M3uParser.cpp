#include "M3uParser.h"

#include <QStringView>
#include <QUrl>

#include <algorithm>
#include <array>

namespace {

QString fallbackNameFromUrl(const QString &url)
{
    const QString path = QUrl(url).path();
    const QString file = path.section(QLatin1Char('/'), -1);
    return file.isEmpty() ? url : file;
}

} // namespace

M3uParser::Kind M3uParser::classify(const QString &url)
{
    const QString path = QUrl(url).path().toLower();
    if (path.contains(QLatin1String("/movie/"))) return Kind::Movie;
    if (path.contains(QLatin1String("/series/"))) return Kind::Series;
    static const std::array<QLatin1String, 8> videoFiles = {
        QLatin1String(".mp4"), QLatin1String(".mkv"), QLatin1String(".avi"),
        QLatin1String(".mov"), QLatin1String(".wmv"), QLatin1String(".m4v"),
        QLatin1String(".webm"), QLatin1String(".flv"),
    };
    for (const QLatin1String &ext : videoFiles)
        if (path.endsWith(ext)) return Kind::Movie;
    return Kind::Live;
}

QHash<QString, QString> M3uParser::parseAttributes(const QString &body, QString *displayName)
{
    QHash<QString, QString> attrs;
    const int n = body.size();
    int i = 0;

    // Skip the duration token ("-1", "0", "123.4") if present.
    while (i < n && body[i].isSpace()) ++i;
    if (i < n && (body[i] == QLatin1Char('-') || body[i].isDigit())) {
        while (i < n && !body[i].isSpace() && body[i] != QLatin1Char(',')) ++i;
    }

    while (i < n) {
        while (i < n && body[i].isSpace()) ++i;
        if (i >= n) break;

        if (body[i] == QLatin1Char(',')) {           // start of display name
            if (displayName) *displayName = body.mid(i + 1).trimmed();
            return attrs;
        }

        // Read key.
        const int keyStart = i;
        while (i < n && body[i] != QLatin1Char('=') && !body[i].isSpace()
               && body[i] != QLatin1Char(','))
            ++i;
        const QString key = body.mid(keyStart, i - keyStart).toLower();

        if (i < n && body[i] == QLatin1Char('=')) {
            ++i;
            QString value;
            if (i < n && (body[i] == QLatin1Char('"') || body[i] == QLatin1Char('\''))) {
                const QChar quote = body[i++];
                const int valStart = i;
                while (i < n && body[i] != quote) ++i;
                value = body.mid(valStart, i - valStart);
                if (i < n) ++i;                       // closing quote
            } else {
                const int valStart = i;
                while (i < n && !body[i].isSpace() && body[i] != QLatin1Char(',')) ++i;
                value = body.mid(valStart, i - valStart);
            }
            if (!key.isEmpty()) attrs.insert(key, value.trimmed());
        } else if (!key.isEmpty()) {
            attrs.insert(key, QString());             // bare flag
        }
    }

    if (displayName) displayName->clear();
    return attrs;
}

PlaylistResult M3uParser::parse(const QByteArray &data, bool keepVod)
{
    PlaylistResult result;

    // Line by line from the downloaded bytes: converting the whole playlist to
    // one string and then a list of lines needed five or six times its size at
    // once, which a TV's memory couldn't hold for a large provider playlist.
    QByteArrayView rest(data);
    if (rest.startsWith("\xEF\xBB\xBF")) rest = rest.sliced(3);   // UTF-8 BOM

    Channel pending;
    bool hasPending = false;
    QString pendingGroup;     // from #EXTGRP
    QString pendingUa;        // from #EXTVLCOPT
    QString pendingRef;
    bool sawHeader = false;

    auto resetPending = [&] {
        pending = Channel();
        hasPending = false;
        pendingGroup.clear();
        pendingUa.clear();
        pendingRef.clear();
    };

    while (!rest.isEmpty()) {
        const qsizetype end = rest.indexOf('\n');
        const QByteArrayView raw = end < 0 ? rest : rest.first(end);
        rest = end < 0 ? QByteArrayView() : rest.sliced(end + 1);
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty()) continue;

        if (line.startsWith(QLatin1String("#EXTM3U"), Qt::CaseInsensitive)) {
            sawHeader = true;
            const auto attrs = parseAttributes(line.mid(7), nullptr);
            result.epgUrl = attrs.value(QStringLiteral("url-tvg"),
                                        attrs.value(QStringLiteral("x-tvg-url")));
            continue;
        }

        if (line.startsWith(QLatin1String("#EXTINF:"), Qt::CaseInsensitive)) {
            resetPending();
            QString name;
            const auto attrs = parseAttributes(line.mid(8), &name);

            pending.name = name.isEmpty() ? attrs.value(QStringLiteral("tvg-name")) : name;
            pending.tvgId = attrs.value(QStringLiteral("tvg-id"));
            pending.tvgShiftMinutes = qBound(-720, qRound(attrs.value(QStringLiteral("tvg-shift")).toDouble() * 60), 720);
            pending.logo = attrs.value(QStringLiteral("tvg-logo"));
            pending.group = attrs.value(QStringLiteral("group-title"));
            pending.number = attrs.value(QStringLiteral("tvg-chno")).toInt();
            pending.userAgent = attrs.value(QStringLiteral("http-user-agent"),
                                            attrs.value(QStringLiteral("user-agent")));
            pending.referrer = attrs.value(QStringLiteral("http-referrer"));
            pending.catchupType = attrs.value(QStringLiteral("catchup"));
            pending.catchupSource = attrs.value(QStringLiteral("catchup-source"));
            pending.catchupDays = attrs.value(QStringLiteral("catchup-days"),
                                              attrs.value(QStringLiteral("timeshift"))).toInt();
            hasPending = true;
            continue;
        }

        if (line.startsWith(QLatin1String("#EXTGRP:"), Qt::CaseInsensitive)) {
            pendingGroup = line.mid(8).trimmed();
            continue;
        }

        if (line.startsWith(QLatin1String("#EXTVLCOPT:"), Qt::CaseInsensitive)) {
            const QString opt = line.mid(11).trimmed();
            const int eq = opt.indexOf(QLatin1Char('='));
            if (eq > 0) {
                const QString k = opt.left(eq).trimmed().toLower();
                const QString v = opt.mid(eq + 1).trimmed();
                if (k == QLatin1String("http-user-agent")) pendingUa = v;
                else if (k == QLatin1String("http-referrer") || k == QLatin1String("http-referer"))
                    pendingRef = v;
            }
            continue;
        }

        if (line.startsWith(QLatin1Char('#'))) continue;   // other directives

        // A URL line. Without a preceding #EXTINF, only accept things that look
        // like URLs, so an HTML error page isn't mistaken for a playlist.
        if (!hasPending && !line.contains(QLatin1String("://"))) continue;

        if (!keepVod) {   // movies and episodes only counted
            const Kind kind = classify(line);
            if (kind != Kind::Live) {
                ++(kind == Kind::Movie ? result.skippedMovies : result.skippedSeries);
                resetPending();
                continue;
            }
        }

        Channel ch = hasPending ? pending : Channel();
        ch.url = line;
        if (ch.name.isEmpty()) ch.name = fallbackNameFromUrl(line);
        if (ch.group.isEmpty()) ch.group = pendingGroup;
        if (ch.userAgent.isEmpty()) ch.userAgent = pendingUa;
        if (ch.referrer.isEmpty()) ch.referrer = pendingRef;
        if (ch.group.isEmpty()) ch.group = QStringLiteral("Uncategorized");
        result.channels.push_back(ch);
        resetPending();
    }

    // Keep live TV only. If the whole playlist is on-demand video (say, a
    // list of the user's own files), keep it all rather than show nothing.
    // (Entries move rather than copy: a large playlist holds 100,000 of them.)
    const bool anyLive = std::any_of(result.channels.cbegin(), result.channels.cend(),
                                     [](const Channel &c) { return classify(c.url) == Kind::Live; });
    if (anyLive) {
        QVector<Channel> live, movies, series;
        for (Channel &c : result.channels) {
            switch (classify(c.url)) {
            case Kind::Live: live.push_back(std::move(c)); break;
            case Kind::Movie: movies.push_back(std::move(c)); break;
            case Kind::Series: series.push_back(std::move(c)); break;
            }
        }
        result.channels = std::move(live);
        result.skippedMovies += movies.size();   // plus any only counted (keepVod off)
        result.skippedSeries += series.size();
        result.vodMovies = std::move(movies);
        result.vodEpisodes = std::move(series);
    }

    // Number channels that had no tvg-chno by playlist position.
    for (int i = 0; i < result.channels.size(); ++i) {
        if (result.channels[i].number <= 0) result.channels[i].number = i + 1;
    }

    if (result.channels.isEmpty()) {
        result.error = sawHeader ? QStringLiteral("The playlist has no channels.")
                                 : QStringLiteral("This doesn't look like an M3U playlist.");
    }
    return result;
}
