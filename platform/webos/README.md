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
  `sdl` platform and a UTF-8 locale.
- `app/TvVideo` is `Relay.Video`'s `MpvVideo` on the TV, with the same QML
  interface as `src/player/MpvItem`, so Relay's screens and channel handling
  are unchanged. It plays through `app/TvPlayer`: the TV's own player
  (`luna://com.webos.media`) in an exported video window under Relay's
  screens. Where it plays, the item leaves the screen see-through, and the
  video window follows the item (full screen, or the guide's small picture).
  - Live channels go through Relay's stream relay (`src/cast/CastRelay`,
    inside Relay, on 127.0.0.1 only): one connection to the provider, with
    the channel's user agent.
  - Movies and episodes are fetched by the TV's player itself, which reads
    the file as it plays. Through the relay they piled up in Relay's memory
    (240 MB in 25 seconds). If that doesn't start, one more try goes through
    the relay.
  - There is one player for the whole app: the TV gives an app one video
    decoder.
  - A live channel that drops is reopened up to four times. Changing
    channels quickly starts only the channel the presses stop on.
  - Not on the TV yet: pausing live TV, choosing sound and subtitle tracks,
    picture shape, and trailers (they need yt-dlp).
- Relay keeps its settings, guide and log in `/media/developer/temp/relay`
  (`RELAY_DATA` changes it), because the app's own folder is replaced by each
  update. The log is `data/Relay/Relay/relay.log` there.
- `Settings.tv` is true, and Settings hides what needs a file or folder
  picker: choosing a playlist file, the recordings folder, an external
  player, and backup and restore. Before the first playlist, the phone remote
  is on and its pairing code shows, to type the address on a phone.
- The package carries the SDK's OpenSSL 3.6 (`lib/libssl.so.3` and
  `lib/libcrypto.so.3`) beside its libstdc++. Relay's Qt is built against it,
  the TV's own OpenSSL is an older 3.0, and the TV's libraries load OpenSSL
  into the same process. With Relay's loaded first there is one OpenSSL, the
  newer. A static copy in Relay next to the TV's crashed inside OpenSSL, and
  so did Relay on the TV's older one.
- OpenSSL is set up once, on the main thread, while Qt starts
  (`app/TvPlatform.cpp`): its digests (Qt's `QCryptographicHash` goes
  through OpenSSL here, image cache names included) and the TV's
  certificates. When several threads did that at once on a quick start,
  Relay crashed in `ossl_lib_ctx_get_data` (3 starts in 9); with this, none
  in 16. It takes about 0.2 seconds.

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

Memory. The TV has 1.6 GB for everything. Its memory manager closes an app
that grows large (it closed Relay at about 850 MB), and the TV restarts when
memory runs out altogether. Relay stays near 140 MB on live TV and 350 MB in
Movies & series:

- A playlist is read line by line. For a playlist with an Xtream login its
  movie and episode entries are only counted, since the server's own lists
  replace them (one playlist had 370,000).
- The movie catalogue loads when Movies & series opens and is let go 90
  seconds after it closes (`VodService::release`). Other playlists'
  catalogues are combined only when that's turned on in Settings; on the TV
  it starts off.
- The guide and the channel list have their rows only while they're open. A
  hidden guide rebuilt its rows on every change to the channels, which also
  froze the screen for seconds.
- After reading a playlist Relay hands the memory it used back to the TV
  (`malloc_trim`): a 100 MB M3U otherwise left Relay 115 MB larger.
- Relay marks itself as the first to go when memory runs out
  (`oom_score_adj`), rather than one of the TV's services.

Starting. Relay saves a playlist's channels after each load
(`src/sources/SavedPlaylist`, in the playlist's own folder) and opens on
them at the next start, then fetches the new list in the background unless
the saved one is less than an hour old. A new list with the same channels
changes nothing on screen. The saved copy holds no login: the password and
username are put back from Settings when it's read. Fetching is quicker
with an Xtream Codes login, too: Relay then asks the server for the live
channels only, where an M3U link brings every movie and episode as well
(100 MB for one provider).

Smoothness. The TV's processor and graphics are far slower than a
computer's, so on the TV:

- Icons are drawn directly, without an offscreen layer each (the TV's
  graphics have no multisampling), and Movies & series cards have square
  corners.
- In Movies & series the selected card widens once the remote rests on it,
  and rows of posters are made a little at a time, before they're in view.
- Posters come at the size the TV shows them. Providers list TMDB's 600×900
  ones; TMDB has the same at 300×450 and 220×330, a third of the download and
  a quarter of the decoding, and decoding (one poster at a time) is what
  posters wait for on the TV (`TmdbClient::imageAtWidth`). The selected row
  also fetches the next few posters before their cards exist.
- Moving through the channel groups switches the channel list once the
  remote stops (0.3 seconds).

Finding problems. In `/media/developer/temp/relay`:

- `data/Relay/Relay/relay.log` has a `Screen:` line every 10 seconds while
  the remote is used: how long presses took to show, frame times, and how
  many frames came at 60 a second (from `qpa-sdl`).
- `crash.log`: a crash's address and thread, the calls on its stack, and
  where everything was loaded.
- `freeze.log`: where the main thread was whenever it stopped answering for
  more than 1.5 seconds (`app/TvWatchdog.cpp`).
- `profile.log`: a sample of the main thread every 10 ms while a file named
  `profile-on` exists there (create it, use Relay, remove it).

Addresses are looked up in `<out-dir>/relay.debug`, the unstripped program
that `build-app.sh` keeps:

    arm-webos-linux-gnueabi-addr2line -f -C -e relay.debug <address>

TV items for later:

- Game controllers: Relay's `Gamepads` reads SDL's events itself, which would
  take them from the window, so it is off on the TV for now.
- Where saved logins and Relay's files should live for good.
- The updater still checks for new releases, which are Windows and macOS
  setup files: hide it on the TV.

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

Parts that needed a TV answer, and where they stand:

- **The player.** Done: `app/TvVideo` and `app/TvPlayer` (see above). Left:
  pausing live TV, sound and subtitle tracks, picture shape.
- **Multi-view, picture-in-picture and casting.** The TV gives an app one
  video decoder, and casting from a TV isn't useful: all three are left out
  of Relay's menu on the TV.
- **Trailers** run yt-dlp, which won't run on the TV: off.
- **The updater** installs Windows and macOS setup files. The TV updates with
  a new IPK; the check still runs and has to be hidden.
- **Open in external player** and **backup and restore** need a file picker:
  their Settings rows are hidden. Restoring would also have to relaunch Relay
  through the TV's app manager.
- **Saved logins** (`SecretStore`) use the Windows and macOS secure stores;
  elsewhere it reports none, so on the TV they're in Relay's settings file.
  The TV needs a decision on where logins live.
- **The phone remote** works, and its pairing code shows on the first screen.
  Unlike the stream relay, its server listens on the network, because the
  phone connects over Wi-Fi.
- **Game controllers** go through SDL, which the TV has, but `Gamepads` has to
  share SDL's events with the window first (see above).
