#!/bin/bash
# Builds the small static FFmpeg Relay uses on webOS: network, demuxers and
# parsers only. The TV decodes; FFmpeg just reads and splits the stream.
# Usage: platform/webos/build-ffmpeg.sh <webos-sdk-dir> <ffmpeg-source-dir> <prefix>
set -euo pipefail
SDK=$(cd "$1" && pwd); SRC=$(cd "$2" && pwd); PREFIX=$3
cd "$SRC"
mkdir -p doc/examples && touch -a doc/Makefile doc/examples/Makefile
./configure --prefix="$PREFIX" --enable-cross-compile --cross-prefix="$SDK/bin/arm-webos-linux-gnueabi-" \
  --arch=arm --target-os=linux --sysroot="$SDK/arm-webos-linux-gnueabi/sysroot" \
  --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
  --disable-everything --disable-autodetect --disable-debug --enable-small --enable-network --enable-zlib \
  --enable-protocol=http,tcp,file,hls,crypto \
  --enable-demuxer=mpegts,hls,flv,mov,matroska,aac,ac3,eac3,h264,hevc \
  --enable-parser=h264,hevc,mpegvideo,aac,aac_latm,ac3,mpegaudio \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
  --disable-avdevice --disable-swscale --disable-avfilter --disable-swresample
make -j"$(nproc)" && make install
