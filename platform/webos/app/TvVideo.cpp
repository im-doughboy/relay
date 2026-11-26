#include "TvVideo.h"

#include "TvPlayer.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QUrl>

namespace {
// Writes "nothing here" (transparent black) over its area without blending,
// so the TV's video layer under Qt Quick's shows through. Marked opaque, it is
// drawn before the see-through parts of the screen and hides what's behind
// it, while anything drawn over the video (info bar, menus) still is.
class VideoHoleMaterial : public QSGFlatColorMaterial
{
public:
    VideoHoleMaterial()
    {
        setColor(Qt::transparent);
        setFlag(Blending, false);
    }
};

// What can be shown in Relay's log about an address: scheme and host only.
QString safeAddress(const QString &url)
{
    const QUrl u(url);
    return u.isValid() ? u.scheme() + QStringLiteral("://") + u.host() + QStringLiteral("/…") : QStringLiteral("(invalid)");
}

constexpr int kMaxReconnects = 4;
} // namespace

TvVideo::TvVideo(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &TvVideo::open);
    m_openTimer.setSingleShot(true);
    m_openTimer.setInterval(400);
    connect(&m_openTimer, &QTimer::timeout, this, &TvVideo::open);

    TvPlayer *p = TvPlayer::instance();
    connect(p, &TvPlayer::loaded, this, [this] { if (owns()) setShowVideo(true); });
    connect(p, &TvPlayer::playing, this, [this] { if (owns()) onPlaying(); });
    connect(p, &TvPlayer::paused, this, [this] { if (owns()) setPaused(true); });
    connect(p, &TvPlayer::buffering, this, [this](bool on) { if (owns()) setBuffering(on); });
    connect(p, &TvPlayer::ended, this, [this] { if (owns()) onEnded(); });
    connect(p, &TvPlayer::failed, this, [this](const QString &m) { if (owns()) onFailed(m); });
    connect(p, &TvPlayer::positionChanged, this, [this](double s) {
        if (!owns() || !m_vod || qAbs(s - m_position) < 0.25) return;
        m_position = s;
        emit positionChanged();
    });
    connect(p, &TvPlayer::durationChanged, this, [this](double s) {
        if (!owns() || qFuzzyCompare(s, m_duration)) return;
        m_duration = s;
        emit durationChanged();
        emit seekableChanged();
    });
    connect(p, &TvPlayer::videoInfo, this, [this](int w, int h, double fps, const QString &vc, const QString &ac) {
        if (!owns()) return;
        if (w > 0) m_videoW = w;
        if (h > 0) m_videoH = h;
        if (fps > 0) m_fps = fps;
        if (!vc.isEmpty()) m_vcodec = vc;
        if (!ac.isEmpty()) m_acodec = ac;
        emit videoInfoChanged();
    });

    // The TV's home screen, another app or input in front: let go of the
    // TV's video decoder (another app may need it), and pick up the same
    // channel or movie where it was when Relay is back.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) resume();
        else release();
    });
}

TvVideo::~TvVideo()
{
    if (owns()) TvPlayer::instance()->unload();
}

bool TvVideo::owns() const { return TvPlayer::instance()->owner() == this; }

// ---- what Relay asks for ----------------------------------------------------------------------

void TvVideo::play(const QString &url, const QString &userAgent, const QString &)
{
    m_userAgent = userAgent.isEmpty() ? m_defaultUserAgent : userAgent;
    start(url, false, 0);
}

void TvVideo::playVod(const QString &url, double startSeconds)
{
    m_userAgent = m_defaultUserAgent;
    start(url, true, startSeconds);
}

void TvVideo::playTrailer(const QString &, const QString &)
{
    // YouTube trailers need yt-dlp, which doesn't run on the TV.
    qInfo().noquote() << "TV player: trailers don't play on the TV";
}

void TvVideo::start(const QString &url, bool vod, double startSeconds)
{
    m_reconnectTimer.stop();
    m_url = url;
    m_released = false;
    m_everStarted = false;
    m_attempt = 0;
    m_startSeconds = startSeconds;
    setVod(vod);
    m_position = vod ? startSeconds : 0;
    m_duration = 0;
    emit positionChanged();
    emit durationChanged();
    resetInfo();
    setReconnecting(false);
    // Changing channels quickly (holding CH, pressing it several times):
    // the TV's player takes a second or more to start each stream, so only
    // the channel the presses stop on is started. The first press starts
    // at once; the info bar follows every press meanwhile.
    const bool quick = !vod && m_lastStart.isValid() && m_lastStart.elapsed() < 800;
    m_lastStart.restart();
    if (quick) {
        setBuffering(true);
        m_openTimer.start();
        return;
    }
    m_openTimer.stop();
    open();
}

void TvVideo::open()
{
    if (m_url.isEmpty()) return;
    setPlaying(false);
    setPaused(false);
    setBuffering(true);
    m_lastRect = QRect();
    // A movie or episode: the TV's player fetches the file itself; if that
    // doesn't start, once more through the relay. Live TV: the relay.
    const bool direct = m_vod && m_attempt == 0;
    TvPlayer::instance()->load(this, m_url, m_userAgent, m_vod, direct);
    trackGeometry();
    applyVolume();
}

void TvVideo::stop()
{
    m_reconnectTimer.stop();
    m_openTimer.stop();
    m_url.clear();
    m_released = false;
    if (owns()) TvPlayer::instance()->unload();
    setShowVideo(false);
    setPlaying(false);
    setBuffering(false);
    setPaused(false);
    setReconnecting(false);
    setVod(false);
    resetInfo();
}

void TvVideo::togglePause()
{
    if (!owns() || !m_playing) return;
    if (m_paused) {
        TvPlayer::instance()->play();
        setPaused(false);
    } else {
        TvPlayer::instance()->pause();
        setPaused(true);
    }
}

void TvVideo::seek(double seconds, bool relative)
{
    if (!owns() || !m_vod) return;
    double target = relative ? m_position + seconds : seconds;
    if (m_duration > 0) target = qBound(0.0, target, qMax(0.0, m_duration - 1));
    m_position = qMax(0.0, target);
    emit positionChanged();
    TvPlayer::instance()->seek(m_position);
}

// Sound tracks, subtitles and pausing live TV come with later work on the TV.
QVariantList TvVideo::tracks(const QString &) const { return {}; }
void TvVideo::setTrack(const QString &, int) {}
void TvVideo::seekLive(double) {}
void TvVideo::goLive() {}

// ---- what the TV's player says ----------------------------------------------------------------

void TvVideo::onPlaying()
{
    setShowVideo(true);
    setBuffering(false);
    setPaused(false);
    setReconnecting(false);
    const bool first = !m_playing;
    setPlaying(true);
    if (!first) return;
    m_attempt = 0;
    if (!m_everStarted) {
        m_everStarted = true;
        emit playbackStarted();
        if (m_vod && m_startSeconds > 1) TvPlayer::instance()->seek(m_startSeconds);   // Continue watching
    }
}

void TvVideo::onEnded()
{
    // The TV's player also reports the end of a stream that had already
    // failed (30 s after a provider's 509, say). That isn't the movie
    // finishing: taken as one, it was saved as watched and left the movie
    // while Relay was offering other sources.
    if (!m_playing) {
        qInfo().noquote() << "TV player: ignoring the end of a stream that had already stopped";
        return;
    }
    if (m_vod) {
        qInfo().noquote() << "TV player: the movie or episode finished";
        setPlaying(false);
        emit finished();
        return;
    }
    onFailed(tr("The channel stopped sending."));
}

void TvVideo::onFailed(const QString &message)
{
    qWarning().noquote() << "TV player:" << message;
    // A live channel that played drops now and then: open it again a few
    // times, waiting longer each time. One that never started gets a single
    // quick second try before the error shows; a movie that never started,
    // one more try through the relay.
    const int limit = m_vod ? (m_everStarted ? 0 : 1) : (m_everStarted ? kMaxReconnects : 1);
    if (!m_url.isEmpty() && m_attempt < limit) {
        ++m_attempt;
        setReconnecting(m_everStarted);
        setBuffering(true);
        m_reconnectTimer.start(1000 * m_attempt);
        qInfo().noquote() << "TV player: trying" << safeAddress(m_url) << "again (" << m_attempt << "of" << limit << ")";
        return;
    }
    setReconnecting(false);
    setBuffering(false);
    setPlaying(false);
    setShowVideo(false);
    emit playbackError(message);
}

void TvVideo::reconnect() { open(); }

// ---- background ---------------------------------------------------------------------------------

void TvVideo::release()
{
    if (m_url.isEmpty() || m_released) return;
    m_released = true;
    m_reconnectTimer.stop();
    m_openTimer.stop();
    if (m_vod) m_startSeconds = m_position;
    qInfo().noquote() << "TV player: Relay is in the background; the video is let go";
    if (owns()) TvPlayer::instance()->unload();
    setShowVideo(false);
}

void TvVideo::resume()
{
    if (!m_released) return;
    m_released = false;
    qInfo().noquote() << "TV player: Relay is back; playing" << safeAddress(m_url) << "again";
    m_everStarted = false;   // a movie starts again where it was (m_startSeconds)
    m_attempt = 0;
    setPlaying(false);
    open();
}

// ---- picture ------------------------------------------------------------------------------------

void TvVideo::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange && data.window)
        // Each frame: the item can move with what holds it (the guide's small picture).
        connect(data.window, &QQuickWindow::afterAnimating, this, &TvVideo::trackGeometry, Qt::UniqueConnection);
}

void TvVideo::trackGeometry()
{
    if (!owns() || width() <= 0 || height() <= 0) return;
    const QRect rect = mapRectToScene(QRectF(0, 0, width(), height())).toAlignedRect() & QRect(0, 0, 1920, 1080);
    if (rect == m_lastRect || rect.isEmpty()) return;
    m_lastRect = rect;
    TvPlayer::instance()->setVideoRect(rect);
}

void TvVideo::setShowVideo(bool on)
{
    if (on == m_showVideo) return;
    m_showVideo = on;
    update();
}

QSGNode *TvVideo::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    if (!m_showVideo || width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }
    auto *node = static_cast<QSGGeometryNode *>(old);
    if (!node) {
        node = new QSGGeometryNode;
        node->setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4));
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new VideoHoleMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }
    QSGGeometry::updateRectGeometry(node->geometry(), QRectF(0, 0, width(), height()));
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}

// ---- small state changes ------------------------------------------------------------------------

void TvVideo::setBuffering(bool b) { if (b != m_buffering) { m_buffering = b; emit bufferingChanged(); } }
void TvVideo::setPlaying(bool p) { if (p != m_playing) { m_playing = p; emit playingChanged(); } }
void TvVideo::setPaused(bool p) { if (p != m_paused) { m_paused = p; emit pausedChanged(); } }
void TvVideo::setReconnecting(bool r) { if (r != m_reconnecting) { m_reconnecting = r; emit reconnectingChanged(); } }

void TvVideo::setVod(bool v)
{
    if (v == m_vod) return;
    m_vod = v;
    emit vodChanged();
    emit seekableChanged();
}

void TvVideo::resetInfo()
{
    m_videoW = m_videoH = 0;
    m_fps = 0;
    m_vcodec.clear();
    m_acodec.clear();
    emit videoInfoChanged();
}

void TvVideo::applyVolume()
{
    if (owns()) TvPlayer::instance()->setVolume(m_muted ? 0 : m_volume);
}

void TvVideo::setMuted(bool m)
{
    if (m == m_muted) return;
    m_muted = m;
    emit mutedChanged();
    applyVolume();
}

void TvVideo::setVolume(int v)
{
    v = qBound(0, v, 100);
    if (v == m_volume) return;
    m_volume = v;
    emit volumeChanged();
    applyVolume();
}

void TvVideo::setTimeshiftMb(int mb)
{
    if (mb == m_timeshiftMb) return;
    m_timeshiftMb = mb;
    emit timeshiftMbChanged();
}

void TvVideo::setPictureShape(const QString &shape)
{
    if (shape == m_shape) return;
    m_shape = shape;
    emit pictureChanged();
}

void TvVideo::setDeinterlace(const QString &mode)
{
    if (mode == m_deinterlace) return;
    m_deinterlace = mode;
    emit pictureChanged();
}

void TvVideo::setAudioDelayMs(int ms)
{
    if (ms == m_audioDelayMs) return;
    m_audioDelayMs = ms;
    emit pictureChanged();
}

void TvVideo::setSpeed(double speed)
{
    if (qFuzzyCompare(speed, m_speed)) return;
    m_speed = speed;
    emit speedChanged();
}

void TvVideo::setPreferredAudio(const QString &code)
{
    if (code == m_prefAudio) return;
    m_prefAudio = code;
    emit pictureChanged();
}

void TvVideo::setPreferredSubtitles(const QString &code)
{
    if (code == m_prefSubs) return;
    m_prefSubs = code;
    emit pictureChanged();
}
