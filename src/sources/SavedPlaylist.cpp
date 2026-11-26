#include "SavedPlaylist.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>

#include <utility>

namespace {

constexpr quint32 kMagic = 0x524c5943;      // "RLYC"
constexpr quint32 kVersion = 1;
constexpr int kHeader = 8 + 20;             // magic, version, SHA-1 of the rest
constexpr qint64 kMaxFile = qint64(1) << 29;
constexpr qint32 kMaxEntries = 10000000;

// Where a secret was: a control character, which no address contains.
QChar marker(int i) { return QChar(char16_t(1 + i)); }

bool hasMarker(const QString &s, qsizetype count)
{
    for (const QChar c : s)
        if (c.unicode() >= 1 && c.unicode() <= count) return true;
    return false;
}

QString masked(QString s, const QStringList &secrets)
{
    for (qsizetype i = 0; i < secrets.size(); ++i)
        if (!secrets.at(i).isEmpty()) s.replace(secrets.at(i), QString(marker(int(i))));
    return s;
}

// The exact reverse of masked(): the last secret first.
QString unmasked(QString s, const QStringList &secrets)
{
    for (qsizetype i = secrets.size() - 1; i >= 0; --i)
        s.replace(marker(int(i)), secrets.at(i));
    return s;
}

// Text is saved as UTF-8: half the size of Qt's own format for addresses.
void put(QDataStream &out, const QString &s) { out << s.toUtf8(); }

QString take(QDataStream &in)
{
    QByteArray b;
    in >> b;
    return QString::fromUtf8(b);
}

void putChannel(QDataStream &out, const Channel &c, const QStringList &secrets)
{
    out << qint32(c.number);
    put(out, c.name);
    put(out, c.group);
    put(out, masked(c.logo, secrets));
    put(out, masked(c.url, secrets));
    put(out, c.tvgId);
    out << qint32(c.tvgShiftMinutes);
    put(out, c.userAgent);
    put(out, c.referrer);
    put(out, c.catchupType);
    put(out, masked(c.catchupSource, secrets));
    out << qint32(c.catchupDays);
}

Channel takeChannel(QDataStream &in, const QStringList &secrets)
{
    Channel c;
    qint32 number = 0, shift = 0, days = 0;
    in >> number;
    c.number = number;
    c.name = take(in);
    c.group = take(in);
    c.logo = unmasked(take(in), secrets);
    c.url = unmasked(take(in), secrets);
    c.tvgId = take(in);
    in >> shift;
    c.tvgShiftMinutes = shift;
    c.userAgent = take(in);
    c.referrer = take(in);
    c.catchupType = take(in);
    c.catchupSource = unmasked(take(in), secrets);
    in >> days;
    c.catchupDays = days;
    return c;
}

bool putList(QDataStream &out, const QVector<Channel> &list, const QStringList &secrets)
{
    out << qint32(list.size());
    for (const Channel &c : list) {
        // A marker already in an address couldn't be told from a secret's.
        if (hasMarker(c.logo, secrets.size()) || hasMarker(c.url, secrets.size())
            || hasMarker(c.catchupSource, secrets.size()))
            return false;
        putChannel(out, c, secrets);
    }
    return true;
}

bool takeList(QDataStream &in, QVector<Channel> *list, const QStringList &secrets)
{
    qint32 n = -1;
    in >> n;
    if (in.status() != QDataStream::Ok || n < 0 || n > kMaxEntries) return false;
    list->clear();
    list->reserve(n);
    for (qint32 i = 0; i < n && in.status() == QDataStream::Ok; ++i)
        list->push_back(takeChannel(in, secrets));
    return in.status() == QDataStream::Ok;
}

// Copies left half-written when Relay was closed while saving (QSaveFile
// names them after the file).
void removeLeftovers(const QString &path)
{
    const QFileInfo target(path);
    const QDateTime old = QDateTime::currentDateTimeUtc().addSecs(-600);
    const QFileInfoList found = target.absoluteDir().entryInfoList(
        {target.fileName() + QStringLiteral(".*")}, QDir::Files);
    for (const QFileInfo &f : found)
        if (f.lastModified().toUTC() < old) QFile::remove(f.absoluteFilePath());
}

void addNumber(QCryptographicHash &h, qint64 v)
{
    v = qToLittleEndian(v);
    h.addData(QByteArrayView(reinterpret_cast<const char *>(&v), sizeof v));
}

void addText(QCryptographicHash &h, const QString &s)
{
    addNumber(h, s.size());
    h.addData(QByteArrayView(reinterpret_cast<const char *>(s.constData()),
                             s.size() * qsizetype(sizeof(QChar))));
}

} // namespace

bool SavedPlaylist::save(const QString &path, const PlaylistResult &result, const QByteArray &source,
                         const QStringList &secrets)
{
    if (hasMarker(result.epgUrl, secrets.size())) return false;
    QByteArray payload;
    {
        QDataStream out(&payload, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_0);
        out << source;
        put(out, masked(result.epgUrl, secrets));
        out << qint32(result.groupOrder.size());
        for (const QString &g : result.groupOrder) put(out, g);
        out << qint32(result.skippedMovies) << qint32(result.skippedSeries);
        if (!putList(out, result.channels, secrets) || !putList(out, result.vodMovies, secrets)
            || !putList(out, result.vodEpisodes, secrets) || out.status() != QDataStream::Ok)
            return false;
    }
    char header[8];
    qToBigEndian<quint32>(kMagic, header);
    qToBigEndian<quint32>(kVersion, header + 4);

    QDir().mkpath(QFileInfo(path).absolutePath());
    removeLeftovers(path);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(header, sizeof header);
    f.write(QCryptographicHash::hash(payload, QCryptographicHash::Sha1));
    f.write(payload);
    return f.commit();
}

bool SavedPlaylist::read(const QString &path, const QByteArray &source, const QStringList &secrets,
                         PlaylistResult *result)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < kHeader || f.size() > kMaxFile) return false;
    const QByteArray data = f.readAll();
    if (data.size() < kHeader || qFromBigEndian<quint32>(data.constData()) != kMagic
        || qFromBigEndian<quint32>(data.constData() + 4) != kVersion)
        return false;
    const QByteArray payload = QByteArray::fromRawData(data.constData() + kHeader, data.size() - kHeader);
    if (QCryptographicHash::hash(payload, QCryptographicHash::Sha1) != data.mid(8, 20)) return false;

    QDataStream in(payload);
    in.setVersion(QDataStream::Qt_6_0);
    QByteArray saved;
    in >> saved;
    if (saved != source) return false;

    PlaylistResult r;
    r.epgUrl = unmasked(take(in), secrets);
    qint32 groups = -1;
    in >> groups;
    if (in.status() != QDataStream::Ok || groups < 0 || groups > kMaxEntries) return false;
    r.groupOrder.reserve(groups);
    for (qint32 i = 0; i < groups && in.status() == QDataStream::Ok; ++i) r.groupOrder << take(in);
    qint32 movies = 0, series = 0;
    in >> movies >> series;
    r.skippedMovies = movies;
    r.skippedSeries = series;
    if (!takeList(in, &r.channels, secrets) || !takeList(in, &r.vodMovies, secrets)
        || !takeList(in, &r.vodEpisodes, secrets) || r.channels.isEmpty())
        return false;

    r.savedAt = QFileInfo(path).lastModified().toUTC();
    if (!r.savedAt.isValid()) r.savedAt = QDateTime::currentDateTimeUtc().addYears(-1);
    r.digest = digest(r);
    *result = std::move(r);
    return true;
}

QByteArray SavedPlaylist::digest(const PlaylistResult &r)
{
    QCryptographicHash h(QCryptographicHash::Sha1);
    addText(h, r.epgUrl);
    addNumber(h, r.groupOrder.size());
    for (const QString &g : r.groupOrder) addText(h, g);
    addNumber(h, r.channels.size());
    for (const Channel &c : r.channels) {
        addNumber(h, c.number);
        addText(h, c.name);
        addText(h, c.group);
        addText(h, c.logo);
        addText(h, c.url);
        addText(h, c.tvgId);
        addNumber(h, c.tvgShiftMinutes);
        addText(h, c.userAgent);
        addText(h, c.referrer);
        addText(h, c.catchupType);
        addText(h, c.catchupSource);
        addNumber(h, c.catchupDays);
    }
    return h.result();
}

QByteArray SavedPlaylist::sourceKey(const QString &description, const QStringList &secrets)
{
    return QCryptographicHash::hash(masked(description, secrets).toUtf8(), QCryptographicHash::Sha256);
}
