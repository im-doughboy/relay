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
//   window=1                  1 = hand the video an exported window (webOS 5+)
// Log: /tmp/relay-spike.log (and the app dir if /tmp is not writable).

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_webOS.h>
#include <uMediaClient.h>

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

using uMediaServer::uMediaClient;

class Player : public uMediaClient {
public:
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
    spikeLog("Relay webOS spike, app dir %s", g_appDir.c_str());

    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
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

    auto conf = readConf();
    std::string url = conf["url"];
    std::string type = conf.count("type") ? conf["type"] : "media";
    bool useWindow = !conf.count("window") || conf["window"] != "0";

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

    Player *player = nullptr;
    std::thread umsThread;
    if (url.empty()) {
        setStatus(3, "No stream set", "Put url=... in /tmp/relay-spike.conf and reopen");
    } else {
        player = new Player();
        std::string opt = std::string("{\"option\":{\"appId\":\"") + kAppId + "\"";
        if (!windowId.empty()) opt += ",\"windowId\":\"" + jsonEscape(windowId) + "\"";
        if (conf.count("transport")) opt += ",\"mediaTransportType\":\"" + jsonEscape(conf["transport"]) + "\"";
        opt += "}}";
        spikeLog("load %s type=%s payload=%s", url.c_str(), type.c_str(), opt.c_str());
        setStatus(1, "Loading", url);
        if (!player->loadAsync(url, type, opt)) setStatus(3, "Load refused");
        umsThread = std::thread([player] { player->run(); });
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
                if (player && e.key.keysym.sym == SDLK_RETURN) {
                    if (g_state == 2) player->pause(); else player->play();
                }
            }
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
        if (g_state == 2 && SDL_GetTicks() - start > 8000) bar = {60, 1000, 600, 50};
        SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
        SDL_RenderFillRect(ren, &bar);
        drawText(ren, font, "Relay spike: " + st, bar.x + 20, bar.y + 8);
        if (bar.h > 60) drawText(ren, font, de, bar.x + 20, bar.y + 60);
        SDL_RenderPresent(ren);
    }

    spikeLog("closing");
    if (player) { player->unload(); player->stop(); }
    if (umsThread.joinable()) umsThread.join();
    if (!windowId.empty()) SDL_webOSDestroyExportedWindow(windowId.c_str());
    SDL_Quit();
    return 0;
}
