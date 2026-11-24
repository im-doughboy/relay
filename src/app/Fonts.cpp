#include "Fonts.h"

#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QtDebug>

void useBundledFonts()
{
    QString family;
    for (const char *file : {"Inter-Regular.ttf", "Inter-Medium.ttf", "Inter-SemiBold.ttf", "Inter-Bold.ttf"}) {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + QLatin1String(file));
        if (id < 0) {
            qWarning().noquote() << "Fonts: couldn't load" << file;
            continue;
        }
        if (family.isEmpty()) family = QFontDatabase::applicationFontFamilies(id).value(0);
    }
    if (family.isEmpty()) return;   // the system's font, as before
    // Only the family changes: Qt still falls back to the system's fonts for
    // scripts Inter doesn't cover, so no channel name shows as empty boxes.
    QFont font = QGuiApplication::font();
    font.setFamilies({family});
    QGuiApplication::setFont(font);
}
