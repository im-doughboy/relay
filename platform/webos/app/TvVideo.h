#pragma once

// Relay.Video's MpvVideo on the TV: the same QML interface as the computer's
// MpvItem (src/player/MpvItem.h), backed by the TV's own player. The picture is
// on the TV's video layer under Qt Quick's; while something plays the item
// leaves its area of the screen see-through, so the video shows there.
//
// Playback isn't connected yet: this first version keeps the interface so
// Relay's screens load and work, and reports what it was asked to play. It
// already follows Relay into the background and back (release, resume).

#include <QQuickItem>
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
    bool reconnecting() const { return false; }
    bool playing() const { return m_playing; }
    bool paused() const { return m_paused; }
    bool muted() const { return m_muted; }
    void setMuted(bool m);
    int volume() const { return m_volume; }
    void setVolume(int v);
    int videoWidth() const { return m_videoW; }
    int videoHeight() const { return m_videoH; }
    double fps() const { return 0; }
    QString videoCodec() const { return QString(); }
    QString audioCodec() const { return QString(); }
    bool vod() const { return m_vod; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    bool seekable() const { return m_vod; }
    int timeshiftMb() const { return m_timeshiftMb; }
    void setTimeshiftMb(int mb);
    double liveBehind() const { return 0; }
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

private:
    void setShowVideo(bool on);
    void setVod(bool v);
    void start(const QString &url, bool vod, double startSeconds);
    void release();   // Relay in the background: let go of the TV's decoder
    void resume();    // back in front: the same channel or movie again

    bool m_released = false;
    double m_resumePosition = 0;

    bool m_showVideo = false;   // the item is see-through to the TV's video layer
    bool m_buffering = false;
    bool m_playing = false;
    bool m_paused = false;
    bool m_muted = false;
    int m_volume = 100;
    int m_videoW = 0, m_videoH = 0;
    bool m_vod = false;
    double m_position = 0, m_duration = 0;
    int m_timeshiftMb = 0;
    QString m_shape = QStringLiteral("auto");
    QString m_deinterlace = QStringLiteral("auto");
    int m_audioDelayMs = 0;
    double m_speed = 1;
    QString m_prefAudio, m_prefSubs;
    QString m_defaultUserAgent;
    QString m_url, m_userAgent;
};

// Relay's main.cpp registers MpvItem as Relay.Video's MpvVideo.
using MpvItem = TvVideo;
