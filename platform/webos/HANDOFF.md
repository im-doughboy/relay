# Handoff: webOS work (26 November 2026)

For the next Claude session on the TV port. Read `CLAUDE.md` and `ROADMAP.md` first.

## In the middle of: movies and episodes cut off with error 509 on the TV

What happens: a movie or episode plays for 20 to 65 seconds, then the provider stops it with
HTTP 509 (the TV's player reports `server error:40509`, then `Network Error (300)`). Seen three
times on 25 and 26 November with the owner's two providers.

What was found:
- While the movie played there was exactly one connection to the provider, the TV player's, and
  none from Relay (counted from `/proc/net/tcp` on the TV, by owner and port).
- Every time, live TV from the same provider had played through Relay's stream relay until the
  movie started. Theory: the provider still counts the live stream for a while after it stops, so
  a one-connection account looks like two and the server cuts the newer one. Not proven.
- One provider answered 404 for an episode its own list has: the file isn't there. Relay moved on
  to the other playlist's copy, which is right.
- Relay's relay fallback for movies got a 302 it didn't follow: `CastRelay::pipe` uses
  `NoLessSafeRedirectPolicy`, so an https-to-http redirect stops it. The TV's own player follows
  those.

Fixed already (8c099cd): the TV's player reports "end of stream" about 30 s after such a failure,
and Relay took that as the movie finishing (saved it as watched, told Trakt, left the movie). It's
ignored now (`TvVideo::onEnded`). The source chooser closes when the movie closes.

Not done, and needs the owner's OK:
- Test the theory: stop live TV, wait 60 s, then start the movie. No 509 would confirm it.
- If confirmed: after a 509, wait about 30 s and resume the same source at the same point.
- Following https-to-http redirects in the relay fallback (a security trade-off, shared code).
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
412 Back), `tv-run.sh` (this run's log, memory, crash note), `tv-conns.sh` (connections by owner).
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

- The owner's Windows Relay needs two playlist links pasted again (see above).
- 0.6 decisions (ROADMAP): logins on the TV go in a file in Relay's private data folder; Relay
  ships its own Trakt app (public Client ID, device code), with "use your own app" kept in Settings.
- Trakt without a Client Secret works on the TV: sign-in and a forced refresh were checked against
  the real Trakt on 26 November.
