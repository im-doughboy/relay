#!/usr/bin/env bash
# Package Relay for macOS (Homebrew Qt and mpv):
#
#     ./packaging/package-macos.sh
#
# Makes dist/Relay.app (with Qt and mpv inside) and dist/Relay-<version>-macos.dmg.
# To update, open the new .dmg and drag Relay over the old one; your playlists
# and settings live in ~/Library and are kept.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD
VERSION=$(sed -n 's/^project(Relay VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
QT_PREFIX=$(brew --prefix qt)
BUILD=build-release
DIST=$ROOT/dist

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DRELAY_BUILD_TESTS=OFF \
      -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD" -j

rm -rf "$DIST/Relay.app"
mkdir -p "$DIST"
cmake --install "$BUILD" --prefix "$DIST"

# Qt, QML modules and the Homebrew libraries (mpv and what it uses).
"$QT_PREFIX/bin/macdeployqt" "$DIST/Relay.app" -qmldir="$ROOT/qml" -always-overwrite

# Changed binaries need signing again to run on Apple silicon (ad hoc: fine for
# your own Macs; others will need to right-click > Open the first time).
codesign --force --deep --sign - "$DIST/Relay.app"

DMG=$DIST/Relay-$VERSION-macos.dmg
rm -f "$DMG"
STAGE=$(mktemp -d)
cp -R "$DIST/Relay.app" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "Relay $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
rm -rf "$STAGE"
echo "Done: $DMG"
