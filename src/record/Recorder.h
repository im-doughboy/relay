#pragma once

#include "HlsPlaylist.h"

#include <QFile>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// Saves one live channel to a file until stop() is called.
//
// MPEG-TS and other continuous streams are written as they arrive. HLS
// (.m3u8) channels are followed segment by segment, and the segments are
// appended to the same file. Dropped connections are retried every few
// seconds; whatever was received is kept.
class Recorder : public QObject
{
    Q_OBJECT

public:
    enum class State { Connecting, Recording, Retrying, Finished, Failed };

    Recorder(QNetworkAccessManager *net, const QUrl &url, const QString &userAgent,
             const QString &referrer, const QString &filePath, QObject *parent = nullptr);
    ~Recorder() override;

    void start();
    void stop();     // keeps the file; state becomes Finished (or Failed if nothing came)

    State state() const { return m_state; }
    qint64 bytes() const { return m_bytes; }
    QString error() const { return m_error; }
    QString filePath() const { return m_file.fileName(); }

signals:
    void changed();
    void stopped();

private:
    void connectStream();
    void retryLater(const QString &why);
    void setState(State s);
    void write(const QByteArray &data);
    QNetworkReply *get(const QUrl &url);

    // HLS
    void fetchPlaylist();
    void onPlaylist(const Hls::Playlist &p);
    void nextSegment();

    QNetworkAccessManager *m_net;
    QUrl m_url;
    QUrl m_mediaUrl;              // HLS media playlist (after picking a variant)
    QString m_userAgent, m_referrer;
    QFile m_file;
    QPointer<QNetworkReply> m_reply;
    QTimer m_retry;
    QTimer m_poll;
    State m_state = State::Connecting;
    qint64 m_bytes = 0;
    QString m_error;
    bool m_hls = false;
    bool m_stopping = false;
    QByteArray m_head;            // first bytes, to spot an HLS playlist
    QVector<Hls::Segment> m_queue;
    qint64 m_lastSequence = -1;
    int m_failures = 0;
};
