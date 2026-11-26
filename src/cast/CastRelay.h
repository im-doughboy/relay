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
class Remuxer;

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
        bool joinHls = false;      // an HLS channel sent on as one MPEG-TS stream (TVs without HLS)
        bool shareLive = false;    // a live MPEG-TS channel: one provider connection, shared by the TV's requests
        // A movie repackaged with one chosen sound track (Remuxer): as HLS when
        // asHls, otherwise as one MPEG-TS stream. Starts `remuxStart` seconds in.
        bool remux = false;
        int remuxAudio = -1;
        double remuxStart = 0;
    };
    // FFmpeg's libraries are there (the sound-track choice works).
    static bool canRemux();
    // The path (on this server) the receiver should play; starts the server.
    QString add(const Source &source);
    void remove(const QString &path);
    void clear();
    // http://<this computer's address facing `peer`>:<port><path>.
    QString urlFor(const QString &path, const QHostAddress &peer) const;
    static QHostAddress localAddressFacing(const QHostAddress &peer);
    quint16 port() const;
    // Where the server listens; set before the first add(). By default all
    // IPv4 interfaces, because TVs and Chromecasts connect over the network.
    void setListenAddress(const QHostAddress &address);
    // How long a provider gets to start answering before the relay asks
    // again (10 s; tests use less).
    void setAnswerTimeout(int ms) { m_answerTimeoutMs = ms; }

    // HLS playlist lines pointing elsewhere -> through the relay. `register_`
    // gets each absolute address and returns the path to use instead.
    static QByteArray rewritePlaylist(const QByteArray &playlist, const QUrl &base,
                                      const std::function<QString(const QString &)> &register_);

private:
    struct Entry {
        Source source;
        QStringList parts;                       // addresses found in playlists, by index
        std::shared_ptr<TsSegmenter> segmenter;
        QPointer<QNetworkReply> feed;            // the one provider connection (HLS output, shared live)
        int feedFailures = 0;
        QList<QPointer<QTcpSocket>> listeners;   // shared live: the TV's open requests
        int logged = 0;                          // requests written to the log
        // Remux: what's been made, how far the receiver has asked, whether it's done.
        std::shared_ptr<Remuxer> remuxer;
        QByteArray head;                         // the start, for a TV that asks again
        qint64 produced = 0;
        int lastAsked = -1;                      // newest HLS segment the receiver asked for
        bool remuxEnded = false;
    };
    struct Waiting { QPointer<QTcpSocket> socket; QString token; qint64 since; };

    bool ensureListening();
    void onConnection();
    void onRequest(QTcpSocket *socket, const QByteArray &head);
    void pipe(QTcpSocket *socket, const QString &token, const QString &url, const QByteArray &method,
              const QHash<QByteArray, QByteArray> &headers, int hops = 0, int retries = 0, QString origin = QString());
    void startFeed(const QString &token);
    void serveShared(QTcpSocket *socket, const QString &token, const QByteArray &method,
                     const QHash<QByteArray, QByteArray> &headers);
    void closeListeners(Entry &e);
    void serveJoined(QTcpSocket *socket, const Source &source, const QByteArray &method,
                     const QHash<QByteArray, QByteArray> &headers);
    void answerWaiting();
    void startRemux(const QString &token);
    void onRemuxData(const QString &token, const QByteArray &ts);
    void serveRemux(QTcpSocket *socket, const QString &token, const QByteArray &method,
                    const QHash<QByteArray, QByteArray> &headers);
    void updateHold(Entry &e);
    void reply(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body,
               const QByteArray &method = "GET", const QString &features = QString());

    QTcpServer *m_server;
    QNetworkAccessManager *m_net;
    int m_answerTimeoutMs = 10000;
    QHash<QString, Entry> m_entries;   // token -> stream
    QList<Waiting> m_waiting;          // playlist requests before there are segments
    QTimer m_waitTimer;
    QHostAddress m_listenAddress = QHostAddress::AnyIPv4;
};
