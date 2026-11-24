#!/bin/bash
# Builds the Qt that Relay uses on webOS: static Core and Network for the TV,
# plus the matching tools for this computer. Use the same Qt version MSYS2
# gives the Windows build, so both share one Qt.
#
# Usage: platform/webos/build-qt.sh <webos-sdk-dir> <work-dir> [qt-version]
# Needs: cmake, ninja, perl, patch, curl, xz and a host C++ compiler.
# Result: <work-dir>/qt-target and <work-dir>/qt-host, the last two arguments
# of build-spike.sh.
#
# qt-patches/<version>/ holds the source checksum and the patches. The SDK
# compiles against old glibc headers (2.12) although the TV runs a newer glibc;
# the patches cover what those headers lack.
set -euo pipefail
[ $# -ge 2 ] || { sed -n '6p' "$0" >&2; exit 2; }
SDK=$(cd "$1" && pwd)
mkdir -p "$2"
WORK=$(cd "$2" && pwd)
V=${3:-6.12.0}
HERE=$(cd "$(dirname "$0")" && pwd)
PATCHES=$HERE/qt-patches/$V
[ -f "$PATCHES/SHA256" ] || { echo "No checksum and patches for Qt $V in $PATCHES" >&2; exit 1; }

TARBALL=$WORK/qtbase-everywhere-src-$V.tar.xz
if [ ! -f "$TARBALL" ]; then
  NAME=$(basename "$TARBALL")
  curl -fsSL -o "$TARBALL.part" "https://download.qt.io/official_releases/qt/${V%.*}/$V/submodules/$NAME" \
    || curl -fsSL -o "$TARBALL.part" "https://download.qt.io/archive/qt/${V%.*}/$V/submodules/$NAME"
  mv "$TARBALL.part" "$TARBALL"
fi
echo "$(cat "$PATCHES/SHA256")  $TARBALL" | sha256sum -c -

SRC=$WORK/qtbase-src
HOST=$WORK/qt-host
TARGET=$WORK/qt-target
rm -rf "$SRC" "$WORK/build-host" "$WORK/build-target" "$HOST" "$TARGET"
mkdir -p "$SRC" "$WORK/build-host" "$WORK/build-target"
tar -xJf "$TARBALL" -C "$SRC" --strip-components=1
for p in "$PATCHES"/*.patch; do
  patch -d "$SRC" -p1 --no-backup-if-mismatch < "$p"
done

COMMON=(-release -no-pch -nomake examples -nomake tests -no-gui -no-widgets -no-dbus
        -no-glib -no-icu -no-feature-sql -qt-zlib -qt-pcre -no-zstd -no-feature-brotli
        -no-feature-libproxy -no-feature-gssapi -no-libudev)

( cd "$WORK/build-host"
  "$SRC/configure" -prefix "$HOST" "${COMMON[@]}" -no-openssl
  cmake --build . --parallel
  cmake --install . )

# The SDK's toolchain hides packages outside its sysroot.
cat > "$WORK/webos-qt-toolchain.cmake" <<TOOLCHAIN
include("$SDK/share/buildroot/toolchainfile.cmake")
list(APPEND CMAKE_FIND_ROOT_PATH "$HOST" "$TARGET")
TOOLCHAIN

( cd "$WORK/build-target"
  "$SRC/configure" -prefix "$TARGET" -static "${COMMON[@]}" -no-feature-qtwaylandscanner \
    -openssl-linked -qt-host-path "$HOST" -- \
    -DCMAKE_TOOLCHAIN_FILE="$WORK/webos-qt-toolchain.cmake" \
    -DQT_HOST_PATH_CMAKE_DIR="$HOST/lib/cmake" -DOPENSSL_USE_STATIC_LIBS=ON
  cmake --build . --parallel
  cmake --install . )

echo "Qt $V for webOS: $TARGET (tools: $HOST)"
