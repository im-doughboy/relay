#!/bin/bash
# Builds the webOS hardware video spike and packs it as an IPK.
# Usage: platform/webos/build-spike.sh <webos-sdk-dir> <out-dir> <ffmpeg-prefix> <qt-prefix> <qt-host-prefix>
# <qt-prefix> is Qt built for the TV and <qt-host-prefix> the matching Qt tools
# for this computer (build-qt.sh makes both).
# <ffmpeg-prefix> is a static FFmpeg built for the TV (see build-ffmpeg.sh).
# The SDK is openlgtv/buildroot-nc4's arm-webos-linux-gnueabi_sdk-buildroot
# (run its relocate-sdk.sh once after unpacking).
set -euo pipefail
SDK=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd); FF=$(cd "$3" && pwd)
QT=${4:+$(cd "$4" && pwd)}
QT_HOST=${5:+$(cd "$5" && pwd)}
[ -n "$QT" ] && [ -n "$QT_HOST" ] || { sed -n '3p' "$0" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
ID=com.imdoughboy.relay.spike
VER=$(python3 -c "import json,sys;print(json.load(open('$HERE/spike/appinfo.json'))['version'])")
CXX=$SDK/bin/arm-webos-linux-gnueabi-g++
SR=$SDK/arm-webos-linux-gnueabi/sysroot
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
APP=$STAGE/data/usr/palm/applications/$ID
mkdir -p "$APP/lib" "$STAGE/data/usr/palm/packages/$ID" "$STAGE/control"

"$QT/bin/qt-cmake" -S "$HERE/spike" -B "$STAGE/spike" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQT_HOST_PATH="$QT_HOST" -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST/lib/cmake" -DWEBOS_SYSROOT="$SR" -DFFMPEG_PREFIX="$FF"
cmake --build "$STAGE/spike" --parallel
cp "$STAGE/spike/relay-spike" "$APP/"
cp "$STAGE/spike/relay-spike" "$OUT/relay-spike.debug"   # unstripped, for reading crash addresses
"$SDK/bin/arm-webos-linux-gnueabi-strip" "$APP/relay-spike"
# The TV's own libstdc++ can be older than this compiler's, so ship ours.
cp -L "$SR/usr/lib/libstdc++.so.6" "$APP/lib/"
"$SDK/bin/arm-webos-linux-gnueabi-strip" "$APP/lib/libstdc++.so.6"

if [ -n "$QT" ]; then
  "$QT/bin/qt-cmake" -S "$HERE/relay-helper" -B "$STAGE/helper" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DQT_HOST_PATH="$QT_HOST" -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST/lib/cmake"
  cmake --build "$STAGE/helper" --parallel
  cp "$STAGE/helper/relay-helper" "$APP/"
  "$SDK/bin/arm-webos-linux-gnueabi-strip" "$APP/relay-helper"
fi

cp "$HERE/spike/appinfo.json" "$APP/"
python3 - "$HERE/../../packaging/icons/relay.png" "$APP" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
im.resize((80, 80), Image.LANCZOS).save(sys.argv[2] + "/icon.png")
im.resize((130, 130), Image.LANCZOS).save(sys.argv[2] + "/largeIcon.png")
PY
cat > "$STAGE/data/usr/palm/packages/$ID/packageinfo.json" <<J
{ "app": "$ID", "id": "$ID", "loc_name": "Relay Spike", "package_format_version": 2, "vendor": "im-doughboy", "version": "$VER" }
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
Description: Relay hardware video test
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
