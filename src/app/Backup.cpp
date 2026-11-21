#include "Backup.h"

#include "AppSettings.h"
#include "UserLibrary.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>
#include <QtDebug>

namespace {

const char kMarker[] = "relayBackup";
const char kPending[] = "/restore-pending.json";

QJsonObject readBackup(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = Backup::tr("Couldn't open that file.");
        return {};
    }
    if (f.size() > 64 * 1024 * 1024) {
        *error = Backup::tr("That file is too big to be a Relay backup.");
        return {};
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QLatin1String(kMarker)).toInt() < 1 || !o.value(QLatin1String("settings")).isObject()) {
        *error = Backup::tr("That isn't a Relay backup file.");
        return {};
    }
    return o;
}

// Only files Relay itself writes, inside its data folder.
bool safeRelative(const QString &rel)
{
    if (rel.isEmpty() || rel.contains(QLatin1String("..")) || rel.contains(QLatin1Char('\\'))
        || rel.contains(QLatin1Char(':')) || rel.startsWith(QLatin1Char('/')))
        return false;
    return rel == QLatin1String("schedule.json")
        || (rel.startsWith(QLatin1String("playlists/")) && rel.endsWith(QLatin1String("/library.json"))
            && rel.count(QLatin1Char('/')) == 2);
}

} // namespace

Backup::Backup(AppSettings *settings, UserLibrary *library, const QString &dataDir, QObject *parent)
    : QObject(parent), m_settings(settings), m_library(library), m_dataDir(dataDir)
{
}

QString Backup::localPath(const QString &file)
{
    const QUrl url(file);
    return url.isLocalFile() ? url.toLocalFile() : file;
}

QString Backup::suggestedName() const
{
    return QStringLiteral("Relay backup %1.relaybackup").arg(QDate::currentDate().toString(Qt::ISODate));
}

QString Backup::saveTo(const QString &file)
{
    QString path = localPath(file);
    if (path.isEmpty()) return tr("No file chosen.");
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".relaybackup");
    if (m_library) m_library->saveNow();

    QSettings &s = m_settings->store();
    s.sync();
    QJsonObject settings;
    for (const QString &key : s.allKeys()) {
        // Logins are written below, readable, whether or not they're in the
        // credential store on this computer.
        if (key.endsWith(QLatin1String("/m3uSecure")) || key.endsWith(QLatin1String("/xtreamPasswordSecure"))
            || key.endsWith(QLatin1String("/m3u")) || key.endsWith(QLatin1String("/xtreamPassword"))
            || key.startsWith(QLatin1String("tmdb/key")))
            continue;
        settings.insert(key, QJsonValue::fromVariant(s.value(key)));
    }
    if (!m_settings->tmdbKey().isEmpty()) settings.insert(QStringLiteral("tmdb/key"), m_settings->tmdbKey());
    for (const QString &id : m_settings->playlistIds()) {
        const AppSettings::Source src = m_settings->source(id);
        const QString base = QStringLiteral("playlists/%1/").arg(id);
        if (!src.m3u.isEmpty()) settings.insert(base + QStringLiteral("m3u"), src.m3u);
        if (!src.password.isEmpty()) settings.insert(base + QStringLiteral("xtreamPassword"), src.password);
    }

    QJsonObject files;
    QStringList names{QStringLiteral("schedule.json")};
    for (const QString &id : m_settings->playlistIds())
        names << QStringLiteral("playlists/%1/library.json").arg(id);
    for (const QString &rel : names) {
        QFile f(m_dataDir + QLatin1Char('/') + rel);
        if (f.open(QIODevice::ReadOnly)) files.insert(rel, QString::fromUtf8(f.readAll()));
    }

    QJsonArray playlistNames;
    for (const QString &id : m_settings->playlistIds()) playlistNames.append(m_settings->playlistName(id));

    QJsonObject root;
    root.insert(QLatin1String(kMarker), 1);
    root.insert(QStringLiteral("version"), QCoreApplication::applicationVersion());
    root.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    root.insert(QStringLiteral("playlistNames"), playlistNames);
    root.insert(QStringLiteral("settings"), settings);
    root.insert(QStringLiteral("files"), files);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) return tr("Couldn't write to that location.");
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!out.commit()) return tr("Couldn't write to that location.");
    qInfo().noquote() << "Backup: saved to" << QDir::toNativeSeparators(path);
    return QString();
}

QVariantMap Backup::describe(const QString &file) const
{
    QString error;
    const QJsonObject o = readBackup(localPath(file), &error);
    if (!error.isEmpty()) return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    QStringList names;
    for (const QJsonValue &v : o.value(QLatin1String("playlistNames")).toArray()) names << v.toString();
    const QDateTime created = QDateTime::fromString(o.value(QLatin1String("created")).toString(), Qt::ISODate);
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("created"), QLocale().toString(created, QLocale::ShortFormat)},
            {QStringLiteral("playlists"), names}};
}

QString Backup::restoreFrom(const QString &file)
{
    QString error;
    const QJsonObject o = readBackup(localPath(file), &error);
    if (!error.isEmpty()) return error;
    QSaveFile out(m_dataDir + QLatin1String(kPending));
    if (!out.open(QIODevice::WriteOnly)) return tr("Couldn't prepare the restore.");
    out.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    if (!out.commit()) return tr("Couldn't prepare the restore.");
    qInfo() << "Backup: restore prepared; applying at the next start";
    return QString();
}

void Backup::restart()
{
    QStringList args = QCoreApplication::arguments();
    args.removeFirst();
    QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
    QCoreApplication::quit();
}

bool Backup::applyPending(const QString &dataDir)
{
    const QString pending = dataDir + QLatin1String(kPending);
    if (!QFile::exists(pending)) return false;
    QString error;
    const QJsonObject o = readBackup(pending, &error);
    QFile::remove(pending);
    if (!error.isEmpty()) {
        qWarning().noquote() << "Backup: restore skipped:" << error;
        return false;
    }

    QSettings s;
    s.clear();
    const QJsonObject settings = o.value(QLatin1String("settings")).toObject();
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it)
        s.setValue(it.key(), it.value().toVariant());
    s.sync();

    // Each playlist's own files; guides and the movie catalogue download again.
    QDir(dataDir + QStringLiteral("/playlists")).removeRecursively();
    QFile::remove(dataDir + QStringLiteral("/schedule.json"));
    const QJsonObject files = o.value(QLatin1String("files")).toObject();
    int written = 0;
    for (auto it = files.constBegin(); it != files.constEnd(); ++it) {
        if (!safeRelative(it.key())) continue;
        const QString path = dataDir + QLatin1Char('/') + it.key();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) { f.write(it.value().toString().toUtf8()); ++written; }
    }
    qInfo().noquote() << QStringLiteral("Backup: restored %1 settings and %2 files")
                             .arg(settings.size()).arg(written);
    return true;
}
