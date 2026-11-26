#pragma once

#include "Channel.h"
#include "XtreamParser.h"

#include <QObject>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// Downloads and parses playlists off the UI thread.
// Emits loaded() with the channels, or failed() with a human-readable reason.
class PlaylistService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit PlaylistService(QObject *parent = nullptr);

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }

    // Accepts an http(s) URL, a file:// URL, or a plain local path.
    Q_INVOKABLE void loadM3u(const QString &location);
    // With output "portal" this is a Stalker / MAC portal: `server` is the
    // portal address and `username` the MAC address (see PortalClient).
    Q_INVOKABLE void loadXtream(const QString &server, const QString &username,
                                const QString &password, const QString &output = QStringLiteral("ts"));
    void loadPortal(const QString &server, const QString &mac);
    // The channels saved after the last load (SavedPlaylist), for a quick
    // start; `login` is the server's for movies and series, as a load sets
    // it. `none` runs instead when there are none that can be used.
    void loadSaved(const QString &path, const QByteArray &source, const QStringList &secrets,
                   const XtreamParser::Credentials &login, std::function<void()> none);
    Q_INVOKABLE void cancel();

signals:
    void loaded(const PlaylistResult &result);
    void failed(const QString &message);
    void busyChanged();
    void statusChanged();

private:
    void begin(const QString &status);
    void setStatus(const QString &status);
    void finishWith(const PlaylistResult &result);
    void fail(const QString &message);
    QNetworkReply *get(const QUrl &url);
    static QString describeError(QNetworkReply *reply);

    QNetworkAccessManager *m_net;
    bool m_busy = false;
    QString m_status;
    quint64 m_generation = 0;   // ignores results from superseded loads
};
