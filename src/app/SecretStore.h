#pragma once

#include <QString>

// Small secrets (the playlist link, which usually contains the login, and
// the Xtream password) in the operating system's credential store:
// Windows Credential Manager or the macOS Keychain. Elsewhere there's no
// store, and callers keep using the settings file.
namespace SecretStore {

bool available();
// Name is like "playlist"; stored as "Relay/<name>".
bool write(const QString &name, const QString &value);
// Empty with *found = false when there's nothing stored.
QString read(const QString &name, bool *found = nullptr);
bool remove(const QString &name);

} // namespace SecretStore
