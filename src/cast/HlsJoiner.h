#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpSocket;

// Live HLS -> one continuous MPEG-TS stream, for TVs that play TS but not HLS
// (Samsung). Follows the provider's playlist and sends each new segment to
// the TV in order, as it would arrive from a channel that streams TS itself.
class HlsJoiner : public QObject
{
    Q_OBJECT

public:
    struct Playlist {
        bool master = false;
        QList<QPair<QUrl, qint64>> variants;   // master: (address, bandwidth)
        qint64 sequence = 0;                   // number of the first segment
        QList<QUrl> segments;
        double targetDuration = 6;
        bool ended = false;                    // #EXT-X-ENDLIST
        bool encrypted = false;                // #EXT-X-KEY with a method other than NONE
        bool fmp4 = false;                     // #EXT-X-MAP (not MPEG-TS segments)
    };
    static Playlist parse(const QByteArray &text, const QUrl &base);
    // The variant to follow: the highest bandwidth (the TV has the screen for it).
    static QUrl pickVariant(const Playlist &master);

    HlsJoiner(QNetworkAccessManager *net, const QUrl &url, const QString &userAgent, const QString &referrer,
              QTcpSocket *socket, QObject *parent = nullptr);
    void start();

private:
    void fetchPlaylist();
    void onPlaylist(const QByteArray &body, const QUrl &url);
    void nextSegment();
    void schedulePlaylist(double seconds);
    void stop(const QString &why);
    QNetworkReply *get(const QUrl &url);

    QNetworkAccessManager *m_net;
    QUrl m_url;
    QString m_userAgent, m_referrer;
    QPointer<QTcpSocket> m_socket;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timer;
    qint64 m_next = -1;            // number of the next segment to send
    QList<QPair<qint64, QUrl>> m_queue;
    double m_target = 6;
    int m_failures = 0;
    int m_masterHops = 0;
    qint64 m_sent = 0;
    bool m_stopped = false;
};
