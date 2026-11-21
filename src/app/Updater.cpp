#include "Updater.h"

#include "SecretStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QtDebug>

#ifndef RELAY_UPDATE_REPO
#define RELAY_UPDATE_REPO "im-doughboy/relay"
#endif

namespace {
constexpr int kFirstCheckMs = 45 * 1000;          // after start, once things have settled
constexpr int kEveryMs = 24 * 3600 * 1000;
const char kTokenKey[] = "github-token";
} // namespace

Updater::Updater(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (autoCheck()) check(false);
        m_timer.start(kEveryMs);
    });
    m_timer.start(kFirstCheckMs);
}

QString Updater::currentVersion() const { return QCoreApplication::applicationVersion(); }

QString Updater::repository() const
{
    const QString env = qEnvironmentVariable("RELAY_UPDATE_REPO");
    return env.isEmpty() ? QStringLiteral(RELAY_UPDATE_REPO) : env;
}

QString Updater::apiBase()
{
    const QString env = qEnvironmentVariable("RELAY_UPDATE_URL");
    return env.isEmpty() ? QStringLiteral("https://api.github.com") : env;
}

QString Updater::platform()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

bool Updater::available() const
{
    return !latestVersion().isEmpty() && compareVersions(latestVersion(), currentVersion()) > 0;
}

bool Updater::offered() const
{
    return available() && QSettings().value(QStringLiteral("updates/skipped")).toString() != latestVersion();
}

bool Updater::autoCheck() const { return QSettings().value(QStringLiteral("updates/auto"), true).toBool(); }

void Updater::setAutoCheck(bool on)
{
    QSettings().setValue(QStringLiteral("updates/auto"), on);
    emit changed();
}

QString Updater::token() const
{
    QSettings s;
    if (s.value(QStringLiteral("updates/tokenSecure")).toBool()) return SecretStore::read(QLatin1String(kTokenKey));
    return s.value(QStringLiteral("updates/token")).toString();
}

void Updater::setToken(const QString &tokenIn)
{
    const QString t = tokenIn.trimmed();
    QSettings s;
    SecretStore::remove(QLatin1String(kTokenKey));
    s.remove(QStringLiteral("updates/token"));
    s.remove(QStringLiteral("updates/tokenSecure"));
    if (!t.isEmpty()) {
        if (SecretStore::available() && SecretStore::write(QLatin1String(kTokenKey), t)) s.setValue(QStringLiteral("updates/tokenSecure"), true);
        else s.setValue(QStringLiteral("updates/token"), t);
    }
    emit changed();
    if (!t.isEmpty()) check(true);
}

void Updater::setState(const QString &state, const QString &status)
{
    m_state = state;
    m_status = status;
    emit changed();
}

// ---- versions and releases ----------------------------------------------------------------

int Updater::compareVersions(const QString &a, const QString &b)
{
    auto parts = [](QString v) {
        v = v.trimmed();
        if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) v.remove(0, 1);
        v = v.section(QLatin1Char('-'), 0, 0);   // "1.2.0-beta" compares as 1.2.0
        QList<int> out;
        for (const QString &p : v.split(QLatin1Char('.'))) out << p.toInt();
        while (!out.isEmpty() && out.last() == 0) out.removeLast();
        return out;
    };
    const QList<int> x = parts(a), y = parts(b);
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int p = x.value(i), q = y.value(i);
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

QVariantMap Updater::parseRelease(const QByteArray &json, const QString &platformName)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.value(QLatin1String("draft")).toBool() || o.value(QLatin1String("prerelease")).toBool()) return {};
    QString version = o.value(QLatin1String("tag_name")).toString();
    if (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) version.remove(0, 1);
    static const QRegularExpression looksLikeVersion(QStringLiteral("^\\d+(\\.\\d+)*$"));
    if (!looksLikeVersion.match(version).hasMatch()) return {};
    QVariantMap out{
        {QStringLiteral("version"), version},
        {QStringLiteral("page"), o.value(QLatin1String("html_url")).toString()},
        {QStringLiteral("notes"), o.value(QLatin1String("body")).toString()},
    };
    // The installer for this system.
    const QString wanted = platformName == QLatin1String("windows") ? QStringLiteral("-setup.exe")
                         : platformName == QLatin1String("macos") ? QStringLiteral(".dmg")
                         : QString();
    if (wanted.isEmpty()) return out;
    for (const QJsonValue &v : o.value(QLatin1String("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value(QLatin1String("name")).toString();
        if (!name.endsWith(wanted, Qt::CaseInsensitive)) continue;
        out.insert(QStringLiteral("assetName"), name);
        // The API address works for private repositories too (with a token).
        out.insert(QStringLiteral("assetUrl"), a.value(QLatin1String("url")).toString());
        out.insert(QStringLiteral("assetSize"), qint64(a.value(QLatin1String("size")).toDouble()));
        const QString digest = a.value(QLatin1String("digest")).toString();
        if (digest.startsWith(QLatin1String("sha256:"))) out.insert(QStringLiteral("sha256"), digest.mid(7).toLower());
        break;
    }
    return out;
}

// ---- checking ----------------------------------------------------------------------------------

void Updater::check(bool byHand)
{
    if (m_state == QLatin1String("downloading") || m_state == QLatin1String("installing") || m_state == QLatin1String("checking")) return;
    setState(QStringLiteral("checking"), tr("Checking for updates…"));
    QNetworkRequest req(QUrl(apiBase() + QStringLiteral("/repos/") + repository() + QStringLiteral("/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setRawHeader("User-Agent", "Relay/" + currentVersion().toUtf8());
    const QString t = token();
    if (!t.isEmpty()) req.setRawHeader("Authorization", "Bearer " + t.toUtf8());
    req.setTransferTimeout(20000);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, byHand, hadToken = !t.isEmpty()] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QSettings().setValue(QStringLiteral("updates/lastCheck"), QDateTime::currentSecsSinceEpoch());
        if (http != 200) {
            // A private repository looks like a missing one without a token.
            const QString problem = http == 404 && !hadToken ? tr("Couldn't see Relay's releases. While the repository is private, Relay needs a GitHub token (below).")
                                  : http == 404 ? tr("No releases yet, or the token can't read the repository.")
                                  : http == 401 ? tr("GitHub didn't accept the token.")
                                  : http == 0 ? tr("Couldn't reach GitHub. Check the internet connection.")
                                              : tr("GitHub answered with HTTP %1.").arg(http);
            qInfo().noquote() << "Updates: check failed:" << (http ? QString::number(http) : reply->errorString());
            setState(byHand ? QStringLiteral("error") : QString(), byHand ? problem : QString());
            return;
        }
        const QVariantMap release = parseRelease(reply->readAll(), platform());
        if (release.isEmpty()) { setState(QString(), byHand ? tr("No release found.") : QString()); return; }
        m_release = release;
        const bool newer = available();
        qInfo().noquote() << QStringLiteral("Updates: newest release %1 (this is %2)").arg(latestVersion(), currentVersion());
        setState(QString(), newer ? tr("Relay %1 is available.").arg(latestVersion())
                                  : tr("Relay is up to date (%1).").arg(currentVersion()));
        if (byHand && !newer) emit message(tr("Relay is up to date"), false);
    });
}

void Updater::later()
{
    QSettings().setValue(QStringLiteral("updates/skipped"), latestVersion());
    emit changed();
}

// ---- downloading and installing ----------------------------------------------------------------

void Updater::updateNow()
{
    if (!available()) return;
    if (!canInstall()) {
        // No installer for this system: the release page has what there is.
        emit message(tr("Opening the release page"), false);
        QMetaObject::invokeMethod(this, [url = releaseUrl()] {
            QProcess::startDetached(
#if defined(Q_OS_MACOS)
                QStringLiteral("open"),
#else
                QStringLiteral("xdg-open"),
#endif
                {url});
        }, Qt::QueuedConnection);
        return;
    }
    if (m_state == QLatin1String("downloading") || m_state == QLatin1String("installing")) return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/relay-update");
    QDir().mkpath(dir);
    m_path = dir + QLatin1Char('/') + QFileInfo(m_release.value(QStringLiteral("assetName")).toString()).fileName();
    QFile::remove(m_path);
    delete m_file;
    m_file = new QFile(m_path, this);
    if (!m_file->open(QIODevice::WriteOnly)) {
        setState(QStringLiteral("error"), tr("Couldn't save the update: %1").arg(m_file->errorString()));
        return;
    }
    m_progress = 0;
    setState(QStringLiteral("downloading"), tr("Downloading Relay %1…").arg(latestVersion()));
    qInfo().noquote() << "Updates: downloading" << m_release.value(QStringLiteral("assetName")).toString();
    download(QUrl(m_release.value(QStringLiteral("assetUrl")).toString()), true, 0);
}

void Updater::download(const QUrl &url, bool withToken, int redirects)
{
    QNetworkRequest req(url);
    req.setRawHeader("Accept", "application/octet-stream");
    req.setRawHeader("User-Agent", "Relay/" + currentVersion().toUtf8());
    // The token only goes to GitHub's API, not to where it sends the file.
    const QString t = token();
    if (withToken && !t.isEmpty()) req.setRawHeader("Authorization", "Bearer " + t.toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(60000);
    QNetworkReply *reply = m_net->get(req);
    m_reply = reply;
    const qint64 expected = m_release.value(QStringLiteral("assetSize")).toLongLong();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http == 200 && m_file) m_file->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, expected](qint64 got, qint64 total) {
        const qint64 all = total > 0 ? total : expected;
        if (all > 0) { m_progress = double(got) / double(all); emit changed(); }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, redirects] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (http >= 300 && http < 400 && redirects < 5) {
            const QUrl next = reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
            const bool sameApi = next.host() == QUrl(apiBase()).host();
            download(next, sameApi, redirects + 1);
            return;
        }
        if (http != 200) {
            if (m_file) { m_file->close(); QFile::remove(m_path); }
            qWarning().noquote() << "Updates: download failed:" << (http ? QString::number(http) : reply->errorString());
            setState(QStringLiteral("error"), tr("The download failed (%1). Try again later.")
                                                   .arg(http ? QStringLiteral("HTTP %1").arg(http) : reply->errorString()));
            return;
        }
        if (m_file) m_file->write(reply->readAll());
        finishDownload();
    });
}

void Updater::finishDownload()
{
    m_file->close();
    // Make sure it's the file GitHub listed, whole and unchanged.
    const qint64 expected = m_release.value(QStringLiteral("assetSize")).toLongLong();
    const QString sha = m_release.value(QStringLiteral("sha256")).toString();
    QString problem;
    if (expected > 0 && m_file->size() != expected) problem = tr("The download was incomplete.");
    if (problem.isEmpty() && !sha.isEmpty()) {
        QFile f(m_path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!f.open(QIODevice::ReadOnly) || !hash.addData(&f) || QString::fromLatin1(hash.result().toHex()) != sha)
            problem = tr("The download didn't match the release (checksum).");
    }
    if (!problem.isEmpty()) {
        QFile::remove(m_path);
        qWarning().noquote() << "Updates:" << problem;
        setState(QStringLiteral("error"), problem);
        return;
    }
    qInfo().noquote() << "Updates: downloaded and checked" << m_path << (sha.isEmpty() ? "(size)" : "(size and SHA-256)");
    install();
}

void Updater::install()
{
    setState(QStringLiteral("installing"), tr("Installing Relay %1 · Relay will open again in a moment").arg(latestVersion()));
    emit downloaded(m_path);
    if (qEnvironmentVariableIsSet("RELAY_UPDATE_NO_INSTALL")) return;   // tests
#if defined(Q_OS_WIN)
    // Quietly, then open Relay again (relay.nsi: /S /RELAUNCH).
    const bool ok = QProcess::startDetached(QDir::toNativeSeparators(m_path), {QStringLiteral("/S"), QStringLiteral("/RELAUNCH")});
#else
    const bool ok = QProcess::startDetached(
# if defined(Q_OS_MACOS)
        QStringLiteral("open"),
# else
        QStringLiteral("xdg-open"),
# endif
        {m_path});
#endif
    if (!ok) {
        setState(QStringLiteral("error"), tr("Couldn't start the installer. It's in %1").arg(QDir::toNativeSeparators(m_path)));
        return;
    }
    qInfo().noquote() << "Updates: installer started; closing";
#if defined(Q_OS_WIN)
    QTimer::singleShot(800, qApp, &QCoreApplication::quit);
#endif
}
