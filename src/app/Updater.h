#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

// Updates (QML: Updates). Relay asks GitHub for the newest release of its
// repository; when there's a newer version it offers it, downloads the
// installer, checks it, and runs it. The installer closes Relay, updates it
// in place (your data is kept) and opens it again.
//
// A private repository needs a GitHub token that can read it (Settings).
class Updater : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)          // newer than this one
    Q_PROPERTY(bool offered READ offered NOTIFY changed)              // available and not put off
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY changed)        // an installer for this system
    Q_PROPERTY(QString notes READ notes NOTIFY changed)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY changed)
    // "", checking, downloading, installing, error
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool autoCheck READ autoCheck WRITE setAutoCheck NOTIFY changed)
    Q_PROPERTY(bool hasToken READ hasToken NOTIFY changed)
    Q_PROPERTY(QString repository READ repository CONSTANT)

public:
    explicit Updater(QObject *parent = nullptr);

    QString currentVersion() const;
    QString latestVersion() const { return m_release.value(QStringLiteral("version")).toString(); }
    bool available() const;
    bool offered() const;
    bool canInstall() const { return !m_release.value(QStringLiteral("assetUrl")).toString().isEmpty(); }
    QString notes() const { return m_release.value(QStringLiteral("notes")).toString(); }
    QString releaseUrl() const { return m_release.value(QStringLiteral("page")).toString(); }
    QString state() const { return m_state; }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    bool autoCheck() const;
    void setAutoCheck(bool on);
    bool hasToken() const { return !token().isEmpty(); }
    QString repository() const;

    Q_INVOKABLE void check(bool byHand = true);
    // Downloads the installer, checks it, and runs it (Relay closes).
    Q_INVOKABLE void updateNow();
    Q_INVOKABLE void later();          // don't offer this version again
    Q_INVOKABLE void setToken(const QString &token);   // "" removes it

    // -1, 0, 1: "0.10.2" vs "v0.9" (numbers compared part by part).
    static int compareVersions(const QString &a, const QString &b);
    // A GitHub release (API answer) -> {version, page, notes, assetName,
    // assetUrl, assetSize, sha256}; the installer for `platform` ("windows",
    // "macos", "linux"); {} when it's a draft or has no version.
    static QVariantMap parseRelease(const QByteArray &json, const QString &platform);
    static QString platform();
    static QString apiBase();   // RELAY_UPDATE_URL overrides (tests)
    void setReleaseForTest(const QVariantMap &release) { m_release = release; emit changed(); }

signals:
    void changed();
    void message(const QString &text, bool problem);
    void downloaded(const QString &path);   // checked, about to be installed

private:
    QString token() const;
    void setState(const QString &state, const QString &status = QString());
    void download(const QUrl &url, bool withToken, int redirects);
    void finishDownload();
    void install();

    QNetworkAccessManager *m_net;
    QVariantMap m_release;
    QString m_state, m_status;
    double m_progress = 0;
    QTimer m_timer;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    QString m_path;
};
