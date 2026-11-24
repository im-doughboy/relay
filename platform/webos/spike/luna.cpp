// Luna mode: hand the stream address to the TV's own media player through the
// com.webos.media service, the way webOS web apps (and the webOS IPTV Player)
// play TS and HLS natively. The TV then demuxes, decodes and keeps time
// itself. The uMediaClient library hung for us, so this talks to the service
// directly with luna-service2.

#include "feed.h"

#include <dlfcn.h>
#include <glib.h>
#include <luna-service2/lunaservice.h>

#include <string>
#include <unistd.h>

static LSHandle *g_lunaHandle = nullptr;
static GMainContext *g_lunaContext = nullptr;

// Must run before SDL_Init. The app's bus name can only be held once, so the
// spike registers it here and gives the same connection to SDL.
std::string lunaEarlyInit(const char *appId)
{
    LSError err; LSErrorInit(&err);
    LSHandle *h = nullptr;
    if (!LSRegister(appId, &h, &err)) {
        std::string m = err.message ? err.message : "LSRegister failed";
        LSErrorFree(&err);
        return m;
    }
    GMainContext *ctx = g_main_context_new();
    if (!LSGmainContextAttach(h, ctx, &err)) {
        std::string m = err.message ? err.message : "LSGmainContextAttach failed";
        LSErrorFree(&err);
        return m;
    }
    g_lunaHandle = h;
    g_lunaContext = ctx;
    g_thread_new("luna", [](gpointer l) -> gpointer {
        g_main_loop_run(static_cast<GMainLoop *>(l));
        return nullptr;
    }, g_main_loop_new(ctx, FALSE));
    // Looked up at run time: the SDK's SDL does not declare this TV function.
    auto setHandle = reinterpret_cast<int (*)(LSHandle *)>(dlsym(RTLD_DEFAULT, "SDL_webOSSetLSHandle"));
    if (!setHandle) return "SDL_webOSSetLSHandle not found";
    setHandle(h);
    return {};
}

namespace {

void addTimeout(GMainContext *ctx, guint ms, GSourceFunc fn, gpointer data)
{
    GSource *src = g_timeout_source_new(ms);
    g_source_set_callback(src, fn, data, nullptr);
    g_source_attach(src, ctx);
    g_source_unref(src);
}

struct LunaState {
    const FeedConfig *cfg;
    const FeedHooks *hooks;
    std::atomic<bool> *quit;
    LSHandle *ls = nullptr;
    GMainLoop *loop = nullptr;
    GMainContext *ctx = nullptr;
    std::string mediaId;
    int events = 0;
    bool played = false;
    std::atomic<bool> closed{false};
};

std::string esc(const std::string &s)
{
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

// Small helper: the value of "key":"..." in a JSON reply.
std::string jsonString(const std::string &j, const std::string &key)
{
    auto k = j.find("\"" + key + "\"");
    if (k == std::string::npos) return {};
    auto q1 = j.find('"', j.find(':', k) + 1);
    auto q2 = q1 == std::string::npos ? q1 : j.find('"', q1 + 1);
    return q2 == std::string::npos ? std::string() : j.substr(q1 + 1, q2 - q1 - 1);
}

void call(LunaState *st, const char *method, const std::string &payload, LSFilterFunc cb)
{
    LSError err; LSErrorInit(&err);
    std::string uri = std::string("luna://com.webos.media/") + method;
    if (!LSCall(st->ls, uri.c_str(), payload.c_str(), cb, st, nullptr, &err)) {
        st->hooks->log(std::string("luna call ") + method + " failed: " + (err.message ? err.message : "?"));
        LSErrorFree(&err);
    }
}

bool onPlayReply(LSHandle *, LSMessage *msg, void *data)
{
    auto *st = static_cast<LunaState *>(data);
    if (st->closed) return true;
    st->hooks->log(std::string("play reply ") + LSMessageGetPayload(msg));
    return true;
}

bool onEvent(LSHandle *, LSMessage *msg, void *data)
{
    auto *st = static_cast<LunaState *>(data);
    if (st->closed) return true;
    std::string p = LSMessageGetPayload(msg);
    bool noisy = p.find("currentTime") != std::string::npos || p.find("bufferRange") != std::string::npos;
    if (!noisy && st->events++ < 60) st->hooks->log("event " + p.substr(0, 400));
    if (p.find("loadCompleted") != std::string::npos && !st->played) {
        st->played = true;
        st->hooks->status(1, "Loaded, starting", "");
        call(st, "play", "{\"mediaId\":\"" + st->mediaId + "\"}", onPlayReply);
    }
    if (p.find("\"playing\"") != std::string::npos) st->hooks->status(2, "Playing (TV player)", "");
    if (p.find("\"error\"") != std::string::npos) {
        std::string code = jsonString(p, "errorText");
        st->hooks->status(3, "TV player error", code.empty() ? p.substr(0, 120) : code);
    }
    if (p.find("endOfStream") != std::string::npos) st->hooks->status(3, "Stream ended", "");
    return true;
}

bool onLoadReply(LSHandle *, LSMessage *msg, void *data)
{
    auto *st = static_cast<LunaState *>(data);
    if (st->closed) return true;
    std::string p = LSMessageGetPayload(msg);
    st->hooks->log("load reply " + p);
    st->mediaId = jsonString(p, "mediaId");
    if (st->mediaId.empty()) {
        st->hooks->status(3, "The TV player refused the stream", jsonString(p, "errorText"));
        return true;
    }
    call(st, "subscribe", "{\"mediaId\":\"" + st->mediaId + "\"}", onEvent);
    // Some firmware never sends loadCompleted for a subscriber that joins
    // late; start playing anyway after a moment.
    addTimeout(st->ctx, 3000, [](gpointer d) -> gboolean {
        auto *s = static_cast<LunaState *>(d);
        if (s->closed) return G_SOURCE_REMOVE;
        if (!s->played && !s->mediaId.empty()) {
            s->played = true;
            s->hooks->log("no loadCompleted yet, sending play");
            call(s, "play", "{\"mediaId\":\"" + s->mediaId + "\"}", onPlayReply);
        }
        return G_SOURCE_REMOVE;
    }, st);
    return true;
}

// Xtream address host/user/pass/123 -> host/live/user/pass/123.m3u8
std::string toHls(const std::string &url)
{
    auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    auto pathAt = url.find('/', scheme + 3);
    if (pathAt == std::string::npos) return {};
    std::string host = url.substr(0, pathAt), path = url.substr(pathAt + 1);
    if (path.rfind("live/", 0) == 0) path = path.substr(5);
    auto dot = path.rfind('.');
    if (dot != std::string::npos && path.find('/', dot) == std::string::npos) path = path.substr(0, dot);
    int slashes = 0; for (char c : path) if (c == '/') slashes++;
    if (slashes != 2) return {};
    return host + "/live/" + path + ".m3u8";
}

} // namespace

void runLuna(const FeedConfig &cfg, const FeedHooks &hooks, std::atomic<bool> &quit)
{
    static LunaState st;
    st.cfg = &cfg; st.hooks = &hooks; st.quit = &quit;
    if (!g_lunaHandle) { hooks.status(3, "Could not reach the TV's services", ""); return; }
    st.ls = g_lunaHandle;
    st.ctx = g_lunaContext;
    hooks.log("luna using the app connection");

    std::string uri = cfg.url;
    std::string transport = cfg.transport;
    if (cfg.hls) {
        std::string h = toHls(cfg.url);
        if (!h.empty()) { uri = h; if (transport.empty()) transport = "HLS"; }
        else hooks.log("could not turn this address into an HLS one, using it as is");
    }
    std::string option = "\"appId\":\"" + esc(cfg.appId) + "\"";
    if (!cfg.windowId.empty()) option += ",\"windowId\":\"" + esc(cfg.windowId) + "\"";
    option += ",\"transmission\":{\"httpHeader\":{\"userAgent\":\"" + esc(cfg.userAgent) + "\"}}";
    std::string payload = "{\"uri\":\"" + esc(uri) + "\",\"type\":\"media\",\"payload\":{";
    if (!transport.empty()) payload += "\"mediaTransportType\":\"" + esc(transport) + "\",";
    payload += "\"option\":{" + option + "}}}";
    hooks.log("luna load " + payload);
    hooks.status(1, std::string("Loading in the TV player") + (cfg.hls ? " (HLS)" : ""), cfg.name);
    call(&st, "load", payload, onLoadReply);

    while (!quit.load()) usleep(100000);

    if (!st.mediaId.empty()) {
        LSError e2; LSErrorInit(&e2);
        LSCallOneReply(st.ls, "luna://com.webos.media/unload", ("{\"mediaId\":\"" + st.mediaId + "\"}").c_str(),
                       nullptr, nullptr, nullptr, &e2);
        LSErrorFree(&e2);
        // Let the unload go out before the process ends.
        usleep(200000);
    }
    st.closed = true;
    hooks.log("luna stopped");
}
