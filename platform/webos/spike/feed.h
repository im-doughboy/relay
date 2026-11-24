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
};

struct FeedHooks {
    std::function<void(const std::string &)> log;
    std::function<void(int state, const std::string &status, const std::string &detail)> status;
    std::function<void(const char *name)> counter;
};

// Plays `cfg.url` until `quit` is set. `control` holds the StarfishMediaAPIs
// object while playing, so the UI thread can pause and resume it.
void runFeed(const FeedConfig &cfg, const FeedHooks &hooks, std::atomic<bool> &quit,
             std::atomic<void *> &control);
