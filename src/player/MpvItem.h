#pragma once

#include <QElapsedTimer>
#include <QMutex>
#include <QTimer>
#include <QQuickFramebufferObject>
#include <QVariantList>
#include <memory>

struct mpv_handle;
struct mpv_render_context;
class MpvItem;

// Shared between the GUI-thread item and the render-thread renderer so the
// mpv handle outlives whichever of the two is destroyed last.
struct MpvCore
{
    mpv_handle *mpv = nullptr;
    QMutex itemLock;
    MpvItem *item = nullptr;   // cleared when the item is destroyed
    ~MpvCore();
};

// A Qt Quick item that renders libmpv video through mpv's render API.
// Requires the OpenGL scene graph backend (set in main.cpp).
class MpvItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    // True while the player is reconnecting a stream that dropped.
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
    // Movies and episodes (playVod): seconds into the video and its length.
    Q_PROPERTY(bool vod READ vod NOTIFY vodChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    // Pause live TV: how much of the live channel is kept (MB on disk, 0 =
    // off), how far behind live the picture is, and how much can be rewound.
    Q_PROPERTY(int timeshiftMb READ timeshiftMb WRITE setTimeshiftMb NOTIFY timeshiftMbChanged)
    Q_PROPERTY(double liveBehind READ liveBehind NOTIFY timeshiftChanged)
    Q_PROPERTY(double liveBuffer READ liveBuffer NOTIFY timeshiftChanged)
    // Picture and sound. pictureShape: "auto", "16:9", "4:3", "fill" (crop to
    // fill) or "stretch"; deinterlace: "auto", "on" or "off"; audioDelayMs
    // moves the sound later (+) or earlier (−); speed applies to movies only.
    Q_PROPERTY(QString pictureShape READ pictureShape WRITE setPictureShape NOTIFY pictureChanged)
    Q_PROPERTY(QString deinterlace READ deinterlace WRITE setDeinterlace NOTIFY pictureChanged)
    Q_PROPERTY(bool hardwareDecoding READ hardwareDecoding WRITE setHardwareDecoding NOTIFY pictureChanged)
    Q_PROPERTY(int audioDelayMs READ audioDelayMs WRITE setAudioDelayMs NOTIFY pictureChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    // Sent to the provider for streams that don't name their own (the
    // playlist's setting; empty = Relay's).
    Q_PROPERTY(QString defaultUserAgent MEMBER m_defaultUserAgent)

public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    Renderer *createRenderer() const override;

    Q_INVOKABLE void play(const QString &url, const QString &userAgent = QString(),
                          const QString &referrer = QString());
    // A movie or episode: seekable, starts at `startSeconds`, and finishing
    // it is not a dropped connection.
    Q_INVOKABLE void playVod(const QString &url, double startSeconds = 0);
    // Video and its own audio (a trailer from YouTube).
    Q_INVOKABLE void playTrailer(const QString &video, const QString &audio);
    Q_INVOKABLE void seek(double seconds, bool relative = true);
    // [{id, label, selected}] for "audio" or "sub"; setTrack(type, -1) turns
    // subtitles off.
    Q_INVOKABLE QVariantList tracks(const QString &type) const;
    Q_INVOKABLE void setTrack(const QString &type, int id);
    // Live TV: move within what's been kept, or catch up with live.
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
    bool seekable() const { return m_seekable; }
    int timeshiftMb() const { return m_timeshiftMb; }
    void setTimeshiftMb(int mb);
    double liveBehind() const { return m_liveBehind; }
    double liveBuffer() const { return m_liveBuffer; }

    QString pictureShape() const { return m_shape; }
    void setPictureShape(const QString &shape);
    QString deinterlace() const { return m_deinterlace; }
    void setDeinterlace(const QString &mode);
    bool hardwareDecoding() const { return m_hwdec; }
    void setHardwareDecoding(bool on);
    int audioDelayMs() const { return m_audioDelayMs; }
    void setAudioDelayMs(int ms);
    double speed() const { return m_speed; }
    void setSpeed(double speed);

    std::shared_ptr<MpvCore> core() const { return m_core; }

signals:
    void bufferingChanged();
    void reconnectingChanged();
    void playingChanged();
    void pausedChanged();
    void mutedChanged();
    void volumeChanged();
    void videoInfoChanged();
    void playbackError(const QString &message);
    // The graphics card's video decoder failed: every player has switched to
    // software decoding until Relay restarts (sent once, by the player it happened in).
    void hardwareDecodingFailed();
    void playbackStarted();
    void vodChanged();
    void positionChanged();
    void durationChanged();
    void seekableChanged();
    void finished();          // a movie or episode played to the end
    void timeshiftMbChanged();
    void timeshiftChanged();
    void pictureChanged();
    void speedChanged();

private slots:
    void processEvents();
    void onRenderContextReady();
    void reconnect();

private:
    void setBuffering(bool b);
    void setReconnecting(bool r);
    void load();
    void startFile(const QString &address);
    quint64 m_loadSerial = 0;                      // (re)opens m_url with the current options
    void handleStreamEnd(bool error);
    void setPlaying(bool p);
    void resetVideoInfo();
    void setVod(bool v);
    void resetPosition();
    void applyCacheOptions();
    void pollTimeshift();
    bool cachedRange(double *start, double *seekEnd, double *dataEnd, double *pos) const;
    void setTimeshift(double behind, double buffer);

    std::shared_ptr<MpvCore> m_core;
    bool m_buffering = false;
    bool m_playing = false;
    bool m_paused = false;
    bool m_muted = false;
    bool m_pausedForCache = false;
    bool m_loading = false;
    int m_volume = 100;
    int m_videoW = 0;
    int m_videoH = 0;
    double m_fps = 0;
    QString m_vcodec;
    QString m_acodec;
    QString m_shape = QStringLiteral("auto");
    QString m_deinterlace = QStringLiteral("auto");
    bool m_hwdec = true;
    QString hwdecMode() const;
    void hardwareDecoderFailed();
    int m_audioDelayMs = 0;
    double m_speed = 1;
    QString m_defaultUserAgent;
    QString m_lastError;   // last mpv error log line, shown with failures

    // What's playing, kept so a dropped stream can be reopened.
    QString m_url, m_userAgent, m_referrer;
    bool m_vod = false;
    // Live TV: captions start off; once turned on (C) they stay on across channels.
    bool m_wantCaptions = false;
    bool m_vodSubChosen = false;     // the subtitle choice was made in this movie
    void applyCaptionChoice();
    void logSubtitleTracks();
    double m_startPos = 0;            // where load() starts a movie
    double m_position = 0;
    double m_duration = 0;
    bool m_seekable = false;
    int m_timeshiftMb = 2048;
    double m_liveBehind = 0;
    double m_liveBuffer = 0;
    // Servers send live channels a little ahead (often several seconds when
    // a channel opens). While watching live that head start is measured, so
    // only time paused or rewound counts as "behind live".
    bool m_atLive = true;
    double m_liveSeekTarget = -1;   // where a live jump is heading until it lands
    QElapsedTimer m_liveSeekSince;
    double m_liveSlack = 0;
    QTimer m_timeshiftTimer;
    bool m_renderReady = false;       // until then, load() waits
    bool m_loadPending = false;

    // Automatic reconnection. Live streams often drop briefly; a channel
    // that played before gets several tries with growing pauses, one that
    // never started gets a single quick retry before the error shows.
    bool m_reconnecting = false;
    bool m_everStarted = false;
    int m_attempt = 0;
    QTimer m_reconnectTimer;
    QTimer m_stableTimer;             // resets m_attempt after steady playback
    QElapsedTimer m_playingFor;
    QString m_audioFile, m_audioFileFor;
};
