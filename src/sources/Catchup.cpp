#include "Catchup.h"

#include "XtreamParser.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QUrl>

namespace {

QString pad(qint64 v, int width = 2) { return QStringLiteral("%1").arg(v, width, 10, QLatin1Char('0')); }

// Y m d H M S letters -> parts of a UTC time.
QString formatUtc(qint64 t, const QString &fmt)
{
    const QDateTime d = QDateTime::fromSecsSinceEpoch(t, QTimeZone::utc());
    QString out;
    for (const QChar ch : fmt) {
        switch (ch.unicode()) {
        case 'Y': out += pad(d.date().year(), 4); break;
        case 'm': out += pad(d.date().month()); break;
        case 'd': out += pad(d.date().day()); break;
        case 'H': out += pad(d.time().hour()); break;
        case 'M': out += pad(d.time().minute()); break;
        case 'S': out += pad(d.time().second()); break;
        default: out += ch;
        }
    }
    return out;
}

QString enc(const QString &s) { return QString::fromUtf8(QUrl::toPercentEncoding(s)); }

} // namespace

namespace Catchup {

QString streamIdFromUrl(const QString &url)
{
    // .../live/user/pass/123.ts, .../user/pass/123, .../live/user/pass/123.m3u8
    static const QRegularExpression re(QStringLiteral("/(\\d+)(?:\\.[A-Za-z0-9]{2,4})?(?:\\?.*)?$"));
    const auto m = re.match(url);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString serverTime(qint64 utc, const Account &a)
{
    QDateTime t = a.zone.isValid()
        ? QDateTime::fromSecsSinceEpoch(utc, a.zone)
        : QDateTime::fromSecsSinceEpoch(utc + a.offsetSecs, QTimeZone::utc());
    return t.toString(QStringLiteral("yyyy-MM-dd:HH-mm"));
}

QString expand(const QString &tmpl, qint64 start, qint64 stop, qint64 now)
{
    const qint64 duration = qMax<qint64>(0, stop - start);
    // {name}, {name:FORMAT}, ${name}, ${name:FORMAT}
    static const QRegularExpression re(QStringLiteral("\\$?\\{([a-z_]+|[YmdHMS])(?::([^}]*))?\\}"));
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(tmpl);
    while (it.hasNext()) {
        const auto m = it.next();
        out += tmpl.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString name = m.captured(1);
        const QString fmt = m.captured(2);
        QString v;
        auto time = [&](qint64 t) {
            if (fmt.isEmpty()) return QString::number(t);
            return formatUtc(t, fmt);
        };
        if (name == QLatin1String("utc") || name == QLatin1String("start") || name == QLatin1String("utcstart"))
            v = time(start);
        else if (name == QLatin1String("utcend") || name == QLatin1String("end"))
            v = time(stop);
        else if (name == QLatin1String("lutc") || name == QLatin1String("now") || name == QLatin1String("timestamp"))
            v = time(now);
        else if (name == QLatin1String("duration"))
            v = QString::number(fmt.toInt() > 0 ? duration / fmt.toInt() : duration);
        else if (name == QLatin1String("offset"))
            v = QString::number(fmt.toInt() > 0 ? (now - start) / fmt.toInt() : now - start);
        else if (name.size() == 1)
            v = formatUtc(start, name);
        else
            v = m.captured(0);   // unknown: leave it
        out += v;
    }
    out += tmpl.mid(last);
    return out;
}

bool supported(const Channel &c, const Account &account)
{
    const QString t = c.catchupType.toLower();
    if (c.catchupDays <= 0) return false;
    if (t == QLatin1String("portal")) return c.url.startsWith(QLatin1String("portal://"));
    if (t == QLatin1String("xc")) return account.valid() && !streamIdFromUrl(c.url).isEmpty();
    if (t == QLatin1String("default") || t == QLatin1String("append"))
        return !c.catchupSource.isEmpty() || (account.valid() && !streamIdFromUrl(c.url).isEmpty());
    return t == QLatin1String("shift") || t == QLatin1String("timeshift")
        || t == QLatin1String("flussonic") || t == QLatin1String("flussonic-hls")
        || t == QLatin1String("flussonic-ts") || t == QLatin1String("fs");
}

QString url(const Channel &c, qint64 start, qint64 stop, qint64 now, const Account &account, int *startAdjust)
{
    if (startAdjust) *startAdjust = 0;
    if (!supported(c, account)) return QString();
    const QString t = c.catchupType.toLower();
    stop = qMax(stop, start + 60);

    auto xtream = [&]() {
        // Minute precision: start at the minute and skip the extra seconds.
        const qint64 minute = start - start % 60;
        if (startAdjust) *startAdjust = int(start - minute);
        const qint64 minutes = (stop - minute + 59) / 60;
        return QStringLiteral("%1/timeshift/%2/%3/%4/%5/%6.ts")
            .arg(XtreamParser::normalizeServer(account.server).toString(), enc(account.username),
                 enc(account.password)).arg(minutes)
            .arg(serverTime(minute, account), streamIdFromUrl(c.url));
    };

    if (t == QLatin1String("portal")) {
        // Resolved when played (PortalClient): the portal plays archives by programme.
        return QStringLiteral("portal://%1/archive%2?from=%3&stop=%4")
            .arg(QUrl(c.url).host(), QUrl(c.url).path()).arg(start).arg(stop);
    }
    if (t == QLatin1String("xc")) return xtream();
    if (t == QLatin1String("default"))
        return c.catchupSource.isEmpty() ? xtream() : expand(c.catchupSource, start, stop, now);
    if (t == QLatin1String("append")) {
        if (c.catchupSource.isEmpty()) return xtream();
        return c.url + expand(c.catchupSource, start, stop, now);
    }
    if (t == QLatin1String("shift") || t == QLatin1String("timeshift")) {
        const QChar sep = c.url.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?');
        return c.url + sep + QStringLiteral("utc=%1&lutc=%2").arg(start).arg(now);
    }
    // Flussonic: .../index.m3u8 -> .../index-START-DURATION.m3u8,
    //            .../mpegts      -> .../timeshift_abs-START.ts
    static const QRegularExpression hls(QStringLiteral("^(.*)/([A-Za-z0-9_]+?)(?:-[A-Za-z0-9_]+)?\\.m3u8(\\?.*)?$"));
    static const QRegularExpression ts(QStringLiteral("^(.*)/mpegts(\\?.*)?$"));
    auto m = hls.match(c.url);
    if (m.hasMatch())
        return QStringLiteral("%1/%2-%3-%4.m3u8%5").arg(m.captured(1), m.captured(2))
            .arg(start).arg(stop - start).arg(m.captured(3));
    m = ts.match(c.url);
    if (m.hasMatch())
        return QStringLiteral("%1/timeshift_abs-%2.ts%3").arg(m.captured(1)).arg(start).arg(m.captured(2));
    return QString();
}

} // namespace Catchup
