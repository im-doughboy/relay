#include "MpvItem.h"

#include "sources/PortalClient.h"

#include <QSet>

#include "app/Log.h"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLExtraFunctions>
#include <QQuickOpenGLUtils>
#include <QDir>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QtDebug>

#include <iterator>
#include <utility>

#include <mpv/client.h>
#include <mpv/render_gl.h>

namespace {

enum ObservedProp : uint64_t {
    PropPause = 1,
    PropPausedForCache,
    PropMute,
    PropVolume,
    PropVideoW,
    PropVideoH,
    PropFps,
    PropVideoFormat,
    PropAudioCodec,
    PropTimePos,
    PropDuration,
    PropSeekable,
};

void *getProcAddress(void *, const char *name)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    return ctx ? reinterpret_cast<void *>(ctx->getProcAddress(QByteArray(name))) : nullptr;
}

// Both callbacks arrive on mpv's threads; hop to the GUI thread safely.
void onMpvWakeup(void *ctx)
{
    auto *core = static_cast<MpvCore *>(ctx);
    QMutexLocker lock(&core->itemLock);
    if (core->item) QMetaObject::invokeMethod(core->item, "processEvents", Qt::QueuedConnection);
}

void onRenderUpdate(void *ctx)
{
    auto *core = static_cast<MpvCore *>(ctx);
    QMutexLocker lock(&core->itemLock);
    if (core->item) QMetaObject::invokeMethod(core->item, "update", Qt::QueuedConnection);
}

void setOpt(mpv_handle *mpv, const char *name, const char *value)
{
    if (mpv_set_option_string(mpv, name, value) < 0)
        qWarning() << "mpv: could not set option" << name;
}

} // namespace

MpvCore::~MpvCore()
{
    if (mpv) mpv_terminate_destroy(mpv);
}

// ---- Renderer (render thread) -----------------------------------------------------

class MpvRenderer : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(std::shared_ptr<MpvCore> core) : m_core(std::move(core)) {}

    ~MpvRenderer() override
    {
        // Runs on the render thread with the GL context current, as mpv requires.
        if (m_ctx) mpv_render_context_free(m_ctx);
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        if (!m_ctx) {
            // Software OpenGL (VMs, remote desktops, missing GPU drivers)
            // intermittently renders mpv's default float intermediate buffers
            // black; 8-bit buffers are reliable there. Real GPUs keep the default.
            const QByteArray renderer = reinterpret_cast<const char *>(
                QOpenGLContext::currentContext()->functions()->glGetString(GL_RENDERER));
            qInfo().noquote() << "OpenGL renderer:" << renderer;
            if (renderer.contains("llvmpipe") || renderer.contains("softpipe")
                || renderer.contains("SwiftShader") || renderer.contains("Basic Render")) {
                qInfo().noquote() << "Software OpenGL detected (" + renderer + "), using 8-bit video buffers";
                mpv_set_property_string(m_core->mpv, "fbo-format", "rgba8");
            }

            mpv_opengl_init_params gl{getProcAddress, nullptr};
            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            if (mpv_render_context_create(&m_ctx, m_core->mpv, params) < 0) {
                qCritical() << "mpv: failed to create the OpenGL render context";
                m_ctx = nullptr;
            } else {
                mpv_render_context_set_update_callback(m_ctx, onRenderUpdate, m_core.get());
                // Tell the item it may start playback: libmpv expects the
                // render context to exist before a file is opened.
                QMutexLocker lock(&m_core->itemLock);
                if (m_core->item)
                    QMetaObject::invokeMethod(m_core->item, "onRenderContextReady",
                                              Qt::QueuedConnection);
            }
        }
        return QQuickFramebufferObject::Renderer::createFramebufferObject(size);
    }

    void synchronize(QQuickFramebufferObject *item) override { m_window = item->window(); }

    void render() override
    {
        if (!m_ctx || !m_window) return;
        mpv_render_context_update(m_ctx);   // acknowledge pending update flags
        // Hand mpv a clean GL state rather than whatever Qt Quick last left bound.
        QQuickOpenGLUtils::resetOpenGLState();
        resetStateQtLeaves();
        QOpenGLFramebufferObject *fbo = framebufferObject();
        mpv_opengl_fbo mfbo{static_cast<int>(fbo->handle()), fbo->width(), fbo->height(), 0};
        int flipY = 0;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mfbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_ctx, params);
        // ...and mpv changes GL state behind Qt's back; restore it for Qt Quick.
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    // resetOpenGLState() doesn't cover these. Qt's RHI binds sampler objects
    // to texture units and changes pixel-store settings; a leftover sampler
    // overrides mpv's own texture filtering (black or corrupted video, wrong
    // colours), and leftover unpack settings corrupt plane uploads.
    void resetStateQtLeaves()
    {
        QOpenGLContext *ctx = QOpenGLContext::currentContext();
        QOpenGLExtraFunctions *f = ctx->extraFunctions();
        if (m_textureUnits < 0) {
            m_textureUnits = 0;
            f->glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &m_textureUnits);
            m_textureUnits = qMin(m_textureUnits, 32);
            const QSurfaceFormat fmt = ctx->format();
            m_hasSamplers = !ctx->isOpenGLES()
                ? fmt.version() >= qMakePair(3, 3)
                : fmt.version() >= qMakePair(3, 0);
        }
        if (m_hasSamplers)
            for (int i = 0; i < m_textureUnits; ++i) f->glBindSampler(GLuint(i), 0);
        f->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        f->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        f->glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        f->glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        f->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    }

    int m_textureUnits = -1;
    bool m_hasSamplers = false;
    std::shared_ptr<MpvCore> m_core;
    mpv_render_context *m_ctx = nullptr;
    QQuickWindow *m_window = nullptr;
};

// ---- Item (GUI thread) -----------------------------------------------------------------

namespace {
// Once a graphics card's decoder has failed (a driver fault that can reset
// the card), every player decodes in software until Relay restarts.
bool s_hwdecFailed = false;
QSet<MpvItem *> s_players;
}

MpvItem::MpvItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent), m_core(std::make_shared<MpvCore>())
{
    s_players.insert(this);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MpvItem::reconnect);
    m_stableTimer.setSingleShot(true);
    connect(&m_stableTimer, &QTimer::timeout, this, [this] { m_attempt = 0; });
    m_timeshiftTimer.setInterval(500);
    connect(&m_timeshiftTimer, &QTimer::timeout, this, &MpvItem::pollTimeshift);

    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        qCritical() << "mpv: mpv_create failed";
        return;
    }
    m_core->mpv = mpv;
    m_core->item = this;

    setOpt(mpv, "vo", "libmpv");
#if defined(Q_OS_LINUX)
    // mpv's last-resort SDL audio output isn't safe to start from several
    // players at once (multi-view on a machine without a sound server).
    setOpt(mpv, "ao", "pipewire,pulse,alsa,jack,null");
#endif
    // Hardware decoding by default; RELAY_HWDEC=no is an escape hatch for
    // problematic GPU drivers.
    const QByteArray hwdec = qEnvironmentVariableIsSet("RELAY_HWDEC")
        ? qgetenv("RELAY_HWDEC") : (s_hwdecFailed ? QByteArray("no") : QByteArray("auto-safe"));
    setOpt(mpv, "hwdec", hwdec.constData());
    setOpt(mpv, "idle", "yes");
    setOpt(mpv, "keep-open", "no");
    setOpt(mpv, "terminal", "no");
    setOpt(mpv, "input-default-bindings", "no");
    setOpt(mpv, "ytdl", "no");
    // Closed captions carried inside the picture (how US channels send them)
    // become a subtitle track of their own.
    setOpt(mpv, "sub-create-cc-track", "yes");
    // Interlaced channels (1080i sports and news) are deinterlaced; needs
    // mpv 0.38, older ones ignore it.
    mpv_set_option_string(mpv, "deinterlace", "auto");
    // Live-TV friendly networking: reasonable cache, reconnect on drops,
    // give up on dead streams instead of hanging forever.
    setOpt(mpv, "cache", "yes");
    setOpt(mpv, "demuxer-max-bytes", "64MiB");
    setOpt(mpv, "demuxer-readahead-secs", "10");
    setOpt(mpv, "network-timeout", "15");
    setOpt(mpv, "stream-lavf-o", "reconnect=1,reconnect_streamed=1,reconnect_delay_max=5");
    // Pause live TV: keep what's been received (on disk, see applyCacheOptions)
    // and allow seeking inside it even though live streams aren't seekable.
    setOpt(mpv, "demuxer-seekable-cache", "yes");
    setOpt(mpv, "force-seekable", "yes");
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/timeshift");
        QDir().mkpath(dir);
        setOpt(mpv, "demuxer-cache-dir", QDir::toNativeSeparators(dir).toUtf8().constData());
    }

    if (mpv_initialize(mpv) < 0) {
        qCritical() << "mpv: mpv_initialize failed";
        return;
    }
    // A picture shape set before mpv started (trailers fill their area).
    if (m_shape == QLatin1String("fill")) mpv_set_property_string(mpv, "panscan", "1.0");
    else if (m_shape == QLatin1String("stretch")) mpv_set_property_string(mpv, "keepaspect", "no");
    else if (m_shape == QLatin1String("16:9") || m_shape == QLatin1String("4:3"))
        mpv_set_property_string(mpv, "video-aspect-override", m_shape.toUtf8().constData());
    // RELAY_MPV_LOG=v (or debug) for troubleshooting playback.
    const QByteArray logLevel = qEnvironmentVariableIsSet("RELAY_MPV_LOG")
        ? qgetenv("RELAY_MPV_LOG") : QByteArray("error");
    mpv_request_log_messages(mpv, logLevel.constData());

    mpv_observe_property(mpv, PropPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PropPausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PropMute, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, PropVolume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropVideoW, "video-params/w", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, PropVideoH, "video-params/h", MPV_FORMAT_INT64);
    mpv_observe_property(mpv, PropFps, "container-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropVideoFormat, "video-format", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, PropAudioCodec, "audio-codec-name", MPV_FORMAT_STRING);
    mpv_observe_property(mpv, PropTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, PropSeekable, "seekable", MPV_FORMAT_FLAG);

    mpv_set_wakeup_callback(mpv, onMpvWakeup, m_core.get());
}

MpvItem::~MpvItem()
{
    s_players.remove(this);
    if (m_core->mpv) mpv_set_wakeup_callback(m_core->mpv, nullptr, nullptr);
    QMutexLocker lock(&m_core->itemLock);
    m_core->item = nullptr;
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const
{
    return new MpvRenderer(m_core);
}

namespace {
// Pauses before each reconnection attempt for a stream that was playing.
constexpr int kReconnectDelaysMs[] = {500, 1000, 2000, 3000, 5000, 8000, 10000, 15000};
constexpr int kMaxReconnects = int(std::size(kReconnectDelaysMs));
constexpr int kStableMs = 30000;
}

void MpvItem::onRenderContextReady()
{
    m_renderReady = true;
    if (m_loadPending) load();
}

void MpvItem::play(const QString &url, const QString &userAgent, const QString &referrer)
{
    if (!m_core->mpv || url.isEmpty()) return;
    m_audioFile.clear();
    m_url = url;
    m_userAgent = userAgent;
    m_referrer = referrer;
    m_startPos = 0;
    setVod(false);
    resetPosition();
    m_attempt = 0;
    m_everStarted = false;
    m_reconnectTimer.stop();
    m_stableTimer.stop();
    setReconnecting(false);
    resetVideoInfo();
    load();
}

void MpvItem::playVod(const QString &url, double startSeconds)
{
    if (!m_core->mpv || url.isEmpty()) return;
    if (url != m_audioFileFor) m_audioFile.clear();   // playTrailer's audio goes with its video only
    m_url = url;
    m_userAgent.clear();
    m_referrer.clear();
    m_startPos = qMax(0.0, startSeconds);
    m_vodSubChosen = false;
    if (!qFuzzyCompare(m_speed, 1.0)) { m_speed = 1; emit speedChanged(); }
    setVod(true);
    resetPosition();
    m_attempt = 0;
    m_everStarted = false;
    m_reconnectTimer.stop();
    m_stableTimer.stop();
    setReconnecting(false);
    resetVideoInfo();
    load();
}

void MpvItem::playTrailer(const QString &video, const QString &audio)
{
    if (!m_core->mpv || video.isEmpty()) return;
    m_audioFile = audio;
    m_audioFileFor = video;
    playVod(video, 0);
}

void MpvItem::seek(double seconds, bool relative)
{
    if (!m_core->mpv || !m_vod) return;
    // Seek to an absolute time, stopping just short of the end so a long
    // skip doesn't end the movie by accident.
    double target = relative ? m_position + seconds : seconds;
    if (m_duration > 0) target = qMin(target, qMax(0.0, m_duration - 3));
    target = qMax(0.0, target);
    const QByteArray amount = QByteArray::number(target, 'f', 1);
    const char *cmd[] = {"seek", amount.constData(), "absolute", nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
    // Show the new position straight away; mpv confirms shortly after.
    m_position = target;
    emit positionChanged();
}

// Live TV with captions wanted: prefer the captions inside the picture.
void MpvItem::applyCaptionChoice()
{
    if (!m_core->mpv) return;
    if (m_vod) {
        if (m_vodSubChosen) return;
        // mpv marks the captions track it makes as the default; in a movie
        // keep the film's own subtitle choice instead (C still turns them on).
        char *codec = mpv_get_property_string(m_core->mpv, "current-tracks/sub/codec");
        const bool cc = codec && qstrncmp(codec, "eia_", 4) == 0;
        mpv_free(codec);
        if (cc) mpv_set_property_string(m_core->mpv, "sid", "no");
        return;
    }
    if (!m_wantCaptions) return;
    const QVariantList subs = tracks(QStringLiteral("sub"));
    if (subs.isEmpty()) return;
    int pick = -1;
    for (const QVariant &v : subs) {
        const QVariantMap t = v.toMap();
        if (t.value(QStringLiteral("label")).toString().startsWith(tr("Closed captions"))) { pick = t.value(QStringLiteral("id")).toInt(); break; }
    }
    if (pick < 0) {
        for (const QVariant &v : subs)
            if (v.toMap().value(QStringLiteral("selected")).toBool()) return;   // mpv already chose
        pick = subs.first().toMap().value(QStringLiteral("id")).toInt();
    }
    const QByteArray value = QByteArray::number(pick);
    mpv_set_property_string(m_core->mpv, "sid", value.constData());
}

void MpvItem::logSubtitleTracks()
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv) return;
    auto get = [mpv](const QByteArray &name) {
        char *v = mpv_get_property_string(mpv, name.constData());
        const QString s = v ? QString::fromUtf8(v) : QString();
        mpv_free(v);
        return s;
    };
    QStringList parts;
    const int count = get("track-list/count").toInt();
    for (int i = 0; i < count; ++i) {
        const QByteArray p = "track-list/" + QByteArray::number(i) + '/';
        if (get(p + "type") != QLatin1String("sub")) continue;
        QString d = QStringLiteral("#%1 %2").arg(get(p + "id"), get(p + "codec"));
        const QString lang = get(p + "lang");
        if (!lang.isEmpty()) d += QLatin1Char(' ') + lang;
        if (get(p + "default") == QLatin1String("yes")) d += QStringLiteral(" [default]");
        if (get(p + "selected") == QLatin1String("yes")) d += QStringLiteral(" [on]");
        parts << d;
    }
    if (!parts.isEmpty()) qInfo().noquote() << "Subtitles:" << parts.join(QStringLiteral(", "));
}

QVariantList MpvItem::tracks(const QString &type) const
{
    QVariantList out;
    mpv_handle *mpv = m_core->mpv;
    if (!mpv) return out;
    auto get = [mpv](const QByteArray &name) {
        char *v = mpv_get_property_string(mpv, name.constData());
        const QString s = v ? QString::fromUtf8(v) : QString();
        mpv_free(v);
        return s;
    };
    const int count = get("track-list/count").toInt();
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const QByteArray p = "track-list/" + QByteArray::number(i) + '/';
        if (get(p + "type") != type) continue;
        ++n;
        const QString lang = get(p + "lang");
        const QString title = get(p + "title");
        const QString codec = get(p + "codec").toUpper();
        const bool cc = codec == QLatin1String("EIA_608") || codec == QLatin1String("EIA_708");
        QString label = title.isEmpty() ? (lang.isEmpty() ? (cc ? tr("Closed captions") : tr("Track %1").arg(n))
                                                           : lang.toUpper())
                                        : (lang.isEmpty() || title.contains(lang, Qt::CaseInsensitive)
                                               ? title : QStringLiteral("%1 (%2)").arg(title, lang.toUpper()));
        // Subtitle kinds, so a track that shows rubbish can be told apart.
        if (type == QLatin1String("sub") && !codec.isEmpty() && !label.contains(tr("Closed captions")))
            label += QStringLiteral(" · ") + (cc ? tr("CC") : codec);
        if (type == QLatin1String("audio")) {
            const QString ch = get(p + "demux-channel-count");
            if (!codec.isEmpty()) label += QStringLiteral(" · ") + codec;
            if (!ch.isEmpty()) label += QStringLiteral(" %1ch").arg(ch);
        }
        if (get(p + "external") == QLatin1String("yes")) label += tr(" · external");
        out.append(QVariantMap{
            {QStringLiteral("id"), get(p + "id").toInt()},
            {QStringLiteral("label"), label},
            {QStringLiteral("selected"), get(p + "selected") == QLatin1String("yes")},
        });
    }
    return out;
}

void MpvItem::setTrack(const QString &type, int id)
{
    if (!m_core->mpv) return;
    const QByteArray prop = type == QLatin1String("audio") ? "aid" : type == QLatin1String("sub") ? "sid" : "vid";
    if (type == QLatin1String("sub")) {
        if (m_vod) m_vodSubChosen = true;
        else m_wantCaptions = id >= 0;
    }
    const QByteArray value = id < 0 ? QByteArray("no") : QByteArray::number(id);
    mpv_set_property_string(m_core->mpv, prop.constData(), value.constData());
    // Live TV: other channels start in the language just chosen, when they have it.
    if (type == QLatin1String("audio") && !m_vod && id >= 0) {
        mpv_handle *mpv = m_core->mpv;
        int64_t count = 0;
        mpv_get_property(mpv, "track-list/count", MPV_FORMAT_INT64, &count);
        for (int i = 0; i < count; ++i) {
            const QByteArray p = "track-list/" + QByteArray::number(i) + '/';
            int64_t tid = -1;
            mpv_get_property(mpv, (p + "id").constData(), MPV_FORMAT_INT64, &tid);
            char *t = mpv_get_property_string(mpv, (p + "type").constData());
            const bool audio = t && qstrcmp(t, "audio") == 0;
            mpv_free(t);
            if (!audio || tid != id) continue;
            if (char *lang = mpv_get_property_string(mpv, (p + "lang").constData())) {
                if (*lang) mpv_set_property_string(mpv, "alang", lang);
                mpv_free(lang);
            }
            break;
        }
    }
}

// ---- Picture and sound ---------------------------------------------------------

void MpvItem::setPictureShape(const QString &shape)
{
    static const QStringList known{QStringLiteral("auto"), QStringLiteral("16:9"), QStringLiteral("4:3"),
                                   QStringLiteral("fill"), QStringLiteral("stretch")};
    const QString s = known.contains(shape) ? shape : QStringLiteral("auto");
    if (s == m_shape) return;
    m_shape = s;
    if (mpv_handle *mpv = m_core->mpv) {
        const bool forced = s == QLatin1String("16:9") || s == QLatin1String("4:3");
        mpv_set_property_string(mpv, "video-aspect-override", forced ? s.toUtf8().constData() : "no");
        mpv_set_property_string(mpv, "panscan", s == QLatin1String("fill") ? "1.0" : "0.0");
        mpv_set_property_string(mpv, "keepaspect", s == QLatin1String("stretch") ? "no" : "yes");
        update();
    }
    emit pictureChanged();
}

void MpvItem::setDeinterlace(const QString &mode)
{
    const QString m = mode == QLatin1String("on") || mode == QLatin1String("off") ? mode : QStringLiteral("auto");
    if (m == m_deinterlace) return;
    m_deinterlace = m;
    if (mpv_handle *mpv = m_core->mpv) {
        // "auto" (only interlaced pictures) needs mpv 0.38; older ones leave it off.
        if (m == QLatin1String("auto")) {
            if (mpv_set_property_string(mpv, "deinterlace", "auto") < 0)
                mpv_set_property_string(mpv, "deinterlace", "no");
        } else {
            mpv_set_property_string(mpv, "deinterlace", m == QLatin1String("on") ? "yes" : "no");
        }
    }
    emit pictureChanged();
}

void MpvItem::setHardwareDecoding(bool on)
{
    if (on == m_hwdec) return;
    m_hwdec = on;
    if (m_core->mpv && !qEnvironmentVariableIsSet("RELAY_HWDEC"))
        mpv_set_property_string(m_core->mpv, "hwdec", hwdecMode().toLatin1().constData());
    emit pictureChanged();
}

QString MpvItem::hwdecMode() const
{
    return m_hwdec && !s_hwdecFailed ? QStringLiteral("auto-safe") : QStringLiteral("no");
}

void MpvItem::hardwareDecoderFailed()
{
    if (s_hwdecFailed || qEnvironmentVariableIsSet("RELAY_HWDEC")) return;
    s_hwdecFailed = true;
    qWarning().noquote() << "Playback: the graphics card's video decoder failed; using software decoding until Relay restarts";
    // Stop mpv trying other hardware decoders on a card that may be resetting.
    for (MpvItem *player : std::as_const(s_players))
        if (player->m_core->mpv) mpv_set_property_string(player->m_core->mpv, "hwdec", "no");
    emit hardwareDecodingFailed();
}

void MpvItem::setAudioDelayMs(int ms)
{
    ms = qBound(-2000, ms, 2000);
    if (ms == m_audioDelayMs) return;
    m_audioDelayMs = ms;
    if (m_core->mpv) {
        double d = ms / 1000.0;
        mpv_set_property_async(m_core->mpv, 0, "audio-delay", MPV_FORMAT_DOUBLE, &d);
    }
    emit pictureChanged();
}

void MpvItem::setSpeed(double speed)
{
    speed = qBound(0.25, speed, 3.0);
    if (qFuzzyCompare(speed, m_speed)) return;
    m_speed = speed;
    if (m_core->mpv && m_vod) {
        double d = speed;
        mpv_set_property_async(m_core->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &d);
    }
    emit speedChanged();
}

void MpvItem::setTimeshiftMb(int mb)
{
    mb = qMax(0, mb);
    if (mb == m_timeshiftMb) return;
    m_timeshiftMb = mb;
    emit timeshiftMbChanged();   // takes effect from the next channel
}

// Live TV keeps up to timeshiftMb of the channel on disk (half for pausing,
// half for rewinding). Movies read only a little ahead, so a server isn't
// asked for gigabytes at once.
void MpvItem::applyCacheOptions()
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv) return;
    auto set = [mpv](const char *name, const QByteArray &v) { mpv_set_property_string(mpv, name, v.constData()); };
    if (!m_vod && m_timeshiftMb > 0) {
        const QByteArray half = QByteArray::number(qMax(32, m_timeshiftMb / 2)) + "MiB";
        set("cache-on-disk", "yes");
        set("demuxer-max-bytes", half);
        set("demuxer-max-back-bytes", half);
    } else if (!m_vod) {
        set("cache-on-disk", "no");
        set("demuxer-max-bytes", "64MiB");
        set("demuxer-max-back-bytes", "32MiB");
    } else {
        set("cache-on-disk", "no");
        set("demuxer-max-bytes", "150MiB");
        set("demuxer-max-back-bytes", "50MiB");
    }
}

// What's been kept of the live channel, in playback time: `start` and
// `seekEnd` bound where we can jump to (jumps land on keyframes, so the
// newest few seconds can't be jumped into yet), `dataEnd` is the newest
// video received, `pos` the playing point.
bool MpvItem::cachedRange(double *start, double *seekEnd, double *dataEnd, double *pos) const
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv) return false;
    double cur = 0;
    if (mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &cur) < 0) return false;
    mpv_node node;
    if (mpv_get_property(mpv, "demuxer-cache-state", MPV_FORMAT_NODE, &node) < 0) return false;
    auto num = [](const mpv_node &v) {
        return v.format == MPV_FORMAT_DOUBLE ? v.u.double_ : v.format == MPV_FORMAT_INT64 ? double(v.u.int64) : 0.0;
    };
    bool found = false;
    double s = cur, e = cur, end = cur;
    if (node.format == MPV_FORMAT_NODE_MAP) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const char *key = node.u.list->keys[i];
            const mpv_node &val = node.u.list->values[i];
            if (qstrcmp(key, "cache-end") == 0) { end = num(val); continue; }
            if (qstrcmp(key, "seekable-ranges") != 0 || val.format != MPV_FORMAT_NODE_ARRAY) continue;
            // The range we're playing in: the one that starts before the
            // playing point and reaches furthest.
            for (int r = 0; r < val.u.list->num; ++r) {
                const mpv_node &range = val.u.list->values[r];
                if (range.format != MPV_FORMAT_NODE_MAP) continue;
                double rs = 0, re = 0;
                for (int k = 0; k < range.u.list->num; ++k) {
                    if (qstrcmp(range.u.list->keys[k], "start") == 0) rs = num(range.u.list->values[k]);
                    else if (qstrcmp(range.u.list->keys[k], "end") == 0) re = num(range.u.list->values[k]);
                }
                if (rs - 1 <= cur && (!found || re > e)) { s = rs; e = re; found = true; }
            }
        }
    }
    mpv_free_node_contents(&node);
    end = qMax(end, cur);
    static const bool debug = qEnvironmentVariableIsSet("RELAY_DEBUG_TIMESHIFT");
    if (debug) qInfo().noquote() << QStringLiteral("Timeshift: pos %1, jumpable %2–%3, received to %4%5")
                                        .arg(cur, 0, 'f', 1).arg(s, 0, 'f', 1).arg(e, 0, 'f', 1)
                                        .arg(end, 0, 'f', 1).arg(found ? QString() : QStringLiteral(" (no range)"));
    if (start) *start = s;
    if (seekEnd) *seekEnd = e;
    if (dataEnd) *dataEnd = end;
    if (pos) *pos = cur;
    return found;
}

void MpvItem::setTimeshift(double behind, double buffer)
{
    // Ignore jitter of a second or two at the live edge.
    behind = behind < 3 ? 0 : behind;
    if (qAbs(behind - m_liveBehind) < 0.5 && qAbs(buffer - m_liveBuffer) < 1) return;
    m_liveBehind = behind;
    m_liveBuffer = buffer;
    emit timeshiftChanged();
}

void MpvItem::pollTimeshift()
{
    if (m_vod || m_url.isEmpty() || m_loading) { setTimeshift(0, 0); return; }
    // A jump is on its way: show its target until it lands (or 4 s pass).
    if (m_liveSeekTarget >= 0 && m_liveSeekSince.isValid() && m_liveSeekSince.elapsed() < 4000) return;
    m_liveSeekTarget = -1;
    double s = 0, e = 0, end = 0, cur = 0;
    const bool ok = cachedRange(&s, &e, &end, &cur);
    const double kept = ok ? qMax(0.0, cur - s) : 0;
    const double ahead = qMax(0.0, end - cur);
    if (m_atLive && !m_paused) {
        m_liveSlack = ahead;                 // normal distance from the newest video
        setTimeshift(0, kept);
        return;
    }
    const double behind = ahead - m_liveSlack;
    if (!m_paused && behind < 2) {          // caught up (e.g. by skipping forward)
        m_atLive = true;
        setTimeshift(0, kept);
        return;
    }
    setTimeshift(qMax(0.0, behind), kept);
}

void MpvItem::seekLive(double seconds)
{
    if (!m_core->mpv || m_vod) return;
    double s = 0, e = 0, end = 0, cur = 0;
    if (!cachedRange(&s, &e, &end, &cur)) return;
    // Presses while a jump is still on its way count from where it's going.
    const double from = m_liveSeekTarget >= 0 ? m_liveSeekTarget : cur;
    // Forward: as far as the newest keyframe kept, never past live.
    const double maxTarget = qMax(from, qMin(e - 0.2, end - m_liveSlack));
    const double target = qBound(qMin(from, s + 0.1), from + seconds, maxTarget);
    const double delta = target - from;
    if (qAbs(delta) < 0.5) {
        if (seconds > 0 && !m_paused && m_liveSeekTarget < 0) { m_atLive = true; setTimeshift(0, qMax(0.0, cur - s)); }
        return;
    }
    // Back: land on the keyframe before the target (quick, and it's always
    // been kept). Forward: an exact jump, which decodes from a keyframe that
    // has been kept; a quick forward jump would need the keyframe after the
    // target, which near live may not have arrived yet.
    const QByteArray amount = QByteArray::number(target, 'f', 2);
    const char *cmd[] = {"seek", amount.constData(), delta < 0 ? "absolute+keyframes" : "absolute+exact", nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
    m_liveSeekTarget = target;
    m_liveSeekSince.start();
    if (seconds < 0) m_atLive = false;
    else if (target >= maxTarget - 0.5) m_atLive = true;   // as close to live as it gets
    setTimeshift(m_atLive ? 0 : qMax(0.0, end - target - m_liveSlack), qMax(0.0, target - s));
}

void MpvItem::goLive()
{
    if (!m_core->mpv || m_vod) return;
    if (m_paused) {
        int no = 0;
        mpv_set_property_async(m_core->mpv, 0, "pause", MPV_FORMAT_FLAG, &no);
    }
    double s = 0, e = 0, end = 0, cur = 0;
    const bool ok = cachedRange(&s, &e, &end, &cur);
    m_atLive = true;
    // Jump to the newest keyframe kept; what's left is the new normal distance.
    const double from = m_liveSeekTarget >= 0 ? m_liveSeekTarget : cur;
    if (ok && e - from > 1) {
        const QByteArray amount = QByteArray::number(e - 0.2, 'f', 2);
        const char *cmd[] = {"seek", amount.constData(), "absolute+exact", nullptr};
        mpv_command_async(m_core->mpv, 0, cmd);
        m_liveSeekTarget = e - 0.2;
        m_liveSeekSince.start();
        cur = e;
    }
    setTimeshift(0, qMax(0.0, cur - s));
}

void MpvItem::setVod(bool v)
{
    if (m_vod == v) return;
    m_vod = v;
    emit vodChanged();
}

void MpvItem::resetPosition()
{
    m_position = 0;
    m_duration = 0;
    emit positionChanged();
    emit durationChanged();
    if (m_seekable) { m_seekable = false; emit seekableChanged(); }
}

void MpvItem::reconnect()
{
    if (m_url.isEmpty()) return;
    qInfo().noquote() << QStringLiteral("Playback: reconnecting (attempt %1)").arg(m_attempt);
    load();
}

void MpvItem::load()
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv || m_url.isEmpty()) return;
    m_loading = true;
    setBuffering(true);

    if (!m_renderReady) {          // first frame not drawn yet: start right after
        m_loadPending = true;
        return;
    }
    m_loadPending = false;

    // Per-channel HTTP options must be set before loadfile.
    const QByteArray ua = m_userAgent.toUtf8();
    const QByteArray ref = m_referrer.toUtf8();
    mpv_set_property_string(mpv, "user-agent", ua.isEmpty() ? (m_defaultUserAgent.isEmpty() ? QByteArray("Relay/0.1") : m_defaultUserAgent.toUtf8()).constData()
                                                            : ua.constData());
    mpv_set_property_string(mpv, "referrer", ref.constData());
    applyCacheOptions();
    {   // Live TV always plays at normal speed.
        double d = m_vod ? m_speed : 1.0;
        mpv_set_property_async(mpv, 0, "speed", MPV_FORMAT_DOUBLE, &d);
    }
    m_atLive = true;
    m_liveSlack = 0;
    m_liveSeekTarget = -1;
    // Resume point for movies (also used when a dropped movie reconnects).
    const QByteArray start = m_vod && m_startPos > 1
        ? "+" + QByteArray::number(m_startPos, 'f', 1) : QByteArray("none");
    mpv_set_property_string(mpv, "start", start.constData());
    // A trailer comes as separate video and audio addresses (YouTube).
    if (!m_audioFile.isEmpty()) mpv_set_property_string(mpv, "audio-files", m_audioFile.toUtf8().constData());
    else {
        const char *clear[] = {"change-list", "audio-files", "clr", "", nullptr};
        mpv_command(mpv, clear);
    }
    // Live TV: no captions unless asked for (some streams mark a broken
    // caption track as the default); movies: mpv's usual choice.
    mpv_set_property_string(mpv, "sid", m_vod || m_wantCaptions ? "auto" : "no");

    const quint64 serial = ++m_loadSerial;
    if (PortalClient::isPortalUrl(m_url)) {
        // A portal channel, movie or past show: ask the portal for today's
        // address first. Portals only serve their own box's player.
        if (m_userAgent.isEmpty()) mpv_set_property_string(mpv, "user-agent", PortalClient::kBoxAgent);
        const QString asked = m_url;
        PortalClient::instance()->resolveAt(m_url, this, [this, serial, asked](const QString &real, const QString &error, int skip) {
            if (serial != m_loadSerial || asked != m_url) return;   // moved on meanwhile
            if (real.isEmpty()) {
                qWarning().noquote() << "Playback: portal:" << error;
                m_lastError = error;
                handleStreamEnd(true);
                return;
            }
            if (skip > 1 && skip > m_startPos + 1 && m_core->mpv) {
                // A past show's stream starts where the show starts.
                const QByteArray at = "+" + QByteArray::number(skip, 'f', 1);
                mpv_set_property_string(m_core->mpv, "start", at.constData());
            }
            startFile(real);
        });
        return;
    }
    startFile(m_url);
}

void MpvItem::startFile(const QString &address)
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv) return;
    const QByteArray u = address.toUtf8();
    const char *cmd[] = {"loadfile", u.constData(), "replace", nullptr};
    mpv_command_async(mpv, 0, cmd);

    if (m_paused) {
        int no = 0;
        mpv_set_property_async(mpv, 0, "pause", MPV_FORMAT_FLAG, &no);
    }
}

void MpvItem::handleStreamEnd(bool error)
{
    if (m_url.isEmpty()) return;   // stopped on purpose
    m_stableTimer.stop();
    const qint64 playedSecs = m_playingFor.isValid() ? m_playingFor.elapsed() / 1000 : 0;
    const QString why = error ? QStringLiteral("error: %1").arg(m_lastError)
                              : QStringLiteral("server ended the stream");
    setPlaying(false);
    if (m_vod && m_position > 1)   // pick up where it dropped
        m_startPos = m_duration > 0 ? qMin(m_position, m_duration - 10) : m_position;

    const int maxAttempts = m_everStarted ? kMaxReconnects : 1;
    if (m_attempt < maxAttempts) {
        const int delay = kReconnectDelaysMs[qMin(m_attempt, kMaxReconnects - 1)];
        ++m_attempt;
        qInfo().noquote() << QStringLiteral("Playback: stream stopped (%1) after %2 s; retrying in %3 s")
                                 .arg(why).arg(playedSecs).arg(delay / 1000.0, 0, 'f', 1);
        m_loading = true;
        setBuffering(true);
        setReconnecting(m_everStarted);
        m_reconnectTimer.start(delay);
        return;
    }

    qWarning().noquote() << QStringLiteral("Playback: giving up after %1 attempts (%2)")
                                .arg(m_attempt).arg(why);
    m_loading = false;
    setBuffering(false);
    setReconnecting(false);
    QString msg = m_vod ? (m_everStarted ? tr("Lost the connection to this video.")
                                         : tr("This video can't be played right now."))
                        : m_everStarted ? tr("Lost the connection to this channel.")
                                        : tr("This channel isn't playing right now.");
    if (error && !m_lastError.isEmpty()) msg += QStringLiteral(" (%1)").arg(m_lastError);
    emit playbackError(msg);
}

void MpvItem::setReconnecting(bool r)
{
    if (m_reconnecting == r) return;
    m_reconnecting = r;
    emit reconnectingChanged();
}

void MpvItem::stop()
{
    m_url.clear();
    m_loadPending = false;
    setVod(false);
    resetPosition();
    m_reconnectTimer.stop();
    m_stableTimer.stop();
    setReconnecting(false);
    if (!m_core->mpv) return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
    m_loading = false;
    setBuffering(false);
    setPlaying(false);
    resetVideoInfo();
}

void MpvItem::togglePause()
{
    if (!m_core->mpv) return;
    const char *cmd[] = {"cycle", "pause", nullptr};
    mpv_command_async(m_core->mpv, 0, cmd);
}

void MpvItem::setMuted(bool m)
{
    if (!m_core->mpv || m == m_muted) return;
    m_muted = m;            // update now so the UI shows the new state immediately
    emit mutedChanged();
    int flag = m ? 1 : 0;
    mpv_set_property_async(m_core->mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void MpvItem::setVolume(int v)
{
    v = qBound(0, v, 130);
    if (!m_core->mpv || v == m_volume) return;
    m_volume = v;           // update now; mpv confirms asynchronously
    emit volumeChanged();
    double d = v;
    mpv_set_property_async(m_core->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &d);
}

void MpvItem::setBuffering(bool b)
{
    if (m_buffering == b) return;
    m_buffering = b;
    emit bufferingChanged();
}

void MpvItem::setPlaying(bool p)
{
    if (p) m_timeshiftTimer.start();
    else { m_timeshiftTimer.stop(); setTimeshift(0, 0); }
    if (m_playing == p) return;
    m_playing = p;
    emit playingChanged();
}

void MpvItem::resetVideoInfo()
{
    m_videoW = m_videoH = 0;
    m_fps = 0;
    m_vcodec.clear();
    m_acodec.clear();
    emit videoInfoChanged();
}

void MpvItem::processEvents()
{
    mpv_handle *mpv = m_core->mpv;
    if (!mpv) return;

    while (true) {
        mpv_event *ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *p = static_cast<mpv_event_property *>(ev->data);
            if (!p->data) break;
            switch (ev->reply_userdata) {
            case PropPause: {
                const bool v = *static_cast<int *>(p->data);
                if (v && !m_vod) m_atLive = false;   // live TV paused: now behind
                if (v != m_paused) { m_paused = v; emit pausedChanged(); }
                break;
            }
            case PropPausedForCache:
                m_pausedForCache = *static_cast<int *>(p->data);
                setBuffering(m_loading || m_pausedForCache);
                break;
            case PropMute: {
                const bool v = *static_cast<int *>(p->data);
                if (v != m_muted) { m_muted = v; emit mutedChanged(); }
                break;
            }
            case PropVolume: {
                const int v = qRound(*static_cast<double *>(p->data));
                if (v != m_volume) { m_volume = v; emit volumeChanged(); }
                break;
            }
            case PropVideoW: m_videoW = int(*static_cast<int64_t *>(p->data)); emit videoInfoChanged(); break;
            case PropVideoH: m_videoH = int(*static_cast<int64_t *>(p->data)); emit videoInfoChanged(); break;
            case PropFps: m_fps = *static_cast<double *>(p->data); emit videoInfoChanged(); break;
            case PropVideoFormat:
                m_vcodec = QString::fromUtf8(*static_cast<char **>(p->data)).toUpper();
                emit videoInfoChanged();
                break;
            case PropTimePos: {
                // Notified every frame; the UI only needs a few updates a second.
                const double v = *static_cast<double *>(p->data);
                if (qAbs(v - m_position) >= 0.25) { m_position = v; emit positionChanged(); }
                break;
            }
            case PropDuration: {
                const double v = *static_cast<double *>(p->data);
                if (v != m_duration) { m_duration = v; emit durationChanged(); }
                break;
            }
            case PropSeekable: {
                const bool v = *static_cast<int *>(p->data);
                if (v != m_seekable) { m_seekable = v; emit seekableChanged(); }
                break;
            }
            case PropAudioCodec:
                m_acodec = QString::fromUtf8(*static_cast<char **>(p->data)).toUpper();
                emit videoInfoChanged();
                break;
            }
            break;
        }
        case MPV_EVENT_START_FILE:
            m_lastError.clear();
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            if (m_liveSeekTarget >= 0) {       // a live jump has landed
                m_liveSeekTarget = -1;
                if (!m_vod && !m_loading) QTimer::singleShot(0, this, &MpvItem::pollTimeshift);
            }
            if (m_loading) {
                m_loading = false;
                if (m_reconnecting) qInfo() << "Playback: reconnected";
                m_everStarted = true;
                setReconnecting(false);
                logSubtitleTracks();
                applyCaptionChoice();
                m_playingFor.start();
                m_stableTimer.start(kStableMs);
                setPlaying(true);
                emit playbackStarted();
            }
            setBuffering(m_pausedForCache);
            break;
        case MPV_EVENT_END_FILE: {
            // EOF on live TV means the connection dropped; ERROR covers
            // network failures. Both get reconnected. STOP (switching
            // channels) and QUIT are expected and ignored.
            auto *ef = static_cast<mpv_event_end_file *>(ev->data);
            if (ef->reason == MPV_END_FILE_REASON_ERROR)
                m_lastError = QString::fromUtf8(mpv_error_string(ef->error));
            const bool ended = ef->reason == MPV_END_FILE_REASON_EOF || ef->reason == MPV_END_FILE_REASON_ERROR;
            if (m_vod && ended && !m_url.isEmpty()) {
                // A movie that stops well before its end dropped; one that
                // gets to (or near) the end is simply finished. Servers often
                // report an error rather than a clean end on the last bytes.
                const bool nearEnd = m_duration > 0 && m_position >= m_duration - 30;
                if (!nearEnd && (m_duration > 0 || ef->reason == MPV_END_FILE_REASON_ERROR)) {
                    handleStreamEnd(ef->reason == MPV_END_FILE_REASON_ERROR);
                    break;
                }
                qInfo() << "Playback: video finished";
                m_stableTimer.stop();
                m_loading = false;
                setBuffering(false);
                setPlaying(false);
                emit finished();
                break;
            }
            if (ef->reason == MPV_END_FILE_REASON_ERROR) handleStreamEnd(true);
            else if (ef->reason == MPV_END_FILE_REASON_EOF) handleStreamEnd(false);
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *msg = static_cast<mpv_event_log_message *>(ev->data);
            m_lastError = QString::fromUtf8(msg->text).trimmed();
            qWarning().noquote() << "mpv[" << msg->prefix << "]" << m_lastError;
            if (m_hwdec && (m_lastError.contains(QLatin1String("hardware accelerator failed"))
                            || m_lastError.contains(QLatin1String("Could not copy back hardware decoded frame"))
                            || m_lastError.contains(QLatin1String("Unable to lock D3D11VA surface"))))
                hardwareDecoderFailed();
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            return;
        default:
            break;
        }
    }
}
