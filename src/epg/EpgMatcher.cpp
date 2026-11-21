#include "EpgMatcher.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace EpgMatcher {

QString normalize(const QString &name)
{
    // Leading provider/country tags: "US:", "UK |", "[DE]", "(FR)".
    static const QRegularExpression prefix(
        QStringLiteral("^\\s*(\\[[^\\]]*\\]|\\([^)]*\\)|[a-z]{2,4}\\s*[:|])\\s*"));
    // Bracketed notes anywhere: "[HD]", "(backup)".
    static const QRegularExpression brackets(QStringLiteral("\\[[^\\]]*\\]|\\([^)]*\\)"));
    static const QRegularExpression separators(QStringLiteral("[^\\p{L}\\p{N}]+"));
    // Quality / encoding tags that differ between playlist and guide names.
    static const QSet<QString> noise = {
        QStringLiteral("hd"),    QStringLiteral("fhd"),   QStringLiteral("uhd"),
        QStringLiteral("sd"),    QStringLiteral("hq"),    QStringLiteral("4k"),
        QStringLiteral("8k"),    QStringLiteral("hevc"),  QStringLiteral("h264"),
        QStringLiteral("h265"),  QStringLiteral("720p"),  QStringLiteral("1080p"),
        QStringLiteral("1080i"), QStringLiteral("2160p"), QStringLiteral("50fps"),
        QStringLiteral("60fps"), QStringLiteral("backup"), QStringLiteral("raw"),
        QStringLiteral("tv"),
    };

    QString s = name.toLower().trimmed();
    for (int i = 0; i < 3; ++i) {
        const QString before = s;
        s.remove(prefix);
        if (s == before) break;
    }
    s.replace(brackets, QStringLiteral(" "));
    s.replace(QStringLiteral("+1"), QStringLiteral(" plus1 "));
    s.replace(QLatin1Char('&'), QStringLiteral(" and "));

    QStringList kept;
    const QStringList tokens = s.split(separators, Qt::SkipEmptyParts);
    for (const QString &t : tokens)
        if (!noise.contains(t)) kept << t;

    // Keep "TV" if it was the whole name (e.g. a channel literally called "TV").
    if (kept.isEmpty()) return tokens.join(QString());
    return kept.join(QString());
}

} // namespace EpgMatcher
