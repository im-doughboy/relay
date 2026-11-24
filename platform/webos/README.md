# Relay on webOS (work in progress)

Relay for LG TVs in Developer Mode or rooted with Homebrew Channel. It shares
the Windows code; only the video player and the packaging differ.

## Step 1: hardware video spike

`spike/` is a small test app. It plays one stream through the TV's own media
server under a transparent window, to prove smooth hardware playback before any
screens are ported.

Build (Linux, needs the openlgtv/buildroot-nc4 webOS SDK, unpacked and
relocated with its `relocate-sdk.sh`):

    platform/webos/build-ffmpeg.sh <sdk-dir> <ffmpeg-source> <ffmpeg-prefix>
    platform/webos/build-qt.sh <sdk-dir> <qt-work-dir>
    platform/webos/build-spike.sh <sdk-dir> <out-dir> <ffmpeg-prefix> <qt-work-dir>/qt-target <qt-work-dir>/qt-host

Install the IPK with webOS Dev Manager, then over SSH:

    echo "url=http://your.provider/live/user/pass/1234.ts" > /tmp/relay-spike.conf

The url can also be a whole playlist (an M3U or get.php address); the spike
then plays its first live channel, or the first whose name contains
`channel=...`.

Open Relay Spike from the launcher. OK pauses and plays, Back quits. The log is
in `/tmp/relay-spike.log`. Other settings: `transport=HLS` (or another
mediaTransportType), `type=media`, `window=0` (no exported video window).

By default the spike uses feed mode: FFmpeg reads and splits the stream and the
TV's Starfish player decodes it in hardware (the path Relay will use).
`mode=ums` tries the TV's own URL player instead; `audio=0` plays video only.

`mode=luna` plays through the TV's own player. Add `relay=1` to send the stream
through Relay's stream relay (`relay-helper`), `hls=1` to ask for HLS, and
`switches=8 switchhold=5000` to reload it repeatedly as a channel-switch test.
With a playlist, `channels=4` makes those reloads cycle through four live channels.
`ui=1` draws the screen with Qt Quick (`spike/Main.qml`) over the video instead
of the SDL status bar.

## Screens

SDL keeps the window, input and video layer. Qt Quick draws into SDL's GL
context through `QQuickRenderControl`; there is no Wayland platform plugin.
`qpa-sdl/` is a small Qt platform plugin that hands Qt SDL's context, and the
spike's `ui=1` is the first use of it.
