#!/bin/bash
# Builds Relay for webOS and packs it as an IPK.
# Usage: platform/webos/build-app.sh <webos-sdk-dir> <out-dir> <qt-prefix> <qt-host-prefix>
# <qt-prefix> is Qt built for the TV and <qt-host-prefix> the matching Qt tools
# for this computer (build-qt.sh makes both). The SDK is openlgtv/buildroot-nc4's
# arm-webos-linux-gnueabi_sdk-buildroot (run its relocate-sdk.sh once after unpacking).
set -euo pipefail
[ $# -ge 4 ] || { sed -n '3p' "$0" >&2; exit 2; }
SDK=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd)
QT=$(cd "$3" && pwd); QT_HOST=$(cd "$4" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ID=com.imdoughboy.relay
VER=$(sed -nE 's/^project\(Relay VERSION ([0-9.]+).*/\1/p' "$HERE/../../CMakeLists.txt")
SR=$SDK/arm-webos-linux-gnueabi/sysroot
STRIP=$SDK/bin/arm-webos-linux-gnueabi-strip
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
BUILD=${RELAY_WEBOS_BUILD:-$OUT/build}   # kept between runs, so rebuilds are quick
APP=$STAGE/data/usr/palm/applications/$ID
mkdir -p "$APP/lib" "$STAGE/data/usr/palm/packages/$ID" "$STAGE/control"

"$QT/bin/qt-cmake" -S "$HERE/app" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQT_HOST_PATH="$QT_HOST" -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST/lib/cmake" -DWEBOS_SYSROOT="$SR"
cmake --build "$BUILD" --parallel
cp "$BUILD/relay" "$APP/"
cp "$BUILD/relay" "$OUT/relay.debug"   # unstripped, for reading crash addresses
"$STRIP" "$APP/relay"
# The TV's own libstdc++ can be older than this compiler's, so ship ours.
cp -L "$SR/usr/lib/libstdc++.so.6" "$APP/lib/"
"$STRIP" "$APP/lib/libstdc++.so.6"
# And the OpenSSL Relay's Qt was built against (3.6): the TV's own is an older
# 3.0, and a build for a newer OpenSSL mustn't run on an older one. Relay
# loads this first, so the TV's libraries in the same process use it too
# (built for 3.0, they're fine on 3.6) and there's one OpenSSL.
for l in libssl.so.3 libcrypto.so.3; do
  cp -L "$SR/usr/lib/$l" "$APP/lib/"
  "$STRIP" "$APP/lib/$l"
done

cat > "$APP/appinfo.json" <<J
{
  "id": "$ID",
  "version": "$VER",
  "vendor": "im-doughboy",
  "type": "native",
  "main": "relay",
  "title": "Relay",
  "icon": "icon.png",
  "largeIcon": "largeIcon.png",
  "nativeLifeCycleInterfaceVersion": 2,
  "requiredPermissions": ["media.operation"]
}
J
python3 - "$HERE/../../packaging/icons/relay.png" "$APP" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
im.resize((80, 80), Image.LANCZOS).save(sys.argv[2] + "/icon.png")
im.resize((130, 130), Image.LANCZOS).save(sys.argv[2] + "/largeIcon.png")
PY
cat > "$STAGE/data/usr/palm/packages/$ID/packageinfo.json" <<J
{ "app": "$ID", "id": "$ID", "loc_name": "Relay", "package_format_version": 2, "vendor": "im-doughboy", "version": "$VER" }
J
SIZE=$(du -sk "$STAGE/data" | cut -f1)
cat > "$STAGE/control/control" <<C
Package: $ID
Version: $VER
Section: misc
Priority: optional
Architecture: arm
Installed-Size: $SIZE
Maintainer: im-doughboy
Description: Relay, live TV and movies from your playlist
webOS-Package-Format-Version: 2
webOS-Packager-Version: x.y.x
C
echo "2.0" > "$STAGE/debian-binary"
( cd "$STAGE/control" && tar --owner=0 --group=0 -czf ../control.tar.gz ./control )
( cd "$STAGE/data" && tar --owner=0 --group=0 -czf ../data.tar.gz ./usr )
IPK=$OUT/${ID}_${VER}_arm.ipk
rm -f "$IPK"
( cd "$STAGE" && ar rc "$IPK" debian-binary control.tar.gz data.tar.gz )
echo "$IPK"
