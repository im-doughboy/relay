#pragma once

#include "Programme.h"

#include <QByteArray>
#include <QStringView>
#include <QXmlStreamReader>

#include <functional>
#include <memory>

class GzipInflater;

// Incremental XMLTV parser. Feed it bytes as they arrive (plain XML or
// gzip-compressed, detected automatically) and it calls onChannel /
// onProgramme for each complete entry. Memory use stays flat no matter how
// large the guide file is.
class XmltvParser
{
public:
    XmltvParser();
    ~XmltvParser();

    std::function<void(const EpgChannel &)> onChannel;
    std::function<void(const Programme &)> onProgramme;

    void feed(const QByteArray &bytes);
    // Call after the last chunk. Returns false if the file was unusable.
    bool finish();

    bool failed() const { return m_failed; }
    QString errorString() const { return m_error; }
    qint64 channelCount() const { return m_channels; }
    qint64 programmeCount() const { return m_programmes; }

    // "20261108200000 +0100" -> Unix seconds (UTC). Seconds and offset optional.
    static qint64 parseTime(QStringView text, bool *ok = nullptr);
    // xmltv_ns "1.4.0/1" -> "S2 E5"
    static QString episodeFromXmltvNs(const QString &text);

private:
    enum class Section { None, Channel, Programme };

    void pump();
    void startElement();
    void endElement();
    void setError(const QString &message);

    QXmlStreamReader m_reader;
    std::unique_ptr<GzipInflater> m_gzip;
    bool m_sniffed = false;
    bool m_failed = false;
    bool m_done = false;
    QString m_error;

    Section m_section = Section::None;
    EpgChannel m_channel;
    Programme m_programme;
    bool m_programmeOk = false;
    QString m_leaf;           // child element whose text we're collecting
    QString m_leafLang;
    QString m_leafSystem;
    QString m_text;
    bool m_titleIsEnglish = false;
    bool m_descIsEnglish = false;

    qint64 m_channels = 0;
    qint64 m_programmes = 0;
};
