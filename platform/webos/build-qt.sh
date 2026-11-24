#!/bin/bash
# Builds the Qt that Relay uses on webOS, plus the matching tools for this
# computer: static Core, Network, Gui (OpenGL ES 2), QML and Qt Quick for the
# TV. Use the same Qt version MSYS2 gives the Windows build, so both share one
# Qt.
#
# Usage: platform/webos/build-qt.sh <webos-sdk-dir> <work-dir> [qt-version]
# Needs: cmake, ninja, perl, patch, curl, xz and a host C++ compiler.
# Result: <work-dir>/qt-target and <work-dir>/qt-host, the last two arguments
# of build-spike.sh.
#
# qt-patches/<version>/ holds SHA256SUMS for the source archives and the
# patches, named <module>-<n>-<what>.patch. The SDK compiles against old glibc
# headers (2.12) although the TV runs a newer glibc; the patches cover what
# those headers lack.
#
# No windowing system plugin is built for the TV: SDL keeps the window, input
# and video layer, and Qt Quick draws into SDL's GL context through
# QQuickRenderControl (platform/webos/qpa-sdl).
set -euo pipefail
[ $# -ge 2 ] || { sed -n '7p' "$0" >&2; exit 2; }
SDK=$(cd "$1" && pwd)
mkdir -p "$2"
WORK=$(cd "$2" && pwd)
V=${3:-6.12.0}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCHES=$HERE/qt-patches/$V
[ -f "$PATCHES/SHA256SUMS" ] || { echo "No checksums and patches for Qt $V in $PATCHES" >&2; exit 1; }
MODULES=(qtbase qtshadertools qtdeclarative)

for m in "${MODULES[@]}"; do
  NAME=$m-everywhere-src-$V.tar.xz
  if [ ! -f "$WORK/$NAME" ]; then
    curl -fsSL -o "$WORK/$NAME.part" "https://download.qt.io/official_releases/qt/${V%.*}/$V/submodules/$NAME" \
      || curl -fsSL -o "$WORK/$NAME.part" "https://download.qt.io/archive/qt/${V%.*}/$V/submodules/$NAME"
    mv "$WORK/$NAME.part" "$WORK/$NAME"
  fi
  grep -q "  $NAME\$" "$PATCHES/SHA256SUMS" || { echo "No checksum for $NAME in $PATCHES/SHA256SUMS" >&2; exit 1; }
done
( cd "$WORK" && sha256sum -c "$PATCHES/SHA256SUMS" )

SRC=$WORK/src
HOST=$WORK/qt-host
TARGET=$WORK/qt-target
rm -rf "$SRC" "$WORK/build" "$HOST" "$TARGET"
for m in "${MODULES[@]}"; do
  mkdir -p "$SRC/$m"
  tar -xJf "$WORK/$m-everywhere-src-$V.tar.xz" -C "$SRC/$m" --strip-components=1
  for p in "$PATCHES/$m"-*.patch; do
    [ -e "$p" ] || continue
    patch -d "$SRC/$m" -p1 --no-backup-if-mismatch < "$p"
  done
done

COMMON=(-release -no-pch -nomake examples -nomake tests -no-widgets -no-dbus -no-glib -no-icu
        -no-feature-sql -qt-zlib -qt-pcre -no-zstd -no-feature-brotli -no-feature-libproxy
        -no-feature-gssapi -no-libudev -no-xcb -no-feature-vulkan -no-eglfs -no-linuxfb
        -no-libinput -no-evdev -no-tslib -no-mtdev -no-xkbcommon -qt-harfbuzz -qt-libpng
        -qt-libjpeg)

module() {   # <qt-prefix> <module> [cmake args...]
  local qt=$1 m=$2; shift 2
  mkdir -p "$WORK/build/$(basename "$qt")-$m"
  ( cd "$WORK/build/$(basename "$qt")-$m"
    "$qt/bin/qt-configure-module" "$SRC/$m" -- "$@"
    cmake --build . --parallel
    cmake --install . )
}

# Tools for this computer (moc, qsb, qmlcachegen and the rest).
mkdir -p "$WORK/build/host-qtbase"
( cd "$WORK/build/host-qtbase"
  "$SRC/qtbase/configure" -prefix "$HOST" "${COMMON[@]}" -no-opengl -no-openssl -qt-freetype -no-fontconfig
  cmake --build . --parallel
  cmake --install . )
module "$HOST" qtshadertools
module "$HOST" qtdeclarative

# The SDK's toolchain hides packages outside its sysroot.
cat > "$WORK/webos-qt-toolchain.cmake" <<TOOLCHAIN
include("$SDK/share/buildroot/toolchainfile.cmake")
list(APPEND CMAKE_FIND_ROOT_PATH "$HOST" "$TARGET")
TOOLCHAIN

HOSTARGS=(-DQT_HOST_PATH="$HOST" -DQT_HOST_PATH_CMAKE_DIR="$HOST/lib/cmake")
mkdir -p "$WORK/build/target-qtbase"
( cd "$WORK/build/target-qtbase"
  "$SRC/qtbase/configure" -prefix "$TARGET" -static "${COMMON[@]}" -opengl es2 -fontconfig -system-freetype \
    -no-feature-wayland -no-feature-qtwaylandscanner -openssl-linked -qt-host-path "$HOST" -- \
    -DCMAKE_TOOLCHAIN_FILE="$WORK/webos-qt-toolchain.cmake" \
    -DQT_HOST_PATH_CMAKE_DIR="$HOST/lib/cmake" -DOPENSSL_USE_STATIC_LIBS=ON
  cmake --build . --parallel
  cmake --install . )
module "$TARGET" qtshadertools "${HOSTARGS[@]}"
module "$TARGET" qtdeclarative "${HOSTARGS[@]}" -DFEATURE_qml_debug=OFF

echo "Qt $V for webOS: $TARGET (tools: $HOST)"
