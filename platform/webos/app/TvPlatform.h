#pragma once

// Relay on webOS, before Qt starts: SDL's full-screen window and OpenGL ES
// context, Qt's "sdl" platform (platform/webos/qpa-sdl), and where Relay keeps
// its files. Called first thing in Relay's main() (src/main.cpp).
//
// Returns false when the TV gave no window; Relay then can't run.
bool tvPrepare();

#include <QByteArray>

// The folder Relay keeps its files in on the TV (settings, guide, logs,
// crash.log, freeze.log); set by tvPrepare().
QByteArray tvDataBase();
