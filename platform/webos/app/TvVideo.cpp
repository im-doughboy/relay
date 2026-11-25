#include "TvVideo.h"

#include <QGuiApplication>
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

// What can be shown in Relay's log about an address: scheme and host only,
// never the provider's login in the path or query.
QString safeAddress(const QString &url)
{
    const QUrl u(url);
    return u.isValid() ? u.scheme() + QStringLiteral("://") + u.host() + QStringLiteral("/…") : QStringLiteral("(invalid)");
}
} // namespace

TvVideo::TvVideo(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    // The TV's home screen, another app or input in front: let go of the
    // TV's video decoder (another app may need it), and pick up the same
    // channel or movie where it was when Relay is back.
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) resume();
        else release();
    });
}

void TvVideo::release()
{
    if (m_url.isEmpty() || m_released) return;
    m_released = true;
    m_resumePosition = m_position;
    qInfo().noquote() << "TV player: Relay is in the background; the video is let go";
    // Playback isn't connected yet; this is where the TV's player is unloaded.
    setShowVideo(false);
}

void TvVideo::resume()
{
    if (!m_released) return;
    m_released = false;
    qInfo().noquote() << "TV player: Relay is back; playing" << safeAddress(m_url) << "again";
    start(m_url, m_vod, m_vod ? m_resumePosition : 0);
}

TvVideo::~TvVideo() = default;

void TvVideo::start(const QString &url, bool vod, double startSeconds)
{
    m_url = url;
    setVod(vod);
    m_position = startSeconds;
    emit positionChanged();
    qInfo().noquote() << "TV player: asked to play" << safeAddress(url) << (vod ? "(movie or episode)" : "(live)")
                      << "- the TV's player isn't connected to Relay yet";
}

void TvVideo::play(const QString &url, const QString &userAgent, const QString &)
{
    m_userAgent = userAgent.isEmpty() ? m_defaultUserAgent : userAgent;
    start(url, false, 0);
}

void TvVideo::playVod(const QString &url, double startSeconds) { start(url, true, startSeconds); }

void TvVideo::playTrailer(const QString &, const QString &)
{
    qInfo().noquote() << "TV player: trailers don't play on the TV";
}

void TvVideo::seek(double, bool) {}
QVariantList TvVideo::tracks(const QString &) const { return {}; }
void TvVideo::setTrack(const QString &, int) {}
void TvVideo::seekLive(double) {}
void TvVideo::goLive() {}

void TvVideo::stop()
{
    m_url.clear();
    m_released = false;
    setShowVideo(false);
    if (m_playing) { m_playing = false; emit playingChanged(); }
    if (m_buffering) { m_buffering = false; emit bufferingChanged(); }
    if (m_paused) { m_paused = false; emit pausedChanged(); }
    setVod(false);
}

void TvVideo::togglePause()
{
    if (!m_playing) return;
    m_paused = !m_paused;
    emit pausedChanged();
}

void TvVideo::setMuted(bool m)
{
    if (m == m_muted) return;
    m_muted = m;
    emit mutedChanged();
}

void TvVideo::setVolume(int v)
{
    v = qBound(0, v, 100);
    if (v == m_volume) return;
    m_volume = v;
    emit volumeChanged();
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

void TvVideo::setVod(bool v)
{
    if (v == m_vod) return;
    m_vod = v;
    emit vodChanged();
    emit seekableChanged();
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
