#pragma once

// A Qt platform plugin ("sdl") for Relay on webOS. SDL keeps the window, input
// and the video layer; Qt only gets SDL's OpenGL ES context, which Qt Quick
// draws into through QQuickRenderControl. There is no Wayland plugin.

#include <QSize>

struct SDL_Window;
typedef void *SDL_GLContext;

// Call before creating QGuiApplication.
void qsdlSetWindow(SDL_Window *window, SDL_GLContext context, const QSize &size);
