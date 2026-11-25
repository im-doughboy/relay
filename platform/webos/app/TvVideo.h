#pragma once

// Relay.Video's MpvVideo on the TV: the same QML interface as the computer's
// MpvItem (src/player/MpvItem.h), backed by the TV's own player (TvPlayer).
// The picture is on the TV's video layer under Qt Quick's: while it plays,
// the item leaves its area of the screen see-through and the video window
// follows the item (full screen, or the guide's small picture).
//
// It follows Relay into the background and back: the TV's decoder is let go,
// and the same channel or movie plays again (a movie from where it was).
// Live channels that drop are reopened a few times, as mpv's player does.

#include <QElapsedTimer>
#include <QQuickItem>
#include <QTimer>
#include <QVariantList>

class TvVideo : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY reconnectingChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoInfoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoInfoChanged)
    Q_PROPERTY(double fps READ fps NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY videoInfoChanged)
    Q_PROPERTY(QString audioCodec READ audioCodec NOTIFY videoInfoChanged)
    Q_PROPERTY(bool vod READ vod NOTIFY vodChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(int timeshiftMb READ timeshiftMb WRITE setTimeshiftMb NOTIFY timeshiftMbChanged)
    Q_PROPERTY(double liveBehind READ liveBehind NOTIFY timeshiftChanged)
    Q_PROPERTY(double liveBuffer READ liveBuffer NOTIFY timeshiftChanged)
    Q_PROPERTY(QString pictureShape READ pictureShape WRITE setPictureShape NOTIFY pictureChanged)
    Q_PROPERTY(QString deinterlace READ deinterlace WRITE setDeinterlace NOTIFY pictureChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY pictureChanged)
    Q_PROPERTY(int audioDelayMs READ audioDelayMs WRITE setAudioDelayMs NOTIFY pictureChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QString preferredAudio READ preferredAudio WRITE setPreferredAudio NOTIFY pictureChanged)
    Q_PROPERTY(QString preferredSubtitles READ preferredSubtitles WRITE setPreferredSubtitles NOTIFY pictureChanged)
    Q_PROPERTY(QString defaultUserAgent MEMBER m_defaultUserAgent)

public:
    explicit TvVideo(QQuickItem *parent = nullptr);
    ~TvVideo() override;

    Q_INVOKABLE void play(const QString &url, const QString &userAgent = QString(),
                          const QString &referrer = QString());
    Q_INVOKABLE void playVod(const QString &url, double startSeconds = 0);
    Q_INVOKABLE void playTrailer(const QString &video, const QString &audio);
    Q_INVOKABLE void seek(double seconds, bool relative = true);
    Q_INVOKABLE QVariantList tracks(const QString &type) const;
    Q_INVOKABLE void setTrack(const QString &type, int id);
    Q_INVOKABLE void seekLive(double seconds);
    Q_INVOKABLE void goLive();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void toggleMute() { setMuted(!m_muted); }

    bool buffering() const { return m_buffering; }
    bool reconnecting() const { return m_reconnecting; }
    bool playing() const { return m_playing; }
    bool paused() const { return m_paused; }
    bool muted() const { return m_muted; }
    void setMuted(bool m);
    int volume() const { return m_volume; }
    void setVolume(int v);
    int videoWidth() const { return m_videoW; }
    int videoHeight() const { return m_videoH; }
    double fps() const { return m_fps; }
    QString videoCodec() const { return m_vcodec; }
    QString audioCodec() const { return m_acodec; }
    bool vod() const { return m_vod; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool seekable() const { return m_vod && m_duration > 0; }
    int timeshiftMb() const { return m_timeshiftMb; }
    void setTimeshiftMb(int mb);
    double liveBehind() const { return 0; }    // pausing live TV isn't on the TV yet
    double liveBuffer() const { return 0; }
    QString pictureShape() const { return m_shape; }
    void setPictureShape(const QString &shape);
    QString deinterlace() const { return m_deinterlace; }
    void setDeinterlace(const QString &mode);
    bool hardwareDecoding() const { return true; }   // the TV always decodes in hardware
    void setHardwareDecoding(bool) {}
    int audioDelayMs() const { return m_audioDelayMs; }
    void setAudioDelayMs(int ms);
    double speed() const { return m_speed; }
    void setSpeed(double speed);
    QString preferredAudio() const { return m_prefAudio; }
    void setPreferredAudio(const QString &code);
    QString preferredSubtitles() const { return m_prefSubs; }
    void setPreferredSubtitles(const QString &code);

signals:
    void bufferingChanged();
    void reconnectingChanged();
    void playingChanged();
    void pausedChanged();
    void mutedChanged();
    void volumeChanged();
    void videoInfoChanged();
    void playbackError(const QString &message);
    void hardwareDecodingFailed();
    void playbackStarted();
    void vodChanged();
    void positionChanged();
    void durationChanged();
    void seekableChanged();
    void finished();
    void timeshiftMbChanged();
    void timeshiftChanged();
    void pictureChanged();
    void speedChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    bool owns() const;
    void start(const QString &url, bool vod, double startSeconds);
    void open();                    // (re)loads m_url in the TV's player
    void release();                 // Relay in the background: let go of the TV's decoder
    void resume();                  // back in front: the same channel or movie again
    void reconnect();
    void trackGeometry();           // the video window follows the item on screen
    void setShowVideo(bool on);
    void setVod(bool v);
    void setBuffering(bool b);
    void setPlaying(bool p);
    void setPaused(bool p);
    void setReconnecting(bool r);
    void resetInfo();
    void applyVolume();

    // From the TV's player.
    void onPlaying();
    void onFailed(const QString &message);
    void onEnded();

    bool m_showVideo = false;
    bool m_buffering = false;
    bool m_playing = false;
    bool m_paused = false;
    bool m_muted = false;
    bool m_reconnecting = false;
    int m_volume = 100;
    int m_videoW = 0, m_videoH = 0;
    double m_fps = 0;
    QString m_vcodec, m_acodec;
    bool m_vod = false;
    double m_position = 0, m_duration = 0;
    double m_startSeconds = 0;      // where a movie starts when it opens
    int m_timeshiftMb = 0;
    QString m_shape = QStringLiteral("auto");
    QString m_deinterlace = QStringLiteral("auto");
    int m_audioDelayMs = 0;
    double m_speed = 1;
    QString m_prefAudio, m_prefSubs;
    QString m_defaultUserAgent;
    QString m_url, m_userAgent;

    bool m_released = false;
    bool m_everStarted = false;
    int m_attempt = 0;
    QTimer m_reconnectTimer;
    QTimer m_openTimer;             // quick channel changes: start the last one only
    QElapsedTimer m_lastStart;
    QRect m_lastRect;
};

// Relay's main.cpp registers MpvItem as Relay.Video's MpvVideo.
using MpvItem = TvVideo;
