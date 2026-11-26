#pragma once

#include "Channel.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

// A playlist's channels, saved after each load so Relay can open on them at
// once next time and fetch the new list in the background (Session).
//
// The file holds no login. Each of `secrets` (the password, then the
// username) is replaced by a marker when saving and put back when reading,
// so the saved channels also work after the password changes.
namespace SavedPlaylist {

// Writes `result`'s live channels, and its movie and episode entries if it
// has any, for the playlist `source` identifies. False if it couldn't (or an
// address already holds one of the markers).
bool save(const QString &path, const PlaylistResult &result, const QByteArray &source,
          const QStringList &secrets);

// The channels saved for `source`, with `secrets` put back and savedAt and
// digest filled in. False when there are none, the file is damaged, or it
// was saved for another source.
bool read(const QString &path, const QByteArray &source, const QStringList &secrets,
          PlaylistResult *result);

// A fingerprint of the live channels, their groups' order and the guide
// address: the same for two loads that brought the same channels.
QByteArray digest(const PlaylistResult &result);

// What identifies a playlist (its type and server or address), with
// `secrets` masked: the same after a password change, and no login in it.
QByteArray sourceKey(const QString &description, const QStringList &secrets);

} // namespace SavedPlaylist
