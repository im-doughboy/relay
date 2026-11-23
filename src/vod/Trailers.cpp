#include "Trailers.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtDebug>

namespace {
constexpr int kKeepSecs = 2 * 3600;      // YouTube's addresses last about six hours
constexpr int kTimeoutMs = 25000;
// 720p at most (it's a preview or a trailer), H.264 + AAC where there's a choice.
const char kFormat[] = "bv*[height<=720][vcodec^=avc1]+ba[acodec^=mp4a]/bv*[height<=720]+ba/b[height<=720]/b";
} // namespace

Trailers::Trailers(QObject *parent) : QObject(parent)
{
    m_tool = findTool();
    if (m_tool.isEmpty()) qInfo().noquote() << "Trailers: yt-dlp not found; YouTube trailers open in the browser";
    else {
        qInfo().noquote() << "Trailers: using" << QDir::toNativeSeparators(m_tool);
        // YouTube changes often; the copy Relay brings keeps itself current (weekly).
        QTimer::singleShot(90 * 1000, this, &Trailers::updateTool);
    }
}

QString Trailers::findTool()
{
    const QString override = qEnvironmentVariable("RELAY_YTDLP");
    if (!override.isEmpty()) return QFileInfo::exists(override) ? override : QString();
    const QDir app(QCoreApplication::applicationDirPath());
    for (const QString &name : {QStringLiteral("yt-dlp.exe"), QStringLiteral("yt-dlp")})
        if (QFileInfo::exists(app.filePath(name))) return app.filePath(name);
    return QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
}

bool Trailers::autoplay() const { return QSettings().value(QStringLiteral("vod/autoplayTrailers"), true).toBool(); }

void Trailers::setAutoplay(bool on)
{
    QSettings().setValue(QStringLiteral("vod/autoplayTrailers"), on);
    emit autoplayChanged();
}

QString Trailers::youtubeId(const QString &trailerIn)
{
    const QString t = trailerIn.trimmed();
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9_-]{11}$"));
    if (bare.match(t).hasMatch()) return t;
    const QUrl url(t);
    const QString host = url.host().toLower();
    if (host.endsWith(QLatin1String("youtu.be"))) return url.path().mid(1).left(11);
    if (host.contains(QLatin1String("youtube"))) {
        const QString v = QUrlQuery(url).queryItemValue(QStringLiteral("v"));
        if (!v.isEmpty()) return v.left(11);
        static const QRegularExpression embed(QStringLiteral("/(?:embed|shorts|v)/([A-Za-z0-9_-]{11})"));
        const auto m = embed.match(url.path());
        if (m.hasMatch()) return m.captured(1);
    }
    return QString();
}

QString Trailers::pageUrl(const QString &trailer)
{
    const QString id = youtubeId(trailer);
    return id.isEmpty() ? trailer : QStringLiteral("https://www.youtube.com/watch?v=") + id;
}

void Trailers::resolve(const QString &trailer)
{
    if (trailer.trimmed().isEmpty()) { emit failed(trailer, tr("No trailer")); return; }
    const QString id = youtubeId(trailer);
    if (id.isEmpty()) {
        // A direct address (some providers): plays as it is.
        const QUrl url(trailer.trimmed());
        if (url.scheme().startsWith(QLatin1String("http"))) emit resolved(trailer, url.toString(), QString());
        else emit failed(trailer, tr("No trailer"));
        return;
    }
    const auto known = m_answers.constFind(id);
    if (known != m_answers.constEnd() && QDateTime::currentSecsSinceEpoch() - known->at < kKeepSecs) {
        emit resolved(trailer, known->video, known->audio);
        return;
    }
    if (m_tool.isEmpty()) { emit failed(trailer, tr("Trailers need yt-dlp")); return; }
    if (m_running.value(id)) return;   // already asking

    auto *p = new QProcess(this);
    m_running.insert(id, p);
    p->setProgram(m_tool);
#ifdef Q_OS_WIN
    // No console window flashing up for yt-dlp.
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; /* CREATE_NO_WINDOW */ });
#endif
    p->setArguments({QStringLiteral("--no-warnings"), QStringLiteral("--no-playlist"), QStringLiteral("-f"), QLatin1String(kFormat),
                     QStringLiteral("-g"), QStringLiteral("https://www.youtube.com/watch?v=") + id});
    auto *timeout = new QTimer(p);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, p, [p] { p->kill(); });
    connect(p, &QProcess::finished, this, [this, p, id, trailer](int code, QProcess::ExitStatus status) {
        p->deleteLater();
        m_running.remove(id);
        const QStringList lines = QString::fromUtf8(p->readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (status != QProcess::NormalExit || code != 0 || lines.isEmpty()) {
            const QString why = QString::fromUtf8(p->readAllStandardError()).trimmed().section(QLatin1Char('\n'), -1);
            qWarning().noquote() << "Trailers: yt-dlp couldn't get" << id << (why.isEmpty() ? QStringLiteral("(timed out)") : why);
            emit failed(trailer, tr("The trailer isn't available"));
            return;
        }
        Answer a{QDateTime::currentSecsSinceEpoch(), lines.value(0).trimmed(), lines.value(1).trimmed()};
        if (m_answers.size() > 200) m_answers.clear();
        m_answers.insert(id, a);
        emit resolved(trailer, a.video, a.audio);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p, id, trailer](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        m_running.remove(id);
        qWarning().noquote() << "Trailers: couldn't run yt-dlp:" << p->errorString();
        emit failed(trailer, tr("Couldn't run yt-dlp"));
    });
    p->start();
    timeout->start(kTimeoutMs);
}

void Trailers::updateTool()
{
    // Only the copy next to Relay (installed with it), at most once a week.
    if (QFileInfo(m_tool).absolutePath() != QFileInfo(QCoreApplication::applicationFilePath()).absolutePath()) return;
    QSettings s;
    const qint64 last = s.value(QStringLiteral("vod/ytdlpUpdated")).toLongLong();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - last < 7 * 24 * 3600) return;
    s.setValue(QStringLiteral("vod/ytdlpUpdated"), now);
    auto *p = new QProcess(this);
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= 0x08000000; });
#endif
    connect(p, &QProcess::finished, p, [p](int code) {
        qInfo().noquote() << "Trailers: yt-dlp update" << (code == 0 ? "done" : "skipped") << QString::fromUtf8(p->readAllStandardOutput()).trimmed().section(QLatin1Char('\n'), -1);
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, p, &QObject::deleteLater);
    p->start(m_tool, {QStringLiteral("-U")});
}
