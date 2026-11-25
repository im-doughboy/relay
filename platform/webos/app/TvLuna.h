#pragma once

// Relay's connection to the TV's services (the luna bus), made before SDL
// starts and shared with SDL. Without it the TV keeps the remote's Back
// button for itself, and the TV's player (com.webos.media) can't be used.

#include <string>

struct LSHandle;

// Registers `appId` on the bus, runs it on a thread of its own and hands it to
// SDL. Returns an empty string, or what went wrong.
std::string tvLunaRegister(const char *appId);

// The connection, or null before tvLunaRegister succeeded.
LSHandle *tvLuna();
