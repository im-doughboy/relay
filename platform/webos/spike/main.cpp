// Relay webOS spike: proves the TV's hardware video path before any Relay
// screens are ported. It plays one stream through the TV's own media server
// (uMediaServer) under a transparent SDL window and draws a status bar on top.
//
// Settings come from a text file (first one found wins):
//   /tmp/relay-spike.conf, /media/developer/temp/relay-spike.conf, <app dir>/spike.conf
// Lines are key=value:
//   url=http://...            the stream to play
//   type=media                uMediaServer media type (default "media")
//   transport=                optional mediaTransportType, e.g. HLS or URI
//   mode=feed                 feed (default): FFmpeg reads, NDL decodes; luna: the TV's own player via
//                             com.webos.media (hls=1 asks for the .m3u8 version); ums: uMediaClient
//   ua=VLC/3.0                user agent sent to the provider (default Relay/0.5.2)
//   audiodelay=-150           NDL: move the sound against the picture, in ms (negative = earlier)
//   pace=pts                  NDL: pace video by display time instead of decode time
//   test=C                    NDL: stick to one feeding test (A-D) instead of cycling
//   audio=0                   feed mode: video only
//   backend=ndl               ndl (default), smp (Starfish), or auto (Starfish, then NDL)
//   window=1                  1 = hand the video an exported window (webOS 5+)
//   channel=ESPN              with a playlist url: play the first live channel whose name has this
//   bus=private               media server bus (default public); conn= sets a connection id
// Log: /tmp/relay-spike.log (and the app dir if /tmp is not writable).

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_webOS.h>
#include <uMediaClient.h>
#include <StarfishMediaAPIs.h>
#include "feed.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <csignal>
#include <execinfo.h>
#include <vector>

static const char *kAppId = "com.imdoughboy.relay.spike";
static std::string g_appDir = ".";
static FILE *g_logFile = nullptr;
static std::mutex g_mx;
static std::string g_status = "Starting";
static std::string g_detail;
static std::atomic<int> g_state{0}; // 0 idle, 1 loading, 2 playing, 3 error

static void spikeLog(const char *fmt, ...)
{
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_logFile) return;
    time_t t = time(nullptr);
    char ts[32]; strftime(ts, sizeof ts, "%H:%M:%S", localtime(&t));
    fprintf(g_logFile, "%s ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(g_logFile, fmt, ap); va_end(ap);
    fputc('\n', g_logFile); fflush(g_logFile);
}

static void setStatus(int state, const std::string &s, const std::string &d = {})
{
    { std::lock_guard<std::mutex> l(g_mx); g_status = s; if (!d.empty()) g_detail = d; }
    g_state = state;
    spikeLog("[status] %s %s", s.c_str(), d.c_str());
}

static std::map<std::string, std::string> readConf()
{
    std::map<std::string, std::string> c;
    const std::string paths[] = { "/tmp/relay-spike.conf",
                                  "/media/developer/temp/relay-spike.conf",
                                  g_appDir + "/spike.conf" };
    for (const auto &p : paths) {
        std::ifstream f(p);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
            c[line.substr(0, eq)] = line.substr(eq + 1);
        }
        spikeLog("settings from %s", p.c_str());
        c["_from"] = p;
        break;
    }
    return c;
}

static std::string jsonEscape(const std::string &s)
{
    std::string o;
    for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
    return o;
}


static int g_logFd = -1;
static void onCrash(int sig)
{
    if (g_logFd >= 0) {
        char m[64]; int n = snprintf(m, sizeof m, "CRASH signal %d\n", sig);
        if (write(g_logFd, m, n) < 0) {}
        void *frames[48]; int c = backtrace(frames, 48);
        backtrace_symbols_fd(frames, c, g_logFd);
    }
    signal(sig, SIG_DFL); raise(sig);
}

// Fetches a URL with the TV's curl or wget (keeps the spike free of extra libraries).
static std::string fetch(const std::string &url)
{
    std::string q = "'"; for (char c : url) { if (c == '\'') q += "'\\''"; else q += c; } q += "'";
    for (std::string cmd : { "curl -sL --max-time 60 -A 'Relay' " + q, "wget -q -T 60 -O - " + q }) {
        FILE *p = popen((cmd + " 2>/dev/null").c_str(), "r");
        if (!p) continue;
        std::string out; char buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
        if (!out.empty()) return out;
    }
    return {};
}

static bool looksLikePlaylist(const std::string &u)
{
    return u.find("get.php") != std::string::npos || u.find("type=m3u") != std::string::npos
        || (u.size() > 4 && (u.compare(u.size() - 4, 4, ".m3u") == 0));
}

static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }

// Picks a live channel from an M3U: the first whose name contains `want`
// (any live channel when `want` is empty). Films and series are skipped.
static std::string pickChannel(const std::string &m3u, const std::string &want, std::string &name)
{
    size_t pos = 0; std::string lastName; int seen = 0;
    while (pos < m3u.size()) {
        size_t e = m3u.find('\n', pos); if (e == std::string::npos) e = m3u.size();
        std::string line = m3u.substr(pos, e - pos); pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("#EXTINF", 0) == 0) {
            size_t c = line.rfind(',');
            lastName = c == std::string::npos ? "" : line.substr(c + 1);
        } else if (!line.empty() && line[0] != '#') {
            seen++;
            if (line.find("/movie/") != std::string::npos || line.find("/series/") != std::string::npos) continue;
            if (!want.empty() && lower(lastName).find(lower(want)) == std::string::npos) continue;
            name = lastName;
            spikeLog("playlist has %d entries so far, chose \"%s\"", seen, lastName.c_str());
            return line;
        }
    }
    spikeLog("no matching live channel among %d entries", seen);
    return {};
}


// Logs what the provider answers before FFmpeg tries, so a refusal shows its
// real HTTP code. Only the status lines are logged (the address has the login).
static void probeHttp(const std::string &url, const std::string &ua)
{
    std::string q = "'"; for (char c : url) { if (c == '\'') q += "'\\''"; else q += c; } q += "'";
    // One short request (3 s), so it doesn't hold a provider connection slot.
    std::string cmd = "curl -s -k -L -o /dev/null --max-time 3 -A '" + ua + "' -D - " + q
                    + " 2>&1 | grep -iE '^HTTP/|^server:|^content-type:'";
    FILE *p = popen(cmd.c_str(), "r");
    std::string out; char buf[512];
    while (p && fgets(buf, sizeof buf, p)) { std::string l(buf); while (!l.empty() && (l.back()=='\n'||l.back()=='\r')) l.pop_back(); out += (out.empty() ? "" : " | ") + l; }
    if (p) pclose(p);
    spikeLog("http check: %s", out.empty() ? "(no answer)" : out.c_str());
    sleep(1);
}

using uMediaServer::uMediaClient;

class Player : public uMediaClient {
public:
    Player(UMSConnectorBusType bus, const std::string &conn) : uMediaClient(false, bus, conn) {}
    bool onLoadCompleted() override { setStatus(1, "Loaded, starting"); play(); return true; }
    bool onPlaying() override { setStatus(2, "Playing"); return true; }
    bool onPaused() override { setStatus(1, "Paused"); return true; }
    bool onBufferingStart() override { spikeLog("buffering start"); return true; }
    bool onBufferingEnd() override { spikeLog("buffering end"); return true; }
    bool onEndOfStream() override { setStatus(3, "Stream ended"); return true; }
    bool onError(int64_t code, const std::string &text) override
    {
        setStatus(3, "Error " + std::to_string(code), text);
        return true;
    }
#if UMS_INTERNAL_API_VERSION == 2
    bool onVideoInfo(const ums::video_info_t &v) override
    {
        char b[128];
        snprintf(b, sizeof b, "%ux%u %.2ffps %s", v.width, v.height,
                 (double)v.frame_rate.num / (v.frame_rate.den ? v.frame_rate.den : 1), v.codec.c_str());
        { std::lock_guard<std::mutex> l(g_mx); g_detail = b; }
        spikeLog("video %s", b);
        return true;
    }
    bool onAudioInfo(const ums::audio_info_t &a) override
    {
        spikeLog("audio %u Hz %s", a.sample_rate, a.codec.c_str());
        return true;
    }
#else
    bool onVideoInfo(const uMediaServer::video_info_t &v) override
    {
        char b[128];
        snprintf(b, sizeof b, "%dx%d %.2ffps", v.width, v.height, v.frameRate);
        { std::lock_guard<std::mutex> l(g_mx); g_detail = b; }
        spikeLog("video %s", b);
        return true;
    }
    bool onAudioInfo(const uMediaServer::audio_info_t &a) override
    {
        spikeLog("audio %.0f Hz %d ch", a.sampleRate, a.channels);
        return true;
    }
#endif
    bool onUserDefinedChanged(const char *m) override { spikeLog("event %s", m); return true; }
};

static TTF_Font *openFont(int size)
{
    const char *fonts[] = {
        "/usr/share/fonts/LG_Smart_UI-Regular.ttf",
        "/usr/share/fonts/LGSmartUI-Regular.ttf",
        "/usr/share/fonts/MuseoSans-Medium.ttf",
        "/usr/share/fonts/DejaVuSans.ttf",
    };
    for (const char *f : fonts)
        if (TTF_Font *ft = TTF_OpenFont(f, size)) { spikeLog("font %s", f); return ft; }
    // Any .ttf in /usr/share/fonts will do.
    FILE *p = popen("ls /usr/share/fonts/*.ttf /usr/share/fonts/*/*.ttf 2>/dev/null", "r");
    char line[512];
    while (p && fgets(line, sizeof line, p)) {
        std::string f(line); while (!f.empty() && (f.back() == '\n')) f.pop_back();
        if (TTF_Font *ft = TTF_OpenFont(f.c_str(), size)) { spikeLog("font %s", f.c_str()); pclose(p); return ft; }
    }
    if (p) pclose(p);
    spikeLog("no font found, status shown as colour only");
    return nullptr;
}

static void drawText(SDL_Renderer *r, TTF_Font *f, const std::string &s, int x, int y)
{
    if (!f || s.empty()) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(r, surf);
    SDL_Rect d{x, y, surf->w, surf->h};
    SDL_RenderCopy(r, t, nullptr, &d);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surf);
}

int main(int argc, char **argv)
{
    char *base = SDL_GetBasePath();
    if (base) { g_appDir = base; SDL_free(base); }
    g_logFile = fopen("/tmp/relay-spike.log", "w");
    if (!g_logFile) g_logFile = fopen((g_appDir + "/relay-spike.log").c_str(), "w");
    if (g_logFile) g_logFd = fileno(g_logFile);
    for (int sig : { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE }) signal(sig, onCrash);
    signal(SIGPIPE, SIG_IGN);
    spikeLog("Relay webOS spike 0.0.14, app dir %s", g_appDir.c_str());

    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    auto conf = readConf();
    if (conf["mode"] == "luna") {
        std::string lunaError = lunaEarlyInit(kAppId);
        spikeLog("luna early init: %s", lunaError.empty() ? "ok" : lunaError.c_str());
    }
    SDL_SetHint(SDL_HINT_WEBOS_REGISTER_APP, "true");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { spikeLog("SDL_Init: %s", SDL_GetError()); return 1; }
    TTF_Init();

    int pw = 1920, ph = 1080, hz = 0;
    SDL_webOSGetPanelResolution(&pw, &ph);
    SDL_webOSGetRefreshRate(&hz);
    spikeLog("panel %dx%d @%d", pw, ph, hz);

    // The video plane sits under the UI, so the UI must be cleared to alpha 0.
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_Window *win = SDL_CreateWindow("Relay", 0, 0, 1920, 1080, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
    if (!win) { spikeLog("window: %s", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) { spikeLog("renderer: %s", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_webOSCursorVisibility(SDL_FALSE);
    TTF_Font *font = openFont(34);

    std::string url = conf["url"];
    std::string type = conf.count("type") ? conf["type"] : "media";
    // 0.0.6 got "601 Resource Allocation Error" for every load while an
    // exported video window existed, so it is now opt-in (window=1).
    std::string mode = conf.count("mode") ? conf["mode"] : "feed";
    // The TV's own player (mode=luna) needs a video window to draw into.
    bool useWindow = conf["window"] == "1" || (mode == "luna" && conf["window"] != "0");
    {
        const char *vars[] = { "APPID", "APP_ID", "SDL_WEBOS_APP_ID", "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "HOME", "LS_SERVICE_NAMES" };
        for (const char *v : vars) spikeLog("env %s=%s", v, getenv(v) ? getenv(v) : "(unset)");
        spikeLog("uid %d gid %d", (int)getuid(), (int)getgid());
    }

    std::string windowId;
    if (useWindow) {
        const char *id = SDL_webOSCreateExportedWindow(SDL_WEBOS_EXPORED_WINDOW_TYPE_VIDEO);
        if (id) {
            windowId = id;
            SDL_Rect src{0, 0, 1920, 1080}, dst{0, 0, 1920, 1080};
            SDL_webOSSetExportedWindow(id, &src, &dst);
            spikeLog("exported window %s", id);
        } else {
            spikeLog("no exported window (%s); older webOS uses the main window", SDL_GetError());
        }
    }

    // Everything that talks to the network or the media server runs on its
    // own thread, so the screen keeps showing what is happening.
    std::atomic<Player *> player{nullptr};
    std::atomic<bool> quitting{false};
    std::atomic<Uint32> loadStarted{0};
    std::atomic<void *> feedControl{nullptr};
    std::atomic<int> dropped{0};
    spikeLog("mode %s", mode.c_str());
    std::atomic<bool> workerDone{false};
    std::thread umsThread;
    if (url.empty()) {
        setStatus(3, "No stream set", "Put url=... in /tmp/relay-spike.conf and reopen");
    } else {
        umsThread = std::thread([&, url, type] {
            struct Done { std::atomic<bool> &d; ~Done() { d = true; } } done{workerDone};
            std::string stream = url, name;
            if (looksLikePlaylist(url)) {
                setStatus(1, "Downloading playlist");
                std::string m3u = fetch(url);
                spikeLog("playlist %zu bytes", m3u.size());
                if (m3u.empty()) { setStatus(3, "Could not download the playlist", "curl/wget failed or the server refused"); return; }
                stream = pickChannel(m3u, conf["channel"], name);
                if (stream.empty()) { setStatus(3, "No live channel found in the playlist"); return; }
            }
            if (quitting) return;
            if (mode != "ums") {
                FeedConfig fc;
                fc.url = stream; fc.name = name; fc.appId = kAppId; fc.windowId = windowId;
                fc.audio = conf["audio"] != "0";
                if (conf.count("ua")) fc.userAgent = conf["ua"];
                if (conf.count("pace")) fc.pace = conf["pace"];
                if (conf.count("test")) fc.test = conf["test"];
                if (conf.count("audiodelay")) fc.audioDelayMs = atoi(conf["audiodelay"].c_str());
                if (conf.count("variant")) fc.variant = atoi(conf["variant"].c_str());
                fc.backend = conf.count("backend") ? conf["backend"] : "ndl"; // NDL works on webOS 10; "auto" also tries Starfish first
                probeHttp(stream, fc.userAgent);
                FeedHooks hooks;
                hooks.log = [](const std::string &m) { spikeLog("%s", m.c_str()); };
                hooks.status = [](int st, const std::string &a, const std::string &b) { setStatus(st, a, b); };
                hooks.counter = [&dropped](const char *) { dropped++; };
                spikeLog("stream %s", stream.c_str());
                if (mode == "luna") {
                    fc.transport = conf["transport"];
                    fc.hls = conf["hls"] == "1";
                    runLuna(fc, hooks, quitting);
                    return;
                }
                runFeed(fc, hooks, quitting, feedControl);
                return;
            }
            setStatus(1, "Connecting to the TV media server", name);
            loadStarted = SDL_GetTicks();
            spikeLog("creating media client (bus=%s)", conf["bus"].c_str());
            UMSConnectorBusType bus = conf["bus"] == "private" ? UMS_CONNECTOR_PRIVATE_BUS : UMS_CONNECTOR_PUBLIC_BUS;
            Player *p = new Player(bus, conf["conn"]);
            spikeLog("media client ready");
            player = p;
            std::string opt = std::string("{\"option\":{\"appId\":\"") + kAppId + "\"";
            if (!windowId.empty()) opt += ",\"windowId\":\"" + jsonEscape(windowId) + "\"";
            if (conf.count("transport")) opt += ",\"mediaTransportType\":\"" + jsonEscape(conf["transport"]) + "\"";
            opt += "}}";
            spikeLog("load type=%s payload=%s", type.c_str(), opt.c_str());
            spikeLog("stream %s", stream.c_str());
            setStatus(1, "Loading", name.empty() ? std::string("stream") : name);
            loadStarted = SDL_GetTicks();
            if (!p->loadAsync(stream, type, opt)) { setStatus(3, "The media server refused the stream"); }
            spikeLog("load sent, running media events");
            p->run();
            spikeLog("media events stopped");
        });
    }

    bool running = true;
    Uint32 start = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN) {
                spikeLog("key scancode %d sym %d", e.key.keysym.scancode, e.key.keysym.sym);
                if (e.key.keysym.scancode == SDL_WEBOS_SCANCODE_BACK || e.key.keysym.sym == SDLK_ESCAPE)
                    running = false;
                if (auto *sf = static_cast<StarfishMediaAPIs *>(feedControl.load()); sf && e.key.keysym.sym == SDLK_RETURN) {
                    if (g_state == 2) { sf->Pause(); setStatus(1, "Paused"); } else { sf->Play(); setStatus(2, "Playing"); }
                }
                if (Player *p = player.load(); p && e.key.keysym.sym == SDLK_RETURN) {
                    if (g_state == 2) p->pause(); else p->play();
                }
            }
        }
        if (g_state == 1 && loadStarted && SDL_GetTicks() - loadStarted > 20000) {
            setStatus(3, "No answer from the media server after 20 s", "try mode=feed (the default)");
            loadStarted = 0;
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 0);
        SDL_RenderClear(ren);

        std::string st, de;
        { std::lock_guard<std::mutex> l(g_mx); st = g_status; de = g_detail; }
        SDL_Color c = g_state == 2 ? SDL_Color{30, 160, 80, 220}
                     : g_state == 3 ? SDL_Color{200, 50, 50, 220}
                                    : SDL_Color{60, 90, 200, 220};
        SDL_Rect bar{60, 900, 1800, 120};
        // Keep the bar small while playing so the video shows.
        if (g_state == 2 && SDL_GetTicks() - start > 8000) bar = {60, 1000, 900, 50};
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(ren, &bar);
        if (dropped > 0) st += "  (dropped " + std::to_string(dropped.load()) + ")";
        drawText(ren, font, "Relay spike: " + st, bar.x + 20, bar.y + 8);
        if (bar.h > 60) drawText(ren, font, de, bar.x + 20, bar.y + 60);
        SDL_RenderPresent(ren);
    }

    spikeLog("closing");
    quitting = true;
    if (Player *p = player.load()) { p->unload(); p->stop(); }
    // Don't hang on a stuck download or media server; the process is ending anyway.
    // Give the feed a moment to release the TV's decoder cleanly.
    for (int i = 0; i < 30 && !workerDone; i++) SDL_Delay(100);
    if (umsThread.joinable()) { if (workerDone) umsThread.join(); else umsThread.detach(); }
    // Destroying the exported video window by hand crashed in libwayland on
    // webOS 10; it goes away with the window, so just close the window.
    spikeLog("closing window");
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    spikeLog("closing SDL");
    SDL_Quit();
    spikeLog("closed");
    return 0;
}
