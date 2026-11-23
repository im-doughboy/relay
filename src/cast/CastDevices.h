#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <functional>

class QNetworkAccessManager;
class QSslSocket;
class QUdpSocket;

// A screen Relay can cast to.
struct CastDevice {
    QString id;            // "cc:<host>:<port>" / "dlna:<control url>"
    QString name;
    QString model;
    QString kind;          // "chromecast" or "dlna"
    QHostAddress address;
    quint16 port = 0;      // Chromecast
    QUrl controlUrl;       // DLNA AVTransport
    QUrl connectionUrl;    // DLNA ConnectionManager (what the TV can play)
    QVariantMap toMap() const;
};

// Finds Chromecasts (mDNS, _googlecast._tcp) and smart TVs and other DLNA
// renderers (SSDP, AVTransport) on the local network.
class CastDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit CastDiscovery(QObject *parent = nullptr);
    void scan();
    // A device the scan can't see: an IP address (Chromecast) or a DLNA
    // description address (http://…/description.xml).
    void addByAddress(const QString &text);
    // Takes a device off the list (and out of the remembered addresses).
    void forget(const QString &id);
    const QHash<QString, CastDevice> &devices() const { return m_devices; }

    // ---- parsing (static, for tests) ----
    static QByteArray mdnsQuery(const QByteArray &service);
    // Chromecasts in an mDNS answer: [{name, model, host, port, address}].
    static QList<CastDevice> parseMdns(const QByteArray &packet, const QHostAddress &from);
    static QByteArray ssdpSearch();
    // LOCATION: from an SSDP answer.
    static QUrl ssdpLocation(const QByteArray &answer);
    // A UPnP device description: name, model and the AVTransport control
    // address (resolved against `location`). Invalid controlUrl when it has none.
    static CastDevice parseDescription(const QByteArray &xml, const QUrl &location);

signals:
    void devicesChanged();
    void message(const QString &text);

private:
    void readMdns(QUdpSocket *socket);
    void readSsdp();
    void fetchDescription(const QUrl &location);
    void addDevice(const CastDevice &d);

    QNetworkAccessManager *m_net;
    QUdpSocket *m_mdnsListen = nullptr, *m_mdnsQuery = nullptr, *m_ssdp = nullptr;
    QHash<QString, CastDevice> m_devices;
    QSet<QString> m_described;
};

// Plays something on a DLNA renderer (SetAVTransportURI, Play, Pause, Stop).
class DlnaRenderer : public QObject
{
    Q_OBJECT

public:
    DlnaRenderer(const CastDevice &device, QObject *parent = nullptr);
    // Asks the TV which types it plays (ConnectionManager GetProtocolInfo,
    // once) and gives the name it uses for `mime` (Samsung: MPEG-TS is
    // video/vnd.dlna.mpeg-tts, not video/mp2t); `mime` when it doesn't say.
    void acceptedType(const QString &mime, std::function<void(const QString &)> done);
    void load(const QString &url, const QString &title, const QString &mime, bool live);
    void play();
    void pause();
    void stop();
    void seek(double secs);
    // Stops the TV, then deletes itself once the TV has answered.
    void stopAndDelete();
    static QByteArray didl(const QString &url, const QString &title, const QString &mime, bool live);
    static QByteArray soapBody(const QString &action, const QString &args, const char *service = nullptr);
    // DLNA.ORG_OP/CI/FLAGS: a live stream can't be sought; a file can (bytes).
    static QString features(bool live);
    // From the TV's Sink list ("http-get:*:video/mpeg:*,..."): what it calls `mime`.
    static QString pickType(const QString &sink, const QString &mime);
    QString sink() const { return m_sink; }
    // The TV lists this type (under any of its names).
    bool plays(const QString &mime) const { return !m_sink.isEmpty() && m_sink.contains(QLatin1Char(':') + pickType(m_sink, mime) + QLatin1Char(':')); }

signals:
    void playing();
    void failed(const QString &message);
    void stopped();

private:
    void call(const QString &action, const QString &args, std::function<void(bool, const QByteArray &)> done = {},
              const QUrl &url = QUrl(), const char *service = nullptr);
    void pollState();
    CastDevice m_device;
    QNetworkAccessManager *m_net;
    QTimer m_poll;
    bool m_started = false;
    double m_seekOnStart = 0;
    int m_stoppedPolls = 0;
    QString m_lastState;
    bool m_askedTypes = false;
    QString m_sink;   // what the TV said it plays
public:
    void seekWhenPlaying(double secs) { m_seekOnStart = secs; }
};

// Plays something on a Chromecast: the Cast protocol (protobuf messages over
// TLS, port 8009) with Google's Default Media Receiver.
class ChromecastClient : public QObject
{
    Q_OBJECT

public:
    ChromecastClient(const CastDevice &device, QObject *parent = nullptr);
    ~ChromecastClient() override;
    void load(const QString &url, const QString &title, const QString &contentType, bool live, double startSecs);
    void play();
    void pause();
    void stop();
    // Stops the Chromecast, then deletes itself once that has gone out.
    void stopAndDelete();

    // One CastMessage (protobuf) with its 4-byte length in front, and back.
    static QByteArray frame(const QString &source, const QString &destination, const QString &ns, const QByteArray &json);
    struct Message { QString source, destination, ns; QByteArray payload; };
    // Takes whole messages off the front of `buffer`.
    static QList<Message> unframe(QByteArray &buffer);

signals:
    void playing();
    void failed(const QString &message);
    void stopped();

private:
    void send(const QString &destination, const QString &ns, const QJsonObject &payload);
    void onMessage(const Message &m);
    void connectTo(const QString &destination);

    CastDevice m_device;
    QSslSocket *m_socket;
    QByteArray m_buffer;
    QTimer m_heartbeat;
    int m_requestId = 1;
    QString m_transport, m_session;
    int m_mediaSession = -1;
    QJsonObject m_pendingLoad;
    bool m_sawPlaying = false;
    QString m_lastState;       // for the log: what the Chromecast says it's doing
    bool m_answered = false;   // it spoke Cast at least once
};
