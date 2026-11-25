#include "TvPlatform.h"

#include "TvLuna.h"
#include "qsdlintegration.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_webOS.h>

#include <QByteArray>
#include <QSize>
#include <QtPlugin>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

Q_IMPORT_PLUGIN(QSdlIntegrationPlugin)

namespace {
// Before Relay's own log exists (it starts once Qt does).
void early(const char *what, const char *detail = "")
{
    std::fprintf(stderr, "Relay TV: %s%s%s\n", what, *detail ? ": " : "", detail);
}

// A crash leaves its address in <data>/crash.log (with where the program was
// loaded), to look up in the unstripped relay.debug that build-app.sh keeps.
char g_crashPath[512];

void writeAll(int fd, const char *s) { if (write(fd, s, strlen(s)) < 0) {} }

void onCrash(int sig, siginfo_t *, void *context)
{
    const int fd = open(g_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        const auto *uc = static_cast<const ucontext_t *>(context);
        char line[160];
        snprintf(line, sizeof line, "Relay crashed: signal %d at pc %08lx lr %08lx\n", sig,
                 static_cast<unsigned long>(uc->uc_mcontext.arm_pc), static_cast<unsigned long>(uc->uc_mcontext.arm_lr));
        writeAll(fd, line);
        const int maps = open("/proc/self/maps", O_RDONLY);
        if (maps >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(maps, buf, sizeof buf)) > 0)
                if (write(fd, buf, size_t(n)) < 0) break;
            close(maps);
        }
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void makeDir(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); ++i)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}
} // namespace

bool tvPrepare()
{
    // Relay's files. The app's own folder (HOME) is replaced when a new
    // version is installed, so settings, logins and the guide live in the
    // developer storage, which keeps them across updates and restarts.
    const char *override = std::getenv("RELAY_DATA");
    const std::string base = override && *override ? override : "/media/developer/temp/relay";
    for (const char *sub : { "/config", "/data", "/cache", "/videos" }) makeDir(base + sub);
    snprintf(g_crashPath, sizeof g_crashPath, "%s/crash.log", base.c_str());
    struct sigaction sa{};
    sa.sa_sigaction = onCrash;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT }) sigaction(sig, &sa, nullptr);
    setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
    setenv("XDG_DATA_HOME", (base + "/data").c_str(), 1);
    setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
    setenv("XDG_VIDEOS_DIR", (base + "/videos").c_str(), 1);
    // The TV starts apps in the plain "C" locale; Qt needs UTF-8 for file
    // names and text (glibc has C.UTF-8 built in).
    setenv("LC_ALL", "C.UTF-8", 1);

    // The TV's services first, shared with SDL: without this the TV keeps
    // the remote's Back button, and its player can't be reached.
    const char *appId = std::getenv("APPID");
    const std::string lunaError = tvLunaRegister(appId && *appId ? appId : "com.imdoughboy.relay");
    if (!lunaError.empty()) early("no connection to the TV's services", lunaError.c_str());
    // Once Relay has shut down (Qt gone, settings saved), end at once: the
    // bus's thread is still running, and the library's own clean-up at exit
    // crashed under it (in libluna-service2, as the TV closed Relay).
    std::atexit([] {
        std::fflush(nullptr);
        _exit(0);
    });

    // The remote's Back key comes to Relay (as Escape) instead of closing it.
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
    SDL_SetHint(SDL_HINT_WEBOS_REGISTER_APP, "true");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { early("SDL didn't start", SDL_GetError()); return false; }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);     // see-through where the video plays
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);    // Qt Quick sorts opaque parts with it
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);   // clipping
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Relay's screens are drawn at 1080p and the TV scales them to its 4K
    // panel: much lighter on the TV, and the video has its own full-resolution layer.
    const QSize size(1920, 1080);
    SDL_Window *window = SDL_CreateWindow("Relay", 0, 0, size.width(), size.height(),
                                          SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
    if (!window) { early("no window", SDL_GetError()); return false; }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) { early("no OpenGL ES context", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(1);
    SDL_webOSCursorVisibility(SDL_TRUE);   // LG's Magic Remote pointer
    qsdlSetWindow(window, context, size);

    setenv("QT_QPA_PLATFORM", "sdl", 1);
    setenv("QSG_RHI_BACKEND", "opengl", 1);
    setenv("QSG_RENDER_LOOP", "basic", 1);   // Qt Quick draws on the main thread, with SDL
    setenv("QT_QUICK_CONTROLS_STYLE", "Basic", 1);
    return true;
}
