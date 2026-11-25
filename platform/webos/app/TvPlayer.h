#pragma once

// The TV's own player (luna://com.webos.media), as the spike proved it: the
// stream goes through Relay's stream relay (src/cast/CastRelay, on 127.0.0.1
// only) and plays in an exported video window under Relay's screens.
//
// One for the whole app: the TV gives an app one video decoder. A TvVideo
// item takes it with load() and is its owner until another one loads or it
// unloads. Replies arrive on the bus's thread and are handled on Qt's.

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>

class CastRelay;

class TvPlayer : public QObject
{
    Q_OBJECT

public:
    static TvPlayer *instance();

    // Starts `url` for `owner`, letting go of whatever played before.
    // `direct`: the TV's player fetches it itself (a movie or episode file,
    // read as it plays); otherwise through Relay's stream relay (live TV,
    // where one shared connection to the provider is what works).
    void load(QObject *owner, const QString &url, const QString &userAgent, bool vod, bool direct);
    void unload();
    QObject *owner() const { return m_owner; }

    void play();
    void pause();
    void seek(double seconds);
    void setVolume(int percent);     // 0-100; 0 mutes
    // Where the picture goes, in Relay's 1920x1080 screen.
    void setVideoRect(const QRect &rect);

signals:
    // All for the owner (check owner() == this in the receiver).
    void loaded();
    void playing();
    void paused();
    void buffering(bool on);
    void ended();
    void failed(const QString &message);
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void videoInfo(int width, int height, double fps, const QString &videoCodec, const QString &audioCodec);

private:
    TvPlayer();
    bool ensureWindow();
    void call(const char *method, const QJsonObject &args);
    // The bus's replies, on Qt's thread (`generation` tells old ones apart).
    Q_INVOKABLE void handleLoadReply(int generation, const QByteArray &payload);
    Q_INVOKABLE void handleEvent(int generation, const QByteArray &payload);
    void stopRelay();

    QPointer<QObject> m_owner;
    CastRelay *m_relay = nullptr;
    QString m_relayPath;
    QString m_windowId;
    QRect m_videoRect{0, 0, 1920, 1080};
    QString m_mediaId;
    unsigned long m_subscription = 0;
    int m_generation = 0;
    bool m_playSent = false;
    bool m_vod = false;
    int m_volume = 100;
    QStringList m_seenEvents;   // each kind of event in the log once
    QElapsedTimer m_loadClock;  // load -> playing, in the log
};
