#pragma once

#include <functional>
#include <string>

struct SDL_Window;

struct UiStatus {
    int state = 0;   // 0 idle, 1 loading, 2 playing, 3 error
    std::string status, detail;
};

// ui=1: Qt Quick draws the screen into SDL's GL context, over the video.
// Returns when the viewer presses Back or the app is closed.
void runUi(SDL_Window *window, const std::function<UiStatus()> &status,
           const std::function<void(const std::string &)> &log);
