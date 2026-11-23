#pragma once

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;
class QTcpSocket;
class TsSegmenter;

// A small web server on this computer that a TV or Chromecast plays from.
// The TV asks Relay; Relay fetches from the provider (with the channel's user
// agent and referrer, from this computer's address, as one connection) and
// passes the bytes on. HLS playlists are rewritten so their parts come
// through Relay too. For receivers that only play HLS, a live MPEG-TS stream
// can be served as HLS instead (TsSegmenter).
//
// Addresses carry a random token and only work while that stream is being
// cast.
class CastRelay : public QObject
{
    Q_OBJECT

public:
    explicit CastRelay(QObject *parent = nullptr);
    ~CastRelay() override;

    struct Source {
        QString url;
        QString userAgent, referrer;
        bool asHls = false;        // live MPEG-TS -> HLS (Chromecast)
        QString contentType;       // what to say it is, when the provider doesn't
        bool forceType = false;    // say contentType even when the provider says something (DLNA TVs)
        QString dlnaFeatures;      // contentFeatures.dlna.org (DLNA TVs ask)
    };
    // The path (on this server) the receiver should play; starts the server.
    QString add(const Source &source);
    void remove(const QString &path);
    void clear();
    // http://<this computer's address facing `peer`>:<port><path>.
    QString urlFor(const QString &path, const QHostAddress &peer) const;
    static QHostAddress localAddressFacing(const QHostAddress &peer);
    quint16 port() const;

    // HLS playlist lines pointing elsewhere -> through the relay. `register_`
    // gets each absolute address and returns the path to use instead.
    static QByteArray rewritePlaylist(const QByteArray &playlist, const QUrl &base,
                                      const std::function<QString(const QString &)> &register_);

private:
    struct Entry {
        Source source;
        QStringList parts;                       // addresses found in playlists, by index
        std::shared_ptr<TsSegmenter> segmenter;
        QPointer<QNetworkReply> feed;            // the one provider connection for HLS output
        int feedFailures = 0;
    };
    struct Waiting { QPointer<QTcpSocket> socket; QString token; qint64 since; };

    bool ensureListening();
    void onConnection();
    void onRequest(QTcpSocket *socket, const QByteArray &head);
    void pipe(QTcpSocket *socket, const QString &token, const QString &url, const QByteArray &method,
              const QHash<QByteArray, QByteArray> &headers);
    void startFeed(const QString &token);
    void answerWaiting();
    void reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
               const QByteArray &method = "GET");

    QTcpServer *m_server;
    QNetworkAccessManager *m_net;
    QHash<QString, Entry> m_entries;   // token -> stream
    QList<Waiting> m_waiting;          // playlist requests before there are segments
    QTimer m_waitTimer;
};
