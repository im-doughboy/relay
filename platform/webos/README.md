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

SDL keeps the window, input and video layer, and Qt Quick draws into SDL's GL
context; there is no Wayland platform plugin. `qpa-sdl/` is a small Qt
platform plugin that hands Qt SDL's context. The spike's `ui=1` draws through
`QQuickRenderControl`. Relay itself can't: its `Main.qml` is a QML `Window`,
which Qt creates itself. For Relay the plugin makes that window SDL's: Qt
Quick draws on the main thread (no threaded OpenGL), swaps through SDL at up to
60 frames a second, and gets SDL's keys, pointer and typed text. The window
always reports full screen and ignores requests to leave it, since Back
arrives as Escape and Relay's Escape leaves full screen on a computer.

## Relay on the TV

`app/` builds Relay's own `src/main.cpp`, code and QML for the TV:

    platform/webos/build-app.sh <sdk-dir> <out-dir> <qt-work-dir>/qt-target <qt-work-dir>/qt-host

- `app/TvPlatform.cpp` runs first (`RELAY_WEBOS` in `main.cpp`): the TV's
  services (`app/TvLuna.cpp`, registered before SDL and shared with it, or
  the TV keeps the Back button), SDL's window and OpenGL ES context, Qt's
  `sdl` platform, a UTF-8 locale, and a crash note in `crash.log`.
- `app/TvVideo` is `Relay.Video`'s `MpvVideo` on the TV, with the same QML
  interface as `src/player/MpvItem`. Where it plays, it leaves the screen
  see-through to the TV's video layer. Playback is not connected yet.
- Relay keeps its settings, guide and log in `/media/developer/temp/relay`
  (`RELAY_DATA` changes it), because the app's own folder is replaced by each
  update. The log is `data/Relay/Relay/relay.log` there.
- `Settings.tv` is true, and Settings hides what needs a file or folder
  picker: choosing a playlist file, the recordings folder, an external
  player, and backup and restore. Before the first playlist, the phone remote
  is on and its pairing code shows, to type the address on a phone.

The remote. webOS gives apps the arrows, OK, Back and CH ∧/∨ only: it keeps
GUIDE, 123, AI, ?, the gear and the dashboard button, and asking for them
(SDL's `SDL_HINT_WEBOS_WINDOW_KEYMASK`, decimal or hex) changed nothing on a
2025 TV. On the TV:

| Button | Relay |
|---|---|
| OK | channel list; held: TV guide (`app/TvRemote.cpp`) |
| ∧ ∨, CH ∧ ∨ | channels |
| → | what's on |
| Back | closes what's open; on the live picture, Relay's menu (Guide, Movies, Search, Settings, Leave Relay) |

Holding Back is the TV's own "leave this app", so it can't be given a job.
The info bar has Guide, Movies and Search buttons for the Magic Remote's
pointer, and a hint line names the buttons.

In the background (the TV's home screen, another app or input), Relay stops
drawing, reports itself suspended, and its player lets go of the TV's video
decoder; launched again, it's back on the same screen and channel.

TV items for later:

- Game controllers: Relay's `Gamepads` reads SDL's events itself, which would
  take them from the window, so it is off on the TV for now.
- Where saved logins and Relay's files should live for good.

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
- **Game controllers** go through SDL, which the TV has, but `Gamepads` has to
  share SDL's events with the window first (see above).
