#include "Log.h"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QRegularExpression>

#include <cstdio>

namespace {

QMutex g_mutex;
QFile *g_file = nullptr;
QString g_path;

void handler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    // Providers often list channel logos that no longer exist; one line per
    // missing logo would bury everything useful.
    if (msg.contains(QLatin1String("QQuickImage: Error transferring"))) return;

    const char *level = "debug";
    switch (type) {
    case QtInfoMsg: level = "info"; break;
    case QtWarningMsg: level = "WARN"; break;
    case QtCriticalMsg: level = "ERROR"; break;
    case QtFatalMsg: level = "FATAL"; break;
    default: break;
    }
    const QByteArray line = QStringLiteral("%1 %2 %3\n")
                                .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                     QString::fromLatin1(level), msg)
                                .toUtf8();
    QMutexLocker lock(&g_mutex);
    if (g_file) {
        g_file->write(line);
        g_file->flush();
    }
    std::fputs(line.constData(), stderr);
}

} // namespace

namespace Log {

void install(const QString &dataDir)
{
    g_path = dataDir + QStringLiteral("/relay.log");
    const QString previous = dataDir + QStringLiteral("/relay.previous.log");
    QFile::remove(previous);
    QFile::rename(g_path, previous);

    g_file = new QFile(g_path);
    if (!g_file->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        delete g_file;
        g_file = nullptr;
    }
    qInstallMessageHandler(handler);
}

QString filePath() { return g_path; }

QString redact(const QString &url)
{
    static const QRegularExpression query(QStringLiteral("(password=)[^&\\s]*"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression path(
        QStringLiteral("/(live|movie|series|timeshift)/([^/\\s]+)/([^/\\s]+)/"));
    QString s = url;
    s.replace(query, QStringLiteral("\\1***"));
    s.replace(path, QStringLiteral("/\\1/\\2/***/"));
    return s;
}

} // namespace Log
