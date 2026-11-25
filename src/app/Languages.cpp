#include "Languages.h"

#include <QRegularExpression>

namespace {

struct Language {
    const char *code;
    QStringList tags;    // what a track's language tag may say
    QStringList words;   // what its title may say
};

const QList<Language> &table()
{
    static const QList<Language> t = {
        {"en", {"en", "eng"}, {"english", "inglés", "ingles", "anglais", "englisch"}},
        {"es", {"es", "spa"}, {"spanish", "español", "espanol", "castellano", "latino", "castilian"}},
        {"fr", {"fr", "fre", "fra"}, {"french", "français", "francais", "vf", "vff"}},
        {"de", {"de", "ger", "deu"}, {"german", "deutsch"}},
        {"it", {"it", "ita"}, {"italian", "italiano"}},
        {"pt", {"pt", "por"}, {"portuguese", "português", "portugues", "brasileiro", "dublado"}},
        {"nl", {"nl", "dut", "nld"}, {"dutch", "nederlands"}},
        {"pl", {"pl", "pol"}, {"polish", "polski"}},
        {"tr", {"tr", "tur"}, {"turkish", "türkçe", "turkce"}},
        {"ru", {"ru", "rus"}, {"russian", "русский"}},
        {"ar", {"ar", "ara"}, {"arabic", "العربية", "عربي"}},
        {"hi", {"hi", "hin"}, {"hindi", "हिन्दी"}},
    };
    return t;
}

const Language *find(const QString &code)
{
    for (const Language &l : table())
        if (QLatin1String(l.code) == code) return &l;
    return nullptr;
}

}

namespace Languages {

QStringList codes()
{
    QStringList out;
    for (const Language &l : table()) out << QLatin1String(l.code);
    return out;
}

QString mpvList(const QString &code)
{
    const Language *l = find(code);
    return l ? l->tags.join(QLatin1Char(',')) : QString();
}

bool matches(const QString &code, const QString &trackLang, const QString &trackTitle)
{
    const Language *l = find(code);
    if (!l) return false;
    // "en", "eng", "en-US", "eng (English)"
    const QString tag = trackLang.trimmed().toLower().section(QRegularExpression(QStringLiteral("[-_ (]")), 0, 0);
    static const QStringList unknown{QStringLiteral("und"), QStringLiteral("unk"), QStringLiteral("mis"),
                                     QStringLiteral("mul"), QStringLiteral("zxx")};
    if (!tag.isEmpty() && !unknown.contains(tag)) return l->tags.contains(tag);
    // No (useful) tag: look for the language's name as a word in the title.
    const QString title = trackTitle.toLower();
    if (title.isEmpty()) return false;
    for (const QString &w : l->words) {
        const QRegularExpression re(QStringLiteral("(^|[^\\p{L}])%1($|[^\\p{L}])").arg(QRegularExpression::escape(w)));
        if (re.match(title).hasMatch()) return true;
    }
    return false;
}

}
