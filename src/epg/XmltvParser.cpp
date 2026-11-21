#include "XmltvParser.h"

#include <QDate>

#include <zlib.h>

// ---- gzip -------------------------------------------------------------------------

class GzipInflater
{
public:
    GzipInflater()
    {
        m_zs.zalloc = Z_NULL;
        m_zs.zfree = Z_NULL;
        m_zs.opaque = Z_NULL;
        m_ok = inflateInit2(&m_zs, 15 + 32) == Z_OK;   // +32: auto-detect gzip/zlib header
    }
    ~GzipInflater() { if (m_ok) inflateEnd(&m_zs); }

    // Appends decompressed bytes to `out`. Returns false on corrupt data.
    bool inflateChunk(const QByteArray &in, QByteArray *out)
    {
        if (!m_ok) return false;
        m_zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
        m_zs.avail_in = uInt(in.size());
        char buf[64 * 1024];
        while (m_zs.avail_in > 0) {
            m_zs.next_out = reinterpret_cast<Bytef *>(buf);
            m_zs.avail_out = sizeof buf;
            const int rc = ::inflate(&m_zs, Z_NO_FLUSH);
            out->append(buf, int(sizeof buf - m_zs.avail_out));
            if (rc == Z_STREAM_END) {
                // Concatenated gzip members are legal; keep going if more follows.
                if (m_zs.avail_in > 0 && inflateReset(&m_zs) != Z_OK) return false;
                if (m_zs.avail_in == 0) break;
            } else if (rc == Z_BUF_ERROR) {
                break;                     // needs more input
            } else if (rc != Z_OK) {
                return false;
            }
        }
        return true;
    }

private:
    z_stream m_zs{};
    bool m_ok = false;
};

// ---- helpers ----------------------------------------------------------------------

namespace {

int digits(QStringView s, int from, int count, bool *ok)
{
    if (from + count > s.size()) { *ok = false; return 0; }
    int v = 0;
    for (int i = from; i < from + count; ++i) {
        if (!s[i].isDigit()) { *ok = false; return 0; }
        v = v * 10 + s[i].digitValue();
    }
    return v;
}

bool isEnglish(const QString &lang)
{
    return lang.isEmpty() || lang.startsWith(QLatin1String("en"), Qt::CaseInsensitive);
}

} // namespace

qint64 XmltvParser::parseTime(QStringView text, bool *ok)
{
    bool good = true;
    const QStringView s = text.trimmed();
    const int y = digits(s, 0, 4, &good);
    const int mo = digits(s, 4, 2, &good);
    const int d = digits(s, 6, 2, &good);
    const int h = digits(s, 8, 2, &good);
    const int mi = digits(s, 10, 2, &good);
    int pos = 12;
    int sec = 0;
    if (good && s.size() >= 14 && s[12].isDigit()) {
        sec = digits(s, 12, 2, &good);
        pos = 14;
    }
    const QDate date(y, mo, d);
    if (!good || !date.isValid() || h > 23 || mi > 59 || sec > 60) {
        if (ok) *ok = false;
        return 0;
    }

    // Optional timezone offset: "+0100", "-05:00", " Z". Missing means UTC.
    qint64 offset = 0;
    while (pos < s.size() && s[pos].isSpace()) ++pos;
    if (pos < s.size() && (s[pos] == QLatin1Char('+') || s[pos] == QLatin1Char('-'))) {
        const int sign = s[pos] == QLatin1Char('-') ? -1 : 1;
        QString tz = s.mid(pos + 1).toString();
        tz.remove(QLatin1Char(':'));
        bool tzOk = true;
        const int oh = digits(tz, 0, 2, &tzOk);
        const int om = tz.size() >= 4 ? digits(tz, 2, 2, &tzOk) : 0;
        if (tzOk) offset = sign * (oh * 3600 + om * 60);
    }

    const qint64 days = date.toJulianDay() - 2440588;   // days since 1970-01-01
    if (ok) *ok = true;
    return days * 86400 + h * 3600 + mi * 60 + sec - offset;
}

QString XmltvParser::episodeFromXmltvNs(const QString &text)
{
    // "season.episode.part", zero-based, each part may be "n/total".
    const QStringList parts = text.split(QLatin1Char('.'));
    auto number = [](const QString &p) {
        bool ok = false;
        const int n = p.section(QLatin1Char('/'), 0, 0).trimmed().toInt(&ok);
        return ok ? n + 1 : 0;
    };
    const int season = parts.size() > 0 ? number(parts[0]) : 0;
    const int episode = parts.size() > 1 ? number(parts[1]) : 0;
    QStringList out;
    if (season > 0) out << QStringLiteral("S%1").arg(season);
    if (episode > 0) out << QStringLiteral("E%1").arg(episode);
    return out.join(QLatin1Char(' '));
}

// ---- parser -------------------------------------------------------------------------

XmltvParser::XmltvParser() = default;
XmltvParser::~XmltvParser() = default;

void XmltvParser::setError(const QString &message)
{
    m_failed = true;
    if (m_error.isEmpty()) m_error = message;
}

void XmltvParser::feed(const QByteArray &bytes)
{
    if (m_failed || m_done || bytes.isEmpty()) return;

    if (!m_sniffed) {
        m_sniffed = true;
        if (bytes.size() >= 2 && uchar(bytes[0]) == 0x1f && uchar(bytes[1]) == 0x8b)
            m_gzip = std::make_unique<GzipInflater>();
    }

    if (m_gzip) {
        QByteArray plain;
        if (!m_gzip->inflateChunk(bytes, &plain)) {
            setError(QStringLiteral("The guide file is compressed but damaged."));
            return;
        }
        m_reader.addData(plain);
    } else {
        m_reader.addData(bytes);
    }
    pump();
}

bool XmltvParser::finish()
{
    if (!m_failed && !m_done) {
        pump();
        if (!m_done && !m_failed) {
            if (m_programmes > 0 || m_channels > 0)
                m_error = QStringLiteral("The guide file ended early; using what was received.");
            else
                setError(QStringLiteral("The guide file is empty or incomplete."));
        }
    }
    if (!m_failed && m_channels == 0 && m_programmes == 0)
        setError(QStringLiteral("No guide data found. Check that the address points to an XMLTV file."));
    // A damaged file that still gave us data is usable.
    return m_programmes > 0 || !m_failed;
}

void XmltvParser::pump()
{
    while (!m_failed && !m_done) {
        const auto token = m_reader.readNext();
        switch (token) {
        case QXmlStreamReader::Invalid:
            if (m_reader.error() == QXmlStreamReader::PrematureEndOfDocumentError) return;
            setError(QStringLiteral("The guide file isn't valid XMLTV (line %1: %2).")
                         .arg(m_reader.lineNumber())
                         .arg(m_reader.errorString()));
            return;
        case QXmlStreamReader::EndDocument:
            m_done = true;
            return;
        case QXmlStreamReader::StartElement:
            startElement();
            break;
        case QXmlStreamReader::EndElement:
            endElement();
            break;
        case QXmlStreamReader::Characters:
            if (!m_leaf.isEmpty()) m_text += m_reader.text();
            break;
        default:
            break;
        }
    }
}

void XmltvParser::startElement()
{
    const QStringView name = m_reader.name();
    const QXmlStreamAttributes attrs = m_reader.attributes();

    if (m_section == Section::None) {
        if (name == QLatin1String("channel")) {
            m_section = Section::Channel;
            m_channel = EpgChannel();
            m_channel.id = attrs.value(QLatin1String("id")).toString().trimmed();
        } else if (name == QLatin1String("programme")) {
            m_section = Section::Programme;
            m_programme = Programme();
            m_titleIsEnglish = m_descIsEnglish = false;
            bool okStart = false, okStop = false;
            m_programme.channel = attrs.value(QLatin1String("channel")).toString().trimmed();
            m_programme.start = parseTime(attrs.value(QLatin1String("start")), &okStart);
            const QStringView stop = attrs.value(QLatin1String("stop"));
            if (!stop.isEmpty()) m_programme.stop = parseTime(stop, &okStop);
            if (!okStop || m_programme.stop <= m_programme.start) m_programme.stop = 0;
            m_programmeOk = okStart && !m_programme.channel.isEmpty();
        }
        return;
    }

    if (!m_leaf.isEmpty()) return;   // ignore markup nested inside text elements

    if (m_section == Section::Channel) {
        if (name == QLatin1String("display-name")) {
            m_leaf = name.toString();
            m_text.clear();
        } else if (name == QLatin1String("icon") && m_channel.icon.isEmpty()) {
            m_channel.icon = attrs.value(QLatin1String("src")).toString();
        }
    } else if (m_section == Section::Programme) {
        if (name == QLatin1String("title") || name == QLatin1String("sub-title")
            || name == QLatin1String("desc") || name == QLatin1String("category")
            || name == QLatin1String("episode-num")) {
            m_leaf = name.toString();
            m_leafLang = attrs.value(QLatin1String("lang")).toString();
            m_leafSystem = attrs.value(QLatin1String("system")).toString();
            m_text.clear();
        }
    }
}

void XmltvParser::endElement()
{
    const QStringView name = m_reader.name();

    if (!m_leaf.isEmpty()) {
        if (name != m_leaf) return;
        const QString text = m_text.trimmed();
        m_leaf.clear();
        if (text.isEmpty()) return;

        if (m_section == Section::Channel) {
            m_channel.names << text;
        } else if (name == QLatin1String("title")) {
            // First title wins, unless a later one is English and the first wasn't.
            const bool en = isEnglish(m_leafLang);
            if (m_programme.title.isEmpty() || (en && !m_titleIsEnglish)) {
                m_programme.title = text;
                m_titleIsEnglish = en;
            }
        } else if (name == QLatin1String("desc")) {
            const bool en = isEnglish(m_leafLang);
            if (m_programme.desc.isEmpty() || (en && !m_descIsEnglish)) {
                m_programme.desc = text;
                m_descIsEnglish = en;
            }
        } else if (name == QLatin1String("sub-title")) {
            if (m_programme.subTitle.isEmpty()) m_programme.subTitle = text;
        } else if (name == QLatin1String("category")) {
            if (m_programme.category.isEmpty()) m_programme.category = text;
        } else if (name == QLatin1String("episode-num")) {
            if (m_leafSystem == QLatin1String("onscreen")) m_programme.episode = text;
            else if (m_leafSystem == QLatin1String("xmltv_ns") && m_programme.episode.isEmpty())
                m_programme.episode = episodeFromXmltvNs(text);
        }
        return;
    }

    if (m_section == Section::Channel && name == QLatin1String("channel")) {
        m_section = Section::None;
        if (!m_channel.id.isEmpty()) {
            ++m_channels;
            if (onChannel) onChannel(m_channel);
        }
    } else if (m_section == Section::Programme && name == QLatin1String("programme")) {
        m_section = Section::None;
        if (m_programmeOk && m_programme.isValid()) {
            ++m_programmes;
            if (onProgramme) onProgramme(m_programme);
        }
    }
}
