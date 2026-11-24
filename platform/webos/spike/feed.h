#pragma once
#include <atomic>
#include <functional>
#include <string>

struct FeedConfig {
    std::string url;
    std::string userAgent = "Relay/0.5.2";
    std::string name;
    std::string appId;
    std::string windowId;
    bool audio = true;
    int variant = 0; // 0 = try them all
    std::string backend = "auto";
    std::string pace = "dts";
    int audioDelayMs = 0;
    std::string transport; // luna mode: mediaTransportType, empty = let the TV choose
    bool hls = false;      // luna mode: ask the provider for HLS (.m3u8) instead of TS
    std::string test; // A-D: stick to one NDL feeding test; empty = cycle
};

struct FeedHooks {
    std::function<void(const std::string &)> log;
    std::function<void(int state, const std::string &status, const std::string &detail)> status;
    std::function<void(const char *name)> counter;
};

// Plays `cfg.url` until `quit` is set. `control` holds the StarfishMediaAPIs
// object while playing, so the UI thread can pause and resume it.
// Plays `cfg.url` with the TV's own player (com.webos.media) until `quit`.
std::string lunaEarlyInit(const char *appId);
void runLuna(const FeedConfig &cfg, const FeedHooks &hooks, std::atomic<bool> &quit);

void runFeed(const FeedConfig &cfg, const FeedHooks &hooks, std::atomic<bool> &quit,
             std::atomic<void *> &control);
