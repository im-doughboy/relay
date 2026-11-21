#include "epg/EpgMatcher.h"
#include "epg/EpgService.h"
#include "epg/EpgStore.h"
#include "epg/XmltvParser.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <zlib.h>

namespace {

QByteArray gzip(const QByteArray &data)
{
    z_stream zs{};
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&zs, uLong(data.size()))) + 32, '\0');
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    zs.avail_in = uInt(data.size());
    zs.next_out = reinterpret_cast<Bytef *>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(int(zs.total_out));
    deflateEnd(&zs);
    return out;
}

QString xmltvTime(qint64 secs)
{
    return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::utc())
               .toString(QStringLiteral("yyyyMMddHHmmss")) + QStringLiteral(" +0000");
}

// A small guide around `now`: two channels, one matched by id, one by name.
QByteArray sampleGuide(qint64 now)
{
    const qint64 hour = 3600;
    const qint64 base = now - now % hour;   // top of the current hour
    QByteArray x =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE tv SYSTEM \"xmltv.dtd\">\n"
        "<tv generator-info-name=\"test\">\n"
        "  <channel id=\"news.us\"><display-name>Harbor News</display-name>"
        "<icon src=\"http://l/n.png\"/></channel>\n"
        "  <channel id=\"kids-42\"><display-name lang=\"en\">Cartoon Kingdom</display-name>"
        "<display-name>CK</display-name></channel>\n"
        "  <channel id=\"unused\"><display-name>Not In Playlist</display-name></channel>\n";
    auto prog = [&](const char *ch, qint64 s, qint64 e, const QString &inner) {
        x += QStringLiteral("  <programme start=\"%1\" %2channel=\"%3\">%4</programme>\n")
                 .arg(xmltvTime(s),
                      e ? QStringLiteral("stop=\"%1\" ").arg(xmltvTime(e)) : QString(),
                      QString::fromLatin1(ch), inner)
                 .toUtf8();
    };
    prog("news.us", base - hour, base, QStringLiteral("<title>Morning &amp; More</title>"));
    prog("news.us", base, base + hour,
         QStringLiteral("<title lang=\"fr\">Le Journal</title><title lang=\"en\">The Bulletin</title>"
                        "<sub-title>Late edition</sub-title><desc>Top stories.</desc>"
                        "<category>News</category>"
                        "<episode-num system=\"xmltv_ns\">1.4.0/1</episode-num>"));
    prog("news.us", base + hour, base + 2 * hour, QStringLiteral("<title>Weather Hour</title>"));
    // No stop times: inferred from the next programme, last one gets a default.
    prog("kids-42", base - 1800, 0, QStringLiteral("<title><![CDATA[Toons <Live>]]></title>"));
    prog("kids-42", base + 1800, 0, QStringLiteral("<title>Story Time</title>"));
    prog("unused", base, base + hour, QStringLiteral("<title>Skipped</title>"));
    // Far outside the kept window.
    prog("news.us", now - 30 * 24 * hour, now - 30 * 24 * hour + hour,
         QStringLiteral("<title>Ancient</title>"));
    x += "</tv>\n";
    return x;
}

QVector<Channel> samplePlaylist()
{
    Channel a; a.name = QStringLiteral("US: Harbor News HD"); a.tvgId = QStringLiteral("NEWS.US");
    Channel b; b.name = QStringLiteral("Cartoon Kingdom FHD");
    Channel c; c.name = QStringLiteral("No Guide Channel");
    return {a, b, c};
}

} // namespace

class TestEpg : public QObject
{
    Q_OBJECT

private slots:
    void parseTime()
    {
        bool ok = false;
        QCOMPARE(XmltvParser::parseTime(u"20261108200000 +0000", &ok), qint64(1794168000));
        QVERIFY(ok);
        QCOMPARE(XmltvParser::parseTime(u"20261108210000 +0100"), qint64(1794168000));
        QCOMPARE(XmltvParser::parseTime(u"20261108150000 -0500"), qint64(1794168000));
        QCOMPARE(XmltvParser::parseTime(u"20261108150000 -05:00"), qint64(1794168000));
        QCOMPARE(XmltvParser::parseTime(u"202611082000"), qint64(1794168000));   // no seconds, UTC
        XmltvParser::parseTime(u"2026110", &ok);
        QVERIFY(!ok);
        XmltvParser::parseTime(u"20261340200000", &ok);
        QVERIFY(!ok);
    }

    void episodes()
    {
        QCOMPARE(XmltvParser::episodeFromXmltvNs(QStringLiteral("1.4.0/1")), QStringLiteral("S2 E5"));
        QCOMPARE(XmltvParser::episodeFromXmltvNs(QStringLiteral(".9.")), QStringLiteral("E10"));
        QCOMPARE(XmltvParser::episodeFromXmltvNs(QStringLiteral("0/3..")), QStringLiteral("S1"));
    }

    void parse_data()
    {
        QTest::addColumn<bool>("compressed");
        QTest::addColumn<int>("chunk");
        QTest::newRow("plain, whole") << false << (1 << 20);
        QTest::newRow("plain, 7-byte chunks") << false << 7;
        QTest::newRow("gzip, whole") << true << (1 << 20);
        QTest::newRow("gzip, 5-byte chunks") << true << 5;
    }

    void parse()
    {
        QFETCH(bool, compressed);
        QFETCH(int, chunk);
        const qint64 now = 1794168000 + 1200;
        QByteArray data = sampleGuide(now);
        if (compressed) data = gzip(data);

        XmltvParser p;
        QVector<EpgChannel> channels;
        QVector<Programme> programmes;
        p.onChannel = [&](const EpgChannel &c) { channels << c; };
        p.onProgramme = [&](const Programme &pr) { programmes << pr; };
        for (int i = 0; i < data.size(); i += chunk) p.feed(data.mid(i, chunk));
        QVERIFY(p.finish());
        QVERIFY2(!p.failed(), qPrintable(p.errorString()));

        QCOMPARE(channels.size(), 3);
        QCOMPARE(channels[1].names, (QStringList{QStringLiteral("Cartoon Kingdom"), QStringLiteral("CK")}));
        QCOMPARE(channels[0].icon, QStringLiteral("http://l/n.png"));
        QCOMPARE(programmes.size(), 7);
        QCOMPARE(programmes[0].title, QStringLiteral("Morning & More"));
        QCOMPARE(programmes[1].title, QStringLiteral("The Bulletin"));       // English preferred
        QCOMPARE(programmes[1].subTitle, QStringLiteral("Late edition"));
        QCOMPARE(programmes[1].episode, QStringLiteral("S2 E5"));
        QCOMPARE(programmes[1].category, QStringLiteral("News"));
        QCOMPARE(programmes[3].title, QStringLiteral("Toons <Live>"));        // CDATA
        QCOMPARE(programmes[3].stop, qint64(0));                              // no stop given
    }

    void truncatedFileKeepsData()
    {
        const QByteArray data = sampleGuide(1794168000);
        XmltvParser p;
        int count = 0;
        p.onProgramme = [&](const Programme &) { ++count; };
        p.feed(data.left(data.indexOf("Story Time")));
        QVERIFY(p.finish());               // usable
        QVERIFY(!p.errorString().isEmpty());
        QCOMPARE(count, 4);
    }

    void rejectsNonGuide()
    {
        XmltvParser p;
        p.feed("<html><body>404 Not Found</body></html>");
        QVERIFY(!p.finish());
        XmltvParser q;
        q.feed("not xml at all");
        QVERIFY(!q.finish());
    }

    void matcher()
    {
        using EpgMatcher::normalize;
        QCOMPARE(normalize(QStringLiteral("US: CNN HD")), normalize(QStringLiteral("CNN")));
        QCOMPARE(normalize(QStringLiteral("[UK] BBC One FHD")), QStringLiteral("bbcone"));
        QCOMPARE(normalize(QStringLiteral("UK | BBC One (Backup)")), QStringLiteral("bbcone"));
        QCOMPARE(normalize(QStringLiteral("Sky Cinema+1")), QStringLiteral("skycinemaplus1"));
        QVERIFY(normalize(QStringLiteral("Sky Cinema +1")) != normalize(QStringLiteral("Sky Cinema")));
        QCOMPARE(normalize(QStringLiteral("AT&T SportsNet")), normalize(QStringLiteral("AT and T SportsNet")));
        QCOMPARE(normalize(QStringLiteral("Das Erste HD")), QStringLiteral("daserste"));
    }

    void storeAndService()
    {
        QTemporaryDir dir;
        const QString guidePath = dir.filePath(QStringLiteral("guide.xml.gz"));
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        {
            QFile f(guidePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(gzip(sampleGuide(now)));
        }

        EpgService epg(dir.filePath(QStringLiteral("guide.sqlite")));
        QVERIFY(!epg.hasGuide());
        QSignalSpy updated(&epg, &EpgService::guideUpdated);
        QSignalSpy failed(&epg, &EpgService::importFailed);

        const QVector<Channel> playlist = samplePlaylist();
        epg.refresh(playlist, {guidePath}, false);
        // Counts rather than wait(): the signal can arrive before a wait starts.
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), 1, 10000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(epg.hasGuide());
        QCOMPARE(epg.programmeCount(), 5);   // "unused" and "Ancient" skipped

        const QVector<QString> ids = epg.resolve(playlist);
        QCOMPARE(ids[0], QStringLiteral("news.us"));   // tvg-id, case-insensitive
        QCOMPARE(ids[1], QStringLiteral("kids-42"));   // by name
        QCOMPARE(ids[2], QString());

        const auto news = epg.nowNext(ids[0]);
        QVERIFY(news.hasNow);
        QCOMPARE(news.now.title, QStringLiteral("The Bulletin"));
        QCOMPARE(news.now.desc, QStringLiteral("Top stories."));
        QVERIFY(news.hasNext);
        QCOMPARE(news.next.title, QStringLiteral("Weather Hour"));

        // Missing stop times were inferred from the following programme.
        const qint64 base = now - now % 3600;
        const auto kids = epg.range(ids[1], base - 3600, base + 7200);
        QCOMPARE(kids.size(), 2);
        QCOMPARE(kids[0].stop, base + 1800);
        QCOMPARE(kids[1].stop, base + 1800 + 3600);

        // Title search: airing now first, then upcoming; past shows excluded.
        const QVector<Programme> hits = epg.search(QStringLiteral("the"), 10);
        QVERIFY(!hits.isEmpty());
        QCOMPARE(hits.first().title, QStringLiteral("The Bulletin"));
        QCOMPARE(epg.search(QStringLiteral("weather"), 10).size(), 1);
        QCOMPARE(epg.search(QStringLiteral("Morning"), 10).size(), 0);   // already over
        QCOMPARE(epg.search(QStringLiteral("100%"), 10).size(), 0);      // wildcards are literal

        // Same source and channels: no re-import.
        epg.refresh(playlist, {guidePath}, false);
        QVERIFY(!epg.busy());
        QVERIFY(!updated.wait(300));

        // Forced: imports again.
        epg.refresh(playlist, {guidePath}, true);
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), 2, 10000);   // may already have arrived

        // A bad guide keeps the old data.
        const QString bad = dir.filePath(QStringLiteral("bad.xml"));
        { QFile f(bad); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("<html>nope</html>"); }
        epg.refresh(playlist, {bad}, true);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(epg.programmeCount(), 5);
        QVERIFY(!epg.lastError().isEmpty());

        // Guide with no matching channels is reported, old data kept.
        QVector<Channel> other(1);
        other[0].name = QStringLiteral("Something Else");
        epg.refresh(other, {guidePath}, true);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 10000);
        QVERIFY(epg.lastError().contains(QStringLiteral("match")));
        QCOMPARE(epg.programmeCount(), 5);
    }

    void severalSourcesOffsetsAndChoices()
    {
        QTemporaryDir dir;
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 base = now - now % 3600;
        auto prog = [](const char *ch, qint64 start, const char *title) {
            return QStringLiteral("<programme channel=\"%1\" start=\"%2 +0000\" stop=\"%3 +0000\"><title>%4</title></programme>\n")
                .arg(QLatin1String(ch), QDateTime::fromSecsSinceEpoch(start).toUTC().toString(QStringLiteral("yyyyMMddHHmmss")),
                     QDateTime::fromSecsSinceEpoch(start + 3600).toUTC().toString(QStringLiteral("yyyyMMddHHmmss")),
                     QLatin1String(title));
        };
        auto write = [&](const QString &name, const QString &body) {
            QFile f(dir.filePath(name));
            if (f.open(QIODevice::WriteOnly)) f.write(("<?xml version=\"1.0\"?><tv>" + body + "</tv>").toUtf8());
            return f.fileName();
        };
        // First file: news; second file: news again (ignored, first wins),
        // movies, and a channel nobody's playlist names.
        const QString a = write(QStringLiteral("a.xml"),
            QStringLiteral("<channel id=\"news\"><display-name>News</display-name></channel>")
            + prog("news", base, "A News") + prog("news", base + 3600, "A Later"));
        const QString b = write(QStringLiteral("b.xml"),
            QStringLiteral("<channel id=\"news\"><display-name>News</display-name></channel>"
                           "<channel id=\"movies\"><display-name>Movies</display-name></channel>"
                           "<channel id=\"spare\"><display-name>Spare Channel</display-name></channel>")
            + prog("news", base, "B News") + prog("movies", base, "B Film") + prog("spare", base, "Spare Show"));

        QVector<Channel> playlist(3);
        playlist[0].name = QStringLiteral("News");
        playlist[1].name = QStringLiteral("Movies");
        playlist[2].name = QStringLiteral("Mystery");          // no automatic match
        playlist[1].tvgShiftMinutes = 60;                       // tvg-shift="1"

        EpgService epg(dir.filePath(QStringLiteral("guide.sqlite")));
        QSignalSpy updated(&epg, &EpgService::guideUpdated);
        epg.refresh(playlist, {a, b}, true);
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), 1, 10000);
        QCOMPARE(epg.programmeCount(), 3);                      // A News, A Later, B Film

        QVector<QString> ids = epg.resolve(playlist);
        QCOMPARE(epg.nowNext(ids[0]).now.title, QStringLiteral("A News"));
        // tvg-shift: the film that started this hour shows an hour later.
        QCOMPARE(EpgService::baseId(ids[1]), QStringLiteral("movies"));
        QVERIFY(!epg.nowNext(ids[1]).hasNow);
        QCOMPARE(epg.nowNext(ids[1]).next.start, base + 3600);
        QCOMPARE(ids[2], QString());

        // Every guide channel can be picked, even ones that weren't kept.
        QCOMPARE(epg.guideChannels(QStringLiteral("spare")).size(), 1);
        QCOMPARE(epg.guideChannels(QString()).size(), 3);
        QVERIFY(!epg.hasChannel(QStringLiteral("spare")));
        // Picked by hand: kept at the next import, then used.
        epg.refresh(playlist, {a, b}, false, {QStringLiteral("spare")});
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), 2, 10000);
        QVERIFY(epg.hasChannel(QStringLiteral("spare")));
        ids = epg.resolve(playlist, {{2, QStringLiteral("spare")}, {0, QStringLiteral("-")}}, {{1, 0}});
        QCOMPARE(epg.nowNext(ids[2]).now.title, QStringLiteral("Spare Show"));
        QCOMPARE(ids[0], QString());                            // "no listings"
        QCOMPARE(epg.nowNext(ids[1]).now.title, QStringLiteral("B Film"));   // offset reset to 0

        // The playlist's own offset moves everything.
        epg.setOffsetMinutes(-60);
        QCOMPARE(epg.nowNext(ids[2]).hasNow, false);
        QCOMPARE(epg.range(ids[2], base - 7200, base + 7200).first().start, base - 3600);
        epg.setOffsetMinutes(0);

        // Fewer days kept: listings beyond are dropped at the next import.
        const QString c = write(QStringLiteral("c.xml"),
            QStringLiteral("<channel id=\"news\"><display-name>News</display-name></channel>")
            + prog("news", base, "Today") + prog("news", base + 3 * 86400, "Far Away"));
        epg.setDaysAhead(2);
        const int before = int(updated.count());      // offsets above also announce changes
        epg.refresh(playlist, {c}, true);
        QTRY_COMPARE_WITH_TIMEOUT(updated.count(), before + 1, 10000);
        QCOMPARE(epg.programmeCount(), 1);
    }
};

QTEST_GUILESS_MAIN(TestEpg)
#include "test_epg.moc"
