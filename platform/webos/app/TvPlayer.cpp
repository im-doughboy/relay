#include "TvPlayer.h"

#include "TvLuna.h"
#include "cast/CastRelay.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_webOS.h>
#include <luna-service2/lunaservice.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

#include <cstdint>

namespace {
const char *kAppId = "com.imdoughboy.relay";

QString appId()
{
    const QByteArray env = qgetenv("APPID");
    return env.isEmpty() ? QString::fromLatin1(kAppId) : QString::fromLatin1(env);
}

QString safeAddress(const QString &url)
{
    const QUrl u(url);
    return u.isValid() ? u.scheme() + QStringLiteral("://") + u.host() + QStringLiteral("/…") : QStringLiteral("(invalid)");
}

void *tag(int generation) { return reinterpret_cast<void *>(static_cast<intptr_t>(generation)); }
int untag(void *ctx) { return static_cast<int>(reinterpret_cast<intptr_t>(ctx)); }

// The bus's thread -> Qt's.
bool onLoadReplyCb(LSHandle *, LSMessage *msg, void *ctx)
{
    const int generation = untag(ctx);
    const QByteArray payload(LSMessageGetPayload(msg));
    QMetaObject::invokeMethod(TvPlayer::instance(), "handleLoadReply", Qt::QueuedConnection,
                              Q_ARG(int, generation), Q_ARG(QByteArray, payload));
    return true;
}

bool onEventCb(LSHandle *, LSMessage *msg, void *ctx)
{
    const int generation = untag(ctx);
    const QByteArray payload(LSMessageGetPayload(msg));
    QMetaObject::invokeMethod(TvPlayer::instance(), "handleEvent", Qt::QueuedConnection,
                              Q_ARG(int, generation), Q_ARG(QByteArray, payload));
    return true;
}

bool onReplyLogCb(LSHandle *, LSMessage *msg, void *ctx)
{
    const QByteArray payload(LSMessageGetPayload(msg));
    // unload answers false once the subscription's end has already let go of the stream.
    if (!payload.contains("\"returnValue\":true") && qstrcmp(static_cast<const char *>(ctx), "unload") != 0)
        qInfo().noquote() << "TV player:" << static_cast<const char *>(ctx) << "answered" << payload.left(200);
    return true;
}
} // namespace

TvPlayer *TvPlayer::instance()
{
    static TvPlayer *player = new TvPlayer;   // lives as long as the app
    return player;
}

TvPlayer::TvPlayer() : QObject(QCoreApplication::instance()) {}

bool TvPlayer::ensureWindow()
{
    if (!m_windowId.isEmpty()) return true;
    // The TV's video plane, under Relay's screens, placed with setVideoRect.
    const char *id = SDL_webOSCreateExportedWindow(SDL_WEBOS_EXPORED_WINDOW_TYPE_VIDEO);
    if (!id) {
        qWarning().noquote() << "TV player: no video window:" << SDL_GetError();
        return false;
    }
    m_windowId = QString::fromLatin1(id);
    setVideoRect(m_videoRect);
    return true;
}

void TvPlayer::setVideoRect(const QRect &rect)
{
    m_videoRect = rect.isValid() ? rect : QRect(0, 0, 1920, 1080);
    if (m_windowId.isEmpty()) return;
    SDL_Rect src{0, 0, 1920, 1080};
    SDL_Rect dst{m_videoRect.x(), m_videoRect.y(), m_videoRect.width(), m_videoRect.height()};
    SDL_webOSSetExportedWindow(m_windowId.toLatin1().constData(), &src, &dst);
}

void TvPlayer::call(const char *method, const QJsonObject &args)
{
    LSHandle *ls = tvLuna();
    if (!ls) return;
    const QByteArray uri = QByteArray("luna://com.webos.media/") + method;
    const QByteArray payload = QJsonDocument(args).toJson(QJsonDocument::Compact);
    LSError err;
    LSErrorInit(&err);
    if (!LSCallOneReply(ls, uri.constData(), payload.constData(), onReplyLogCb, const_cast<char *>(method), nullptr, &err)) {
        qWarning().noquote() << "TV player:" << method << "couldn't be sent:" << (err.message ? err.message : "");
        LSErrorFree(&err);
    }
}

void TvPlayer::load(QObject *owner, const QString &url, const QString &userAgent, bool vod, bool direct)
{
    unload();
    m_owner = owner;
    m_vod = vod;
    LSHandle *ls = tvLuna();
    if (!ls) { emit failed(tr("Relay couldn't reach the TV's player.")); return; }
    if (!ensureWindow()) { emit failed(tr("The TV gave Relay no video window.")); return; }

    const bool hls = QUrl(url).path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
    QJsonObject option{{QStringLiteral("appId"), appId()}, {QStringLiteral("windowId"), m_windowId}};
    QString uri;
    if (direct) {
        // The TV's player reads the file as it plays (and seeks with ranges),
        // so nothing of it passes through Relay's memory.
        uri = url;
        if (!userAgent.isEmpty())
            option.insert(QStringLiteral("transmission"),
                          QJsonObject{{QStringLiteral("httpHeader"), QJsonObject{{QStringLiteral("userAgent"), userAgent}}}});
    } else {
        // Through Relay's stream relay: one connection to the provider with the
        // channel's user agent, from Relay, as on a computer.
        if (!m_relay) {
            m_relay = new CastRelay(this);
            m_relay->setListenAddress(QHostAddress::LocalHost);   // only the TV's own player connects
        }
        CastRelay::Source source;
        source.url = url;
        source.userAgent = userAgent;
        if (hls) source.contentType = QStringLiteral("application/vnd.apple.mpegurl");
        else if (!vod) { source.contentType = QStringLiteral("video/mp2t"); source.shareLive = true; }
        m_relayPath = m_relay->add(source);
        if (m_relayPath.isEmpty()) { emit failed(tr("Relay's stream relay didn't start.")); return; }
        uri = QStringLiteral("http://127.0.0.1:%1%2").arg(m_relay->port()).arg(m_relayPath);
    }

    QJsonObject inner{{QStringLiteral("option"), option}};
    if (hls) inner.insert(QStringLiteral("mediaTransportType"), QStringLiteral("HLS"));
    const QJsonObject args{{QStringLiteral("uri"), uri}, {QStringLiteral("type"), QStringLiteral("media")},
                           {QStringLiteral("payload"), inner}};
    const QByteArray payload = QJsonDocument(args).toJson(QJsonDocument::Compact);

    const int generation = ++m_generation;
    m_playSent = false;
    m_loadClock.start();
    qInfo().noquote() << "TV player: loading" << safeAddress(url) << (vod ? "(movie or episode)" : hls ? "(live, HLS)" : "(live)")
                      << (direct ? "directly" : "through the relay");
    LSError err;
    LSErrorInit(&err);
    if (!LSCallOneReply(ls, "luna://com.webos.media/load", payload.constData(), onLoadReplyCb, tag(generation), nullptr, &err)) {
        const QString why = QString::fromUtf8(err.message ? err.message : "");
        LSErrorFree(&err);
        emit failed(tr("The TV's player didn't answer (%1).").arg(why));
        return;
    }
    // Some firmware never tells a late subscriber the load completed: play anyway.
    QTimer::singleShot(3000, this, [this, generation] {
        if (generation != m_generation || m_mediaId.isEmpty() || m_playSent) return;
        m_playSent = true;
        call("play", {{QStringLiteral("mediaId"), m_mediaId}});
    });
}

void TvPlayer::handleLoadReply(int generation, const QByteArray &payload)
{
    const QJsonObject reply = QJsonDocument::fromJson(payload).object();
    const QString id = reply.value(QStringLiteral("mediaId")).toString();
    if (generation != m_generation) {   // something newer was asked for meanwhile
        if (!id.isEmpty()) call("unload", {{QStringLiteral("mediaId"), id}});
        return;
    }
    if (id.isEmpty()) {
        const QString text = reply.value(QStringLiteral("errorText")).toString();
        qWarning().noquote() << "TV player: load refused:" << payload.left(300);
        emit failed(text.isEmpty() ? tr("The TV's player refused this stream.") : tr("The TV's player refused this stream (%1).").arg(text));
        return;
    }
    m_mediaId = id;
    LSError err;
    LSErrorInit(&err);
    LSMessageToken token = 0;
    const QByteArray args = QJsonDocument(QJsonObject{{QStringLiteral("mediaId"), id}}).toJson(QJsonDocument::Compact);
    if (LSCall(tvLuna(), "luna://com.webos.media/subscribe", args.constData(), onEventCb, tag(generation), &token, &err))
        m_subscription = token;
    else {
        qWarning().noquote() << "TV player: no events:" << (err.message ? err.message : "");
        LSErrorFree(&err);
    }
    call("setVolume", {{QStringLiteral("mediaId"), id}, {QStringLiteral("volume"), m_volume}});
}

void TvPlayer::handleEvent(int generation, const QByteArray &payload)
{
    if (generation != m_generation) return;
    const QJsonObject e = QJsonDocument::fromJson(payload).object();
    const QString kind = e.keys().value(0);
    // What the TV sends, once each (not the clock ticks).
    if (!kind.isEmpty() && !m_seenEvents.contains(kind) && kind != QLatin1String("currentTime")
        && kind != QLatin1String("bufferRange")) {
        m_seenEvents << kind;
        qInfo().noquote() << "TV player: event" << payload.left(400);
    }

    if (e.contains(QStringLiteral("loadCompleted"))) {
        emit loaded();
        if (!m_playSent) {
            m_playSent = true;
            call("play", {{QStringLiteral("mediaId"), m_mediaId}});
        }
    }
    if (e.contains(QStringLiteral("playing"))) {
        if (m_loadClock.isValid()) {
            qInfo().noquote() << "TV player: playing after" << m_loadClock.elapsed() << "ms";
            m_loadClock.invalidate();
        }
        emit playing();
    }
    if (e.contains(QStringLiteral("paused"))) emit paused();
    if (e.contains(QStringLiteral("bufferingStart"))) emit buffering(true);
    if (e.contains(QStringLiteral("bufferingEnd"))) emit buffering(false);
    if (e.contains(QStringLiteral("endOfStream"))) emit ended();
    if (e.contains(QStringLiteral("currentTime"))) {
        // {"currentTime":{"currentTime":10183,"mediaId":…}}, in milliseconds.
        // Read as a plain number it was 0, and movies stayed at 0:00.
        const QJsonValue t = e.value(QStringLiteral("currentTime"));
        const double ms = t.isObject() ? t.toObject().value(QStringLiteral("currentTime")).toDouble() : t.toDouble();
        emit positionChanged(ms / 1000.0);
    }
    if (e.contains(QStringLiteral("error"))) {
        const QJsonObject err = e.value(QStringLiteral("error")).toObject();
        const QString text = err.value(QStringLiteral("errorText")).toString();
        const int code = err.value(QStringLiteral("errorCode")).toInt();
        emit failed(text.isEmpty() ? tr("The TV's player stopped (error %1).").arg(code)
                                   : tr("The TV's player stopped: %1 (%2).").arg(text).arg(code));
    }
    if (e.contains(QStringLiteral("sourceInfo"))) {
        const QJsonObject info = e.value(QStringLiteral("sourceInfo")).toObject();
        const QJsonObject program = info.value(QStringLiteral("programInfo")).toArray().first().toObject();
        double durationMs = info.value(QStringLiteral("duration")).toDouble();
        if (durationMs <= 0) durationMs = program.value(QStringLiteral("duration")).toDouble();
        if (m_vod && durationMs > 0) emit durationChanged(durationMs / 1000.0);
        const QJsonObject v = program.value(QStringLiteral("video_streams")).toArray().first().toObject();
        const QJsonObject a = program.value(QStringLiteral("audio_streams")).toArray().first().toObject();
        emit videoInfo(v.value(QStringLiteral("width")).toInt(), v.value(QStringLiteral("height")).toInt(),
                       v.value(QStringLiteral("frame_rate")).toDouble(), v.value(QStringLiteral("codec")).toVariant().toString(),
                       a.value(QStringLiteral("codec")).toVariant().toString());
    }
    if (e.contains(QStringLiteral("videoInfo"))) {
        const QJsonObject v = e.value(QStringLiteral("videoInfo")).toObject();
        emit videoInfo(v.value(QStringLiteral("width")).toInt(), v.value(QStringLiteral("height")).toInt(),
                       v.value(QStringLiteral("frameRate")).toDouble(), QString(), QString());
    }
}

void TvPlayer::unload()
{
    ++m_generation;
    LSHandle *ls = tvLuna();
    if (ls && m_subscription) {
        LSError err;
        LSErrorInit(&err);
        if (!LSCallCancel(ls, m_subscription, &err)) LSErrorFree(&err);
    }
    m_subscription = 0;
    if (!m_mediaId.isEmpty()) {
        call("unload", {{QStringLiteral("mediaId"), m_mediaId}});
        qInfo().noquote() << "TV player: stopped";
    }
    m_mediaId.clear();
    stopRelay();
    m_owner = nullptr;
}

void TvPlayer::stopRelay()
{
    if (m_relay && !m_relayPath.isEmpty()) m_relay->remove(m_relayPath);
    m_relayPath.clear();
}

void TvPlayer::play() { if (!m_mediaId.isEmpty()) call("play", {{QStringLiteral("mediaId"), m_mediaId}}); }
void TvPlayer::pause() { if (!m_mediaId.isEmpty()) call("pause", {{QStringLiteral("mediaId"), m_mediaId}}); }

void TvPlayer::seek(double seconds)
{
    if (m_mediaId.isEmpty()) return;
    call("seek", {{QStringLiteral("mediaId"), m_mediaId}, {QStringLiteral("position"), qint64(seconds * 1000)}});
}

void TvPlayer::setVolume(int percent)
{
    m_volume = qBound(0, percent, 100);
    if (!m_mediaId.isEmpty()) call("setVolume", {{QStringLiteral("mediaId"), m_mediaId}, {QStringLiteral("volume"), m_volume}});
}
