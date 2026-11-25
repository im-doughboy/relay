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

## Porting Relay

What Relay needs on the TV, checked against the code (Nov 2026).

Qt (built by `build-qt.sh`, the same version as MSYS2):

| Relay uses | In Qt | On the TV |
|---|---|---|
| Core, Gui, Network, Quick, Controls Basic, Window, Layouts | qtbase, qtdeclarative | built |
| Sql with SQLite (the guide store, `src/epg/EpgStore`) | qtbase | built, SQLite inside Qt |
| Concurrent (guide, playlists, catch-up, catalogue) | qtbase | built |
| Shapes (`Icon.qml`), Effects (`RoundedEffect.qml`) | qtdeclarative, Shader Tools | built |
| Dialogs (five file pickers in Setup) | qtdeclarative | built; the pickers can be hidden |
| OpenGL (only for mpv) | qtbase | built, not needed |

Left out because Relay doesn't use them: Particles, the Material, Fusion,
Universal, Imagine and FluentWinUI3 styles, XmlListModel, the QML design and
preview tools, and the test libraries.

Parts that need a TV answer:

- **The player.** `Relay.Video` (`MpvVideo`, `src/player/MpvItem`) needs a TV
  version with the same QML interface (about 80 properties, functions and
  signals), backed by the TV's own player through `com.webos.media` and the
  stream relay, as the spike does. This is the largest piece.
- **Multi-view.** The TV gave the spike one video decoder, so four tiles at
  once probably won't work. Limit or hide it on the TV.
- **Trailers** run yt-dlp, which won't run on the TV: another source or off.
- **The updater** installs Windows and macOS setup files: off. The TV updates
  with a new IPK.
- **Casting from the TV** isn't useful: hide it.
- **Open in external player** starts another program: hide it.
- **Restore from a backup** restarts Relay by starting its own program again;
  on the TV the app manager has to relaunch it.
- **Saved logins** (`SecretStore`) use the Windows and macOS secure stores;
  elsewhere it reports none. The TV needs a decision on where logins live.
- **The phone remote** stays. Unlike the stream relay, its server listens on
  the network, because the phone connects over Wi-Fi.
- **Game controllers** already go through SDL, which the TV has.
