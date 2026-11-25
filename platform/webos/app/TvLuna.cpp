#include "TvLuna.h"

#include <dlfcn.h>
#include <glib.h>
#include <luna-service2/lunaservice.h>

namespace {
LSHandle *g_handle = nullptr;
} // namespace

LSHandle *tvLuna() { return g_handle; }

std::string tvLunaRegister(const char *appId)
{
    LSError err;
    LSErrorInit(&err);
    LSHandle *h = nullptr;
    if (!LSRegister(appId, &h, &err)) {
        std::string m = err.message ? err.message : "LSRegister failed";
        LSErrorFree(&err);
        return m;
    }
    // Its own GLib loop on its own thread: Qt's event loop isn't GLib's here.
    GMainContext *ctx = g_main_context_new();
    if (!LSGmainContextAttach(h, ctx, &err)) {
        std::string m = err.message ? err.message : "LSGmainContextAttach failed";
        LSErrorFree(&err);
        return m;
    }
    g_handle = h;
    g_thread_new("luna", [](gpointer loop) -> gpointer {
        g_main_loop_run(static_cast<GMainLoop *>(loop));
        return nullptr;
    }, g_main_loop_new(ctx, FALSE));
    // SDL uses the same registration (the app's name can be registered once).
    // Looked up at run time: the SDK's SDL doesn't declare this TV function.
    auto setHandle = reinterpret_cast<int (*)(LSHandle *)>(dlsym(RTLD_DEFAULT, "SDL_webOSSetLSHandle"));
    if (!setHandle) return "SDL_webOSSetLSHandle not found";
    setHandle(h);
    return {};
}
