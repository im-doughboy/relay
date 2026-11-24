# Relay on webOS (work in progress)

Relay for LG TVs in Developer Mode or rooted with Homebrew Channel. It shares
the Windows code; only the video player and the packaging differ.

## Step 1: hardware video spike

`spike/` is a small test app. It plays one stream through the TV's own media
server under a transparent window, to prove smooth hardware playback before any
screens are ported.

Build (Linux, needs the openlgtv/buildroot-nc4 webOS SDK, unpacked and
relocated with its `relocate-sdk.sh`):

    platform/webos/build-spike.sh <sdk-dir> <out-dir>

Install the IPK with webOS Dev Manager, then over SSH:

    echo "url=http://your.provider/live/user/pass/1234.ts" > /tmp/relay-spike.conf

The url can also be a whole playlist (an M3U or get.php address); the spike
then plays its first live channel, or the first whose name contains
`channel=...`.

Open Relay Spike from the launcher. OK pauses and plays, Back quits. The log is
in `/tmp/relay-spike.log`. Other settings: `transport=HLS` (or another
mediaTransportType), `type=media`, `window=0` (no exported video window).
