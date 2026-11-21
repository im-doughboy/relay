#pragma once

#include <QString>
#include <QStringList>

// A channel declared in an XMLTV file.
struct EpgChannel
{
    QString id;
    QStringList names;   // <display-name> values, in file order
    QString icon;
};

// One guide entry. Times are Unix seconds (UTC).
struct Programme
{
    QString channel;     // XMLTV channel id
    qint64 start = 0;
    qint64 stop = 0;     // 0 = not given in the file (inferred on import)
    QString title;
    QString subTitle;
    QString desc;
    QString category;
    QString episode;     // e.g. "S2 E5"

    bool isValid() const { return start > 0 && !title.isEmpty(); }
};
