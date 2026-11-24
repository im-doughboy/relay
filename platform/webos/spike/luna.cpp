// Luna mode: hand the stream address to the TV's own media player through the
// com.webos.media service, the way webOS web apps (and the webOS IPTV Player)
// play TS and HLS natively. The TV then demuxes, decodes and keeps time
// itself. The uMediaClient library hung for us, so this talks to the service
// directly with luna-service2.

#include "feed.h"

#include <dlfcn.h>
#include <glib.h>
#include <luna-service2/lunaservice.h>

#include <chrono>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
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
    std::mutex lock;
    std::string mediaId;
    LSMessageToken subscription = 0;
    std::atomic<int> phase{0};       // 0 waiting, 1 playing, 2 failed
    std::atomic<int> generation{0};  // which load replies belong to
    int events = 0;
    std::atomic<bool> played{false};
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

struct LoadContext { LunaState *st; int generation; };

std::string currentMedia(LunaState *st)
{
    std::lock_guard<std::mutex> g(st->lock);
    return st->mediaId;
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
    const std::string id = currentMedia(st);
    auto at = p.find("\"mediaId\"");
    if (at != std::string::npos && (id.empty() || p.find("\"" + id + "\"", at) == std::string::npos)) return true;
    bool noisy = p.find("currentTime") != std::string::npos || p.find("bufferRange") != std::string::npos;
    if (!noisy && st->events++ < 200) st->hooks->log("event " + p.substr(0, 400));
    if (p.find("loadCompleted") != std::string::npos && !st->played.exchange(true)) {
        st->hooks->status(1, "Loaded, starting", "");
        call(st, "play", "{\"mediaId\":\"" + id + "\"}", onPlayReply);
    }
    if (p.find("\"playing\"") != std::string::npos) {
        st->phase = 1;
        st->hooks->status(2, "Playing (TV player)", "");
    }
    if (p.find("\"error\"") != std::string::npos) {
        st->phase = 2;
        std::string code = jsonString(p, "errorText");
        st->hooks->status(3, "TV player error", code.empty() ? p.substr(0, 120) : code);
    }
    if (p.find("endOfStream") != std::string::npos) st->hooks->status(3, "Stream ended", "");
    return true;
}

bool onLoadReply(LSHandle *sh, LSMessage *msg, void *data)
{
    std::unique_ptr<LoadContext> ctx(static_cast<LoadContext *>(data));
    LunaState *st = ctx->st;
    if (st->closed) return true;
    std::string p = LSMessageGetPayload(msg);
    st->hooks->log("load reply " + p);
    const std::string id = jsonString(p, "mediaId");
    if (ctx->generation != st->generation) {
        if (!id.empty()) {
            LSError e; LSErrorInit(&e);
            if (!LSCallOneReply(sh, "luna://com.webos.media/unload", ("{\"mediaId\":\"" + id + "\"}").c_str(), nullptr, nullptr, nullptr, &e)) LSErrorFree(&e);
        }
        return true;
    }
    if (id.empty()) {
        st->phase = 2;
        st->hooks->status(3, "The TV player refused the stream", jsonString(p, "errorText"));
        return true;
    }
    { std::lock_guard<std::mutex> g(st->lock); st->mediaId = id; }
    LSError err; LSErrorInit(&err);
    LSMessageToken token = 0;
    if (LSCall(st->ls, "luna://com.webos.media/subscribe", ("{\"mediaId\":\"" + id + "\"}").c_str(), onEvent, st, &token, &err)) {
        std::lock_guard<std::mutex> g(st->lock);
        st->subscription = token;
    } else {
        st->hooks->log(std::string("subscribe failed: ") + (err.message ? err.message : "?"));
        LSErrorFree(&err);
    }
    // Some firmware never sends loadCompleted for a subscriber that joins
    // late; start playing anyway after a moment.
    addTimeout(st->ctx, 3000, [](gpointer d) -> gboolean {
        std::unique_ptr<LoadContext> c(static_cast<LoadContext *>(d));
        LunaState *s = c->st;
        if (s->closed || c->generation != s->generation) return G_SOURCE_REMOVE;
        if (!s->played.exchange(true)) {
            s->hooks->log("no loadCompleted yet, sending play");
            call(s, "play", "{\"mediaId\":\"" + currentMedia(s) + "\"}", onPlayReply);
        }
        return G_SOURCE_REMOVE;
    }, new LoadContext{st, ctx->generation});
    return true;
}

// Relay's stream relay, running as a child process (relay-helper).
class RelayHelper {
public:
    ~RelayHelper() { stop(); }

    bool start(const FeedHooks &hooks)
    {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) return false;
        std::string path(exe, n);
        path = path.substr(0, path.rfind('/')) + "/relay-helper";
        int toChild[2], fromChild[2];
        if (pipe2(toChild, O_CLOEXEC) != 0) return false;
        if (pipe2(fromChild, O_CLOEXEC) != 0) { close(toChild[0]); close(toChild[1]); return false; }
        m_pid = fork();
        if (m_pid == 0) {
            dup2(toChild[0], 0);
            dup2(fromChild[1], 1);
            int log = open("/tmp/relay-helper.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (log >= 0) dup2(log, 2);
            for (int fd = 3; fd < 1024; fd++) close(fd);
            execl(path.c_str(), "relay-helper", (char *)nullptr);
            _exit(127);
        }
        close(toChild[0]);
        close(fromChild[1]);
        m_in = toChild[1];
        m_out = fromChild[0];
        if (m_pid < 0) { stop(); return false; }
        std::string line = readLine(8000);
        if (line != "ready") {
            hooks.log("relay helper did not start: " + line);
            stop();
            return false;
        }
        return true;
    }

    // The address for the TV player, or empty.
    std::string add(bool hls, const std::string &userAgent, const std::string &url, std::string &path)
    {
        std::string a = request(std::string("add\t") + (hls ? "hls" : "ts") + "\t" + userAgent + "\t" + url);
        auto t1 = a.find('\t');
        auto t2 = t1 == std::string::npos ? t1 : a.find('\t', t1 + 1);
        if (a.rfind("ok\t", 0) != 0 || t2 == std::string::npos) return {};
        path = a.substr(t2 + 1);
        return "http://127.0.0.1:" + a.substr(t1 + 1, t2 - t1 - 1) + path;
    }

    void remove(const std::string &path) { if (!path.empty()) request("remove\t" + path); }

    void stop()
    {
        if (m_in >= 0) { close(m_in); m_in = -1; }
        if (m_out >= 0) { close(m_out); m_out = -1; }
        if (m_pid <= 0) return;
        for (int i = 0; i < 20; i++) {
            if (waitpid(m_pid, nullptr, WNOHANG) == m_pid) { m_pid = -1; return; }
            usleep(50000);
        }
        kill(m_pid, SIGKILL);
        waitpid(m_pid, nullptr, 0);
        m_pid = -1;
    }

private:
    std::string request(const std::string &line)
    {
        if (m_in < 0) return {};
        const std::string out = line + "\n";
        size_t done = 0;
        while (done < out.size()) {
            ssize_t n = write(m_in, out.data() + done, out.size() - done);
            if (n <= 0) return {};
            done += size_t(n);
        }
        return readLine(5000);
    }

    std::string readLine(int timeoutMs)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto nl = m_buffer.find('\n');
            if (nl != std::string::npos) {
                std::string line = m_buffer.substr(0, nl);
                m_buffer.erase(0, nl + 1);
                return line;
            }
            int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
            if (left <= 0 || m_out < 0) return {};
            pollfd p{m_out, POLLIN, 0};
            if (poll(&p, 1, left) <= 0) return {};
            char chunk[1024];
            ssize_t n = read(m_out, chunk, sizeof chunk);
            if (n <= 0) return {};
            m_buffer.append(chunk, size_t(n));
        }
    }

    pid_t m_pid = -1;
    int m_in = -1, m_out = -1;
    std::string m_buffer;
};

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

    std::vector<std::pair<std::string, std::string>> list = cfg.channels;
    if (list.empty()) list.push_back({cfg.url, cfg.name});
    auto sourceFor = [&](const std::string &url, std::string &transport) {
        transport = cfg.transport;
        if (!cfg.hls) return url;
        std::string h = toHls(url);
        if (h.empty()) { hooks.log("could not turn this address into an HLS one, using it as is"); return url; }
        if (transport.empty()) transport = "HLS";
        return h;
    };

    RelayHelper relay;
    if (cfg.relay && !relay.start(hooks)) {
        hooks.status(3, "Could not start the stream relay", "");
        return;
    }

    auto unload = [&] {
        std::string id;
        LSMessageToken token;
        {
            std::lock_guard<std::mutex> g(st.lock);
            id.swap(st.mediaId);
            token = st.subscription;
            st.subscription = 0;
        }
        st.generation++;
        LSError e; LSErrorInit(&e);
        if (token && !LSCallCancel(st.ls, token, &e)) { LSErrorFree(&e); LSErrorInit(&e); }
        if (!id.empty() && !LSCallOneReply(st.ls, "luna://com.webos.media/unload", ("{\"mediaId\":\"" + id + "\"}").c_str(),
                                           nullptr, nullptr, nullptr, &e))
            LSErrorFree(&e);
    };

    const int total = cfg.switches + 1;
    std::string relayPath;
    for (int i = 0; i < total && !quit.load(); i++) {
        const auto &channel = list[size_t(i) % list.size()];
        std::string transport;
        const std::string source = sourceFor(channel.first, transport);
        std::string uri = source;
        if (cfg.relay) {
            uri = relay.add(cfg.hls, cfg.userAgent, source, relayPath);
            if (uri.empty()) { hooks.status(3, "The stream relay refused the stream", ""); break; }
        }
        std::string option = "\"appId\":\"" + esc(cfg.appId) + "\"";
        if (!cfg.windowId.empty()) option += ",\"windowId\":\"" + esc(cfg.windowId) + "\"";
        option += ",\"transmission\":{\"httpHeader\":{\"userAgent\":\"" + esc(cfg.userAgent) + "\"}}";
        std::string payload = "{\"uri\":\"" + esc(uri) + "\",\"type\":\"media\",\"payload\":{";
        if (!transport.empty()) payload += "\"mediaTransportType\":\"" + esc(transport) + "\",";
        payload += "\"option\":{" + option + "}}}";
        hooks.log("luna load " + payload);
        hooks.status(1, std::string("Loading in the TV player") + (cfg.relay ? " (relay)" : "") + (cfg.hls ? " (HLS)" : ""), channel.second);

        st.phase = 0;
        st.played = false;
        const int generation = ++st.generation;
        auto *ctx = new LoadContext{&st, generation};
        const auto began = std::chrono::steady_clock::now();
        LSError err; LSErrorInit(&err);
        if (!LSCall(st.ls, "luna://com.webos.media/load", payload.c_str(), onLoadReply, ctx, nullptr, &err)) {
            hooks.log(std::string("luna call load failed: ") + (err.message ? err.message : "?"));
            LSErrorFree(&err);
            delete ctx;
            st.phase = 2;
        }
        while (!quit.load() && st.phase == 0
               && std::chrono::steady_clock::now() - began < std::chrono::seconds(20))
            usleep(20000);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        const int phase = st.phase;
        hooks.log("load " + std::to_string(i + 1) + "/" + std::to_string(total) + " " + channel.second + ": "
                  + (phase == 1 ? "playing after " : phase == 2 ? "FAILED after " : "NO ANSWER after ") + std::to_string(ms) + " ms");
        if (i + 1 >= total) break;
        for (int w = 0; w < cfg.switchHoldMs / 50 && !quit.load(); w++) usleep(50000);
        unload();
        if (cfg.relay) { relay.remove(relayPath); relayPath.clear(); }
    }
    if (total > 1) hooks.log("switch test finished");

    while (!quit.load()) usleep(100000);
    unload();
    usleep(200000);
    st.closed = true;
    relay.stop();
    hooks.log("luna stopped");
}
