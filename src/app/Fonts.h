#pragma once

// Relay's bundled font (Inter), so text looks the same on every system.
// Characters Inter doesn't have still come from the system's own fonts.
// Call after creating the QGuiApplication and before loading QML.
void useBundledFonts();
