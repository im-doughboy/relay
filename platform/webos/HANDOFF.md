# Handoff: webOS work (26 November 2026)

For the next Claude session on the TV port. Read `CLAUDE.md` and `ROADMAP.md` first.

## Done: movies and episodes cut off with error 509 on the TV

What happened: a movie or episode played for 20 to 65 s, then the provider stopped it with HTTP 509
(the TV's player reports `server error:40509`, then `Network Error (300)`).

Why (measured 26 November): the playlist's server redirects the player to a stream server, and the
stream server ends every response after about 50 MB, with data still flowing. The TV's player then
asks the stream server again for the rest (a range request), straight at the redirected address,
and that second request gets the 509. Waiting after live TV made no difference.

Fix: movies and episodes go through Relay's stream relay on the TV (`TvVideo::open`). Each request
the player makes, the reconnects included, starts again from the playlist's address, which works.
If the relay doesn't get a movie going, it plays once more directly. Checked on the TV: the
reconnects every ~50 MB play on, seeking forwards and back lands within 2 to 6 s, a 3840x2160
60 fps HEVC title plays without buffering with Relay at 0 to 4% CPU, and The Odyssey played to
the end (2 x 40 min, 92 provider cut-offs handled) with Relay's memory level or falling and its CPU
at 0 to 12%.

Fixed on the way:
- The relay read the whole movie ahead of the player after a redirect: Qt drops the read limit on
  the redirected request. The TV ran out of memory and restarted. The relay follows redirects
  itself now, never from https to http (dd4ec91).
- The TV showed 0:00 for every movie: the player's `currentTime` is an object (ea9161d).
- The TV's player reports a failure twice; with a retry waiting, the second report showed the
  error and the source chooser over the retry. Ignored now.
- Opening Search while the movie list loaded (the main thread held up) crashed Relay in LG's SDL
  and `libwayland-client`: the remote's presses that piled up started, stopped and started SDL's
  text input back to back. The platform layer (`qpa-sdl`) now only notes what a text field wants
  and applies it from its event loop. Crashed on the first try before, not in 30 tries after.
- The provider sometimes doesn't answer a request at all (47 minutes into a movie, and once on a
  first start). The TV's player gave up after 40 s. The relay now asks again from the first
  address after 10 s, twice at most.

Still open:
- One provider answered 404 for an episode its own list has: the file isn't there. Relay moved on
  to the other playlist's copy, which is right.
- Decided by the owner (26 November): the relay keeps its redirect policy (no https-to-http).
- Switching to another source automatically is ROADMAP 0.7 ("Switch to a working copy", movies
  and episodes across playlists, decided). Not before 0.6.

## Where things are

The webOS build lives in WSL (`Ubuntu-24.04`) under `/opt/relay-webos`:
- `relay/`: a clone of this repository, used for TV builds. Pull before editing.
- `sdk-path`: a file holding the path of the openlgtv buildroot-nc4 SDK (unpacked in `sdk-unpack/`).
- `qt-settled/qt-target` and `qt-settled/qt-host`: static Qt 6.12.0 for the TV and its host tools
  (built by `build-qt.sh`; the Qt source is in `qtbase-src-6.12.0/`).
- `out-app/`: the TV build: `build/`, `relay.debug` (unstripped, for addresses in crash and freeze
  logs) and `com.imdoughboy.relay_<version>_arm.ipk`.

From Windows, run WSL scripts as `wsl -d Ubuntu-24.04 -- bash /mnt/c/.../script.sh`. Keep anything
with quotes or `$` in a script file: PowerShell and Git Bash mangle them inline.

## Building and testing

TV build:

    cd /opt/relay-webos/relay
    bash platform/webos/build-app.sh "$(cat /opt/relay-webos/sdk-path)" /opt/relay-webos/out-app \
        /opt/relay-webos/qt-settled/qt-target /opt/relay-webos/qt-settled/qt-host

Installing on the TV (rooted, SSH as root; ask the owner for its address, it isn't written in the
repository): copy the IPK to `/media/developer/temp/relay.ipk`, then
`luna-send -i luna://com.webos.appInstallService/dev/install '{"id":"com.imdoughboy.relay","ipkUrl":"/media/developer/temp/relay.ipk","subscribe":true}'`,
wait for `"installed"`, and launch with `luna://com.webos.service.applicationmanager/launch`.

Helper scripts left on the TV in `/media/developer/temp/` (copies were in the session's scratch
folder, which doesn't last): `tv-install.sh` (install and start), `tv-relaunch.sh N` (restart;
with N, makes the saved channel list look N minutes old), `tv-act.sh <action>` (the phone remote's
actions: home, movies, guide, settings; it pairs once, which adds a "phone" in Relay's Settings),
`tvkey /dev/input/event0 <code>[/ms]` (remote keys: 103 up, 108 down, 105 left, 106 right, 28 OK,
412 Back), `tv-run.sh` (this run's log, memory, crash note), `tv-conns.sh` (connections by owner), `tv-mon.sh N S` (memory, Relay's CPU, the TV's CPU every S
seconds into `mon.txt`, synced, and closes Relay below 150 MB free so the TV can't restart),
`tv-playfirst.sh` (plays the first New Movies 2026 title), `tv-show.sh LINE` (player and relay
lines after a log line, no addresses).
Screen captures: `luna://com.webos.service.capture/executeOneShot`.

On the TV, Relay's data is in `/media/developer/temp/relay`: `data/Relay/Relay/relay.log`,
`crash.log`, `freeze.log` (samples now include real call chains after `bt:`), and `config/`.
The log names the owner's providers (playlist names): never quote those lines or commit them.
Don't run heavy tools on the TV (a big `grep -o` once stalled it); copy files to the PC instead.
Wait for `Playlist:` in the log before testing.

Windows tests, before every push (`CLAUDE.md`): the worktree `D:\relay-test` holds GitHub's main
plus the change under test. In MSYS2 UCRT64:
`cmake -S . -B build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DRELAY_BUILD_TESTS=ON`, then
`cmake --build build-tests` and `ctest --test-dir build-tests`. `test_sources.exe` prints nothing
to a console: add `-o results.txt,txt` to see each test.

Careful: before 8c099cd the tests' saved logins used the same names as the installed Relay's in
the Windows Credential Manager, and a test run here deleted two of the owner's playlist addresses.
Check that tests can't reach the real app's settings or logins before running them on this PC.

## Other open items

- 0.6 decisions (ROADMAP): logins on the TV go in a file in Relay's private data folder; Relay
  ships its own Trakt app (public Client ID, device code), with "use your own app" kept in Settings.
- Trakt without a Client Secret works on the TV: sign-in and a forced refresh were checked against
  the real Trakt on 26 November.
