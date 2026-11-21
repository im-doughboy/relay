#pragma once

#include <QString>

// Matches playlist channels to guide channels when tvg-id is missing or wrong.
namespace EpgMatcher {

// Reduces a channel name to a comparable key:
// "US: CNN HD" -> "cnn", "[UK] BBC One FHD" -> "bbcone", "Sky Cinema +1" -> "skycinemaplus1".
QString normalize(const QString &name);

} // namespace EpgMatcher
