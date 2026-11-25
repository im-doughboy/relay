#pragma once

// A Qt platform plugin ("sdl") for Relay on webOS. SDL keeps the window, input
// and the video layer; Qt only gets SDL's OpenGL ES context. There is no
// Wayland plugin.
//
// Two ways to use it:
// - Relay: a QWindow (Relay's QML Window) is SDL's window. Qt Quick draws on
//   the main thread into SDL's context and swaps through SDL, and the plugin
//   turns SDL's input into Qt's. The window is always full screen.
// - The spike: Qt Quick draws through QQuickRenderControl into a texture and
//   the spike reads SDL's input itself. Its window is never shown, so the
//   plugin leaves SDL's events alone.

#include <QSize>

struct SDL_Window;
typedef void *SDL_GLContext;

// Call before creating QGuiApplication. `size` is the screen Qt draws (1080p).
void qsdlSetWindow(SDL_Window *window, SDL_GLContext context, const QSize &size);

// Whether the app is on screen and has the remote (not behind the TV's home
// screen or another app), from SDL's window events.
bool qsdlOnScreen();
