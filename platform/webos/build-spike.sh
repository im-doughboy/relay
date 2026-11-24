#!/bin/bash
# Builds the webOS hardware video spike and packs it as an IPK.
# Usage: platform/webos/build-spike.sh <webos-sdk-dir> <out-dir>
# The SDK is openlgtv/buildroot-nc4's arm-webos-linux-gnueabi_sdk-buildroot
# (run its relocate-sdk.sh once after unpacking).
set -euo pipefail
SDK=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ID=com.imdoughboy.relay.spike
VER=$(python3 -c "import json,sys;print(json.load(open('$HERE/spike/appinfo.json'))['version'])")
CXX=$SDK/bin/arm-webos-linux-gnueabi-g++
SR=$SDK/arm-webos-linux-gnueabi/sysroot
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
APP=$STAGE/data/usr/palm/applications/$ID
mkdir -p "$APP/lib" "$STAGE/data/usr/palm/packages/$ID" "$STAGE/control"

"$CXX" -std=c++17 -O2 -Wall -o "$APP/relay-spike" "$HERE/spike/main.cpp" \
  -I"$SR/usr/include/glib-2.0" -I"$SR/usr/lib/glib-2.0/include" \
  -Wl,-rpath,'$ORIGIN/lib' -lSDL2 -lSDL2_ttf -lumedia_api -lpthread
"$SDK/bin/arm-webos-linux-gnueabi-strip" "$APP/relay-spike"
# The TV's own libstdc++ can be older than this compiler's, so ship ours.
cp -L "$SR/usr/lib/libstdc++.so.6" "$APP/lib/"
"$SDK/bin/arm-webos-linux-gnueabi-strip" "$APP/lib/libstdc++.so.6"

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
