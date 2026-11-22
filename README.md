# Relay

**A desktop IPTV player for Windows, Mac and Linux**, built with Qt 6 and mpv. It feels like a
set-top box: it opens straight into live TV, everything works from the keyboard (or a remote
that sends keys), and the mouse works everywhere too.

> **Relay is a player only. It doesn't come with any channels.** You add a playlist or login from
> your own IPTV provider, or from your own tuner. Use it only with services you're allowed to watch.

## Install

**Windows 10 and 11:** download `Relay-<version>-setup.exe` from
[Releases](../../releases/latest) and run it. It installs for your Windows user only (no
administrator prompt), and Relay then keeps itself up to date.

The installer isn't code-signed yet, so Windows SmartScreen may say *"Windows protected your PC"*.
Click **More info → Run anyway**. A portable `.zip` is on the same page if you'd rather not install.

**Mac and Linux:** there are no ready-made downloads yet. Build it from the source; see
[Building](#building).

## What it does

- **Playlists:** M3U/M3U8 links or files, Xtream Codes logins, and Stalker portal (MAC) logins;
  several playlists side by side
- **TV guide:** XMLTV guides, now/next in the channel list, a full guide grid, reminders
- **Pause live TV, catch-up and recordings**, including recording a whole series
- **Movies and series** from your provider, with posters and details from TMDB
- **Multi-view** (up to four channels at once) and picture-in-picture
- **Favorites, hidden channels, search, renaming and reordering**, and a parental PIN
- **Trakt:** marks what you watch and imports what you've already seen
- **Casting** to a Chromecast or a DLNA smart TV
- **Automatic updates** from this page's releases

The sections below describe each feature in detail.

## Privacy

Relay has no account, no ads and no analytics. It talks only to your provider (including the
logo and guide addresses in your playlist), the guide addresses you add, GitHub to check for
updates (Settings → Updates turns that off), and TMDB and Trakt if you set them up.

Your playlist logins and keys are kept in Windows Credential Manager or the macOS Keychain. On
Linux they're in Relay's settings file in your home folder for now. Passwords are blanked out
of Relay's log.

## Live TV

- **Playlists:** M3U/M3U8 by URL or local file, and Xtream Codes login (MPEG-TS or HLS output)
- **Live TV only:** movie and series entries in "m3u_plus" playlists (`/movie/`, `/series/`
  links and video files) are left out of the channel list; Settings shows how many were hidden
- **Remembers your setup:** reopens the saved playlist and the last channel you watched
- **Channel list overlay:** groups on the left, channels on the right, logos, "now playing" marker
- **Zapping:** up/down stays inside the group you picked from; previous-channel recall
- **Number entry:** type a channel number, it tunes after a short pause (or press Enter)
- **Info bar:** number, logo, name, group, clock, resolution / fps / codecs
- **Clear failures:** offline channels show a message with retry instead of a frozen frame
- **Automatic reconnection:** when a live stream drops, the player reconnects by itself
  ("Connection dropped · reconnecting…"), with growing pauses up to 8 tries; a channel that
  never starts gets one quick retry before the error shows
- Fullscreen, mute, volume, pause, auto-hiding mouse cursor in fullscreen

## TV guide (EPG)

- **Guide import:** XMLTV from the playlist's `url-tvg` or Xtream's `xmltv.php`, or your own
  guide address in Playlist settings. Plain or `.gz`, URL or local file.
- **Fast and light:** streamed and imported on a background thread into SQLite; only channels
  in your playlist are kept (2 days back, 8 days ahead). 770,000 programmes import in about
  13 seconds using ~30 MB of extra memory, and the UI stays usable throughout.
- **Smart matching:** `tvg-id` first (case-insensitive), then the channel name with country
  tags and quality labels stripped (`US: CNN HD` matches `CNN`).
- **Now/next:** the info bar shows the current show, episode, time range, progress, time left,
  category and what's next; press I again for the description.
- **Channel list:** each channel shows what's on now with a progress bar.
- **Stays current:** the stored guide appears instantly at startup and refreshes in the
  background every 12 hours. Settings show the guide status, with an **Update guide** button.

## TV guide grid

Press **G** for a full-screen guide: channels down the left, shows laid out on a two-hour
timeline, a "now" line, past shows dimmed, and the live channel still playing in a preview at
the top right. The header shows the selected show's channel, time, episode, category and
description.

| Key (in the guide) | Action |
|---|---|
| ← → | Previous / next show (the timeline scrolls ahead as you go) |
| ↑ ↓, Page Up / Page Down | Change channel / page through channels |
| ↑ on the first channel | Group switcher; ← → change group, ↓ goes back |
| Home | Back to what's on now |
| Enter | Watch the selected channel |
| Esc, G or Backspace | Close the guide |

Mouse: click a show to select it, click again or double-click to watch, scroll for channels,
Shift+scroll to move through time. Browsing goes 2 days back and 7 days ahead.

## Favorites and hiding

- **Favorites:** press **F** on a channel in the channel list or the guide (or right-click it).
  Favorites is always the first group, in the order you added them, with a ★ on the channel.
- **Hide channels and groups:** press **H** on a channel, or on a group in the groups column
  (or right-click). Hidden items disappear from the channel list, the guide, channel up/down
  and number entry. Hiding a favorite also removes it from Favorites.
- **Bring them back:** Playlist settings (S) → **Hidden (N)** lists everything hidden; select
  an item and press Enter (or click Show).
- **Survives refreshes:** stored in `library.json` in the app data folder, keyed by the
  provider's stream ID, so favorites and hidden items stay put when the playlist reloads,
  even if your provider changes its domain or your password.

## Search, rename and reorder

- **Search:** press **/** or **Ctrl+F** (also from the channel list or the guide). Type to
  find channels by name or number (accents and capitals ignored; every word must match) and
  shows by title, airing-now first. ↓ moves into the results, Enter watches.
- **Rename:** press **E** on a channel or on a group (or right-click → Rename…). Leave the
  name empty to get the original back. Renaming doesn't affect guide matching.
- **Reorder:** **Ctrl+↑ / Ctrl+↓** moves the selected group (in the groups column) or channel
  (inside Favorites or a playlist group; All channels keeps the playlist's order). A group's
  right-click menu also has **Sort channels A–Z** and **Reset channel order**.
- Like favorites and hiding, all of this is saved in `library.json` and survives playlist
  refreshes.

## Movies and series

- Press **V** for Movies and Series. With an Xtream login, or an M3U link from an Xtream-style
  server (`…/get.php?username=…&password=…`), the full catalogue comes from the server, with
  plots, cast, ratings, seasons and episodes. It's saved in the app data folder (`vod`) and
  refreshed once a day, or from Settings → **Update movies**. Other M3U playlists show the
  movie and episode entries they contain, with episodes grouped into series by their
  "S01 E02" names.
- Browse by category, search by typing, change the order with **O** (default, recently added,
  A–Z, top rated, newest), **Tab** switches between Movies and Series, **F** adds a favorite.
- **Continue watching** keeps your place in movies and episodes. A movie counts as watched at
  90 %, and finishing an episode queues the next one. Delete removes an item from the list.
- Playback: **← →** seek 10 s (Shift: 1 min), **↑ ↓** 1 min, Page Up/Down 10 min, Space pause,
  **A** audio track, **C** subtitles, **N** next episode, Esc goes back to the details (live TV
  carries on underneath). The bar at the bottom can be clicked to jump.
- Search (**/**) also finds movies and series once the catalogue has loaded.
- Posters and logos are cached on disk (up to 300 MB).

## Pause live TV and catch-up

- **Pause live TV:** Space pauses a channel while Relay keeps saving it, so you can carry on
  where you left off. **Shift+← / Shift+→** (or **,** and **.**) rewind or skip 10 s,
  the media rewind/fast-forward keys 30 s, and **End** goes back to live. A bar at the top shows
  how far behind live you are; click it to jump, or click **Live**.
  The buffer is kept on disk in Relay's cache folder (`timeshift`). Its size is set in
  Settings → Pause live TV (off, 1, 2 or 4 GB; about 25 minutes of HD per GB, half of it for
  pausing and half for rewinding). Changing channel starts a new buffer.
- **Catch-up:** channels whose provider keeps past shows get a **↺** on those shows in the guide.
  Press **Enter** on one to watch it, or **B** to watch the show that's on now from its start
  (also works while watching, and the info bar says when it's available). In catch-up the arrows
  seek (the server is asked for the archive from the new time), Space pauses, and Esc or End
  goes back to the channel live. When a show ends the next one plays, until you catch up with live.
- Xtream accounts and get.php playlist links: Relay asks the server which channels keep an
  archive and its time zone (archive addresses use the server's local time). M3U playlists
  can describe catch-up with `catchup`, `catchup-days` and `catchup-source` (types `default`,
  `append`, `shift`, `flussonic`, `xc`). Settings shows how many channels have catch-up.

## Reminders and recordings

- **In the guide:** **R** records the selected show (it starts a minute early and runs two minutes
  over), **M** reminds you a minute before a show starts. Press again to cancel. A red dot or a
  clock marks the show.
- **While watching:** **Ctrl+R** records the show that's on until it ends (an hour without
  guide info); press again to stop. A REC badge shows while anything is recording.
- **Reminders** pop up in the top-right corner: Enter (or Watch) switches to the channel,
  Esc dismisses. Reminders and scheduled recordings are kept in `schedule.json` and survive
  restarts; a recording whose time passed while Relay was closed shows as missed.
- **Recordings** are the third tab next to Movies and Series (V, then Tab twice): recorded,
  scheduled and reminders. Enter plays (also while still recording), Delete stops a running
  recording, cancels a scheduled one, or deletes a finished one (press twice).
- Files are saved as `.ts` in Videos\Relay by default; change the folder in Settings. Dropped
  connections are retried every few seconds and HLS (.m3u8) channels are followed segment by
  segment. Note: many providers allow only one connection per account, so recording one channel
  while watching another may be refused by the server.

## Multi-view

- Press **Q** to watch up to four channels at once. It opens with the channel you're watching
  plus the ones you had last time.
- Arrows select a tile; the selected tile has the sound (♪). **Enter** changes its channel
  (type a name or number, or pick from Favorites / All channels), **Insert** adds a tile,
  **Delete** removes one, **Page Up/Down** zaps the selected tile (skipping channels already
  showing). **M** mutes, **H** shows the keys again.
- **Esc**, **F** or **Q** goes back to normal viewing on the selected channel; double-clicking
  a tile does the same.
- Each tile is a separate stream. Many providers limit how many streams an account can play at
  once; tiles that the server refuses show the reason.

## Parental PIN and secure login storage

- **Parental PIN:** set a 4-digit PIN in Settings (S). Then press **K** on a channel or group in
  the channel list (or right-click → Lock), or on a provider category in Movies / Series.
- While locked: locked channels ask for the PIN before playing (from the list, the guide, typed
  numbers, search, reminders, recordings, multi-view and catch-up), zapping skips them, and
  their programme names are hidden in the channel list, guide and search. Locked movie and
  series categories ask for the PIN and their titles are left out of All, search and Continue
  watching. Settings ask for the PIN too (can be turned off).
- Entering the PIN unlocks everything until Relay closes, or until **Lock now** in Settings.
  After five wrong tries Relay waits 30 seconds. Change or remove the PIN in Settings;
  removing it keeps the locks but nothing asks for a PIN.
- **Secure login storage:** the playlist link (which normally contains your username and
  password) and the Xtream password are kept in Windows Credential Manager (macOS Keychain)
  instead of Relay's settings file (or registry, on Windows). Existing settings move over on the first start. The last channel is
  remembered by its id, and recordings and Continue watching no longer save stream addresses.

## Multiple playlists

- Settings (S) lists your playlists. **+ Add another playlist** opens an empty form (name, M3U
  link or Xtream login, optional guide address); it's saved once it loads, and becomes the one
  you're watching. Switch, reorder (↑) or remove playlists from the list (Remove asks twice).
  The form below the list edits the playlist you're watching, including its name.
- Press **P** while watching for a quick switcher (Enter, or 1–9).
- Each playlist keeps its own channels, guide, favorites, hidden and locked channels, custom
  names and order, movies and Continue watching, last channel and multi-view tiles (files in
  the data folder under `playlists\<id>\`). Each login is kept in the credential store.
  The parental PIN, the pause-live-TV size and the recordings folder are shared.
- Reminders and recordings belong to the playlist they were set in. A reminder from another
  playlist still pops up, and Watch switches to that playlist. A scheduled recording only runs
  if its playlist is the open one at the time; otherwise it's marked as missed.
- Upgrading: the existing playlist becomes the first one in the list, named after its server
  (rename it in Settings).

## Multi-view across playlists

- In the multi-view channel picker (Enter or Insert), **Tab** — or a click on a playlist's name
  at the top — picks another playlist. Its channels load in the background the first time
  (you stay on the playlist you're watching) and are kept for the session.
- Tiles from another playlist are labelled with its name. Page Up/Down zaps within the tile's
  own playlist. Leaving multi-view on such a tile switches to that playlist and carries on with
  that channel.
- That playlist's hidden channels stay hidden, its renamed channels keep their names, and its
  locked channels and groups still need the PIN. "What's on now" isn't shown for those tiles,
  since that playlist's guide isn't loaded.
- The tiles (with their playlists) are remembered for next time, whichever playlist you're
  watching. Tiles from different providers count against different accounts' stream limits.

## Multi-view layouts

- The tiles fit the shape of the window. On a portrait (rotated) monitor they stack underneath
  each other; on a wide one they sit side by side or in a grid. They re-arrange when you
  resize the window or move it to another monitor.
- **L** switches layout; the choice is remembered:
  - **Auto** (default): whichever arrangement makes the tiles biggest for the window.
  - **Stacked**: one column.
  - **Side by side**: one row.
  - **Grid**: two columns.
  - **Focus**: the selected tile is big and the others are small beside it (wide window) or
    underneath it (tall window). Arrows move the focus to the previous/next tile.
- Arrows move to the tile next to the selected one on screen, whatever the layout. The
  "+ Add channel" slot appears in a free cell when there's room for one.

## Picture-in-picture

- **W** puts the channel you're watching in a small window in a corner, then opens the channel
  list so you can pick something else for the big picture. **W** again closes it.
- **X**, or a click on the small window, swaps it with the big picture (locked channels still ask
  for the PIN). **Shift+W** moves it to the next corner, or drag it to one; the corner is
  remembered.
- The small window is silent and shows the channel's name for a few seconds after a change (and
  when the mouse is over it). It moves up out of the way of the info bar.
- It keeps playing, hidden, while the guide, movies, recordings, search or settings are open.
  During a movie or catch-up, **W** pins the live channel you came from, so you can keep an eye
  on it. Opening multi-view or switching playlist closes it.
- It's a second stream, so it counts against your provider's connection limit.

## Channels without guide listings

- In the guide, a channel with no listings — or a stretch with nothing listed — is filled with
  hour-long blocks named after the channel (shown dimmer than real shows), instead of an empty
  row. They can be selected like shows.
- On a channel with catch-up, Enter on a past block plays that hour from the archive, and
  **B** on the current one goes back to the top of the hour. When a catch-up hour (or show)
  ends with no listing after it, the next hour follows, until it catches up with live.
- **R** records a block (the hour, or until the next listed show). Reminders need listings.
- The info bar says "No guide listings" and, with catch-up, offers **B** to watch from the top
  of the hour.

## Picture and sound

Press **O** (or the screen button in the menu bar, or **Picture & sound** in a movie's controls)
for a panel down the right: ↑ ↓ pick a row, ← → change it (or click the arrows).

- **Picture shape:** Original, 16:9, 4:3, Fill the window (crops the edges) or Stretch.
  **Z** switches it straight away. Remembered.
- **Audio track:** for channels with a second language or audio description. **A** switches
  it on live TV too; the next channels start in the same language when they have it.
- **Lip-sync:** moves the sound 50 ms at a time, up to 2 seconds either way (useful with
  Bluetooth speakers or a receiver). Remembered.
- **Subtitles / captions** (also **C**).
- **Deinterlacing:** Automatic (default) smooths the combed look of 1080i sports and news
  channels and leaves other channels alone; Always or Off. Needs mpv 0.38 or newer (MSYS2 has it).
- **Speed** (movies and episodes only): 0.5× to 2×; each movie starts at normal speed.
- **Sleep timer:** 15 minutes to 2 hours. A minute before the end a notice counts down; any key
  or a click on **Keep watching** adds 30 minutes. Live TV then stops; a movie pauses (its place
  is saved).

Settings also has **Video decoding**: Graphics card (default) or Software, for the rare graphics
driver that shows glitches or a black picture.

## Recently watched

The channel list has a **Recently watched** group under Favorites: the last 30 channels you
watched for more than a few seconds, newest first. Zapping inside it keeps the order steady; it
updates the next time you open the channel list. Each playlist keeps its own.

## Guide sources, times and listings

- **Several guide addresses:** in Settings, separate them with commas or put each on its own
  line. They're downloaded one after another; when two list the same channel, the first one wins.
  With your own addresses typed in, **Also use the playlist's own guide** adds the provider's.
  A `url-tvg` header listing several addresses is handled the same way.
- **Guide times out:** Settings moves every listing of the playlist 30 minutes at a time. For one
  channel, select it in the channel list and press **U** (or right-click → Guide listings…); Ctrl+← →
  or the arrows move its listings. `tvg-shift` in an M3U playlist is used too.
- **Wrong or missing listings for a channel:** in the same **U** window, type part of a name and pick
  the guide channel that's right (or **No listings**, or **Automatic** to go back). Every channel in
  the guide files can be picked, also ones that weren't matched; the list fills in after the
  first guide update with this version.
- Settings: **Update the guide every** 6 hours to 2 days, and **Keep guide listings for** 3, 8
  or 14 days (fewer days make updates faster).

## Playlist refresh and user agent

- **Refresh the playlist** (Settings): only when Relay starts, or every 6 hours, 12 hours or day
  (default). The refresh happens in the background while you watch live TV: the channel keeps
  playing, favorites and everything else stay, and new channels appear. It waits while a movie,
  multi-view, picture-in-picture or a list is open. If it fails, Relay carries on with the channels
  it has and tries again at the next interval.
- **User agent** (per playlist, Settings): what Relay calls itself to the provider, for providers
  that only accept certain apps. It's used for the playlist, the guide, movies, catch-up, recordings
  and channels (a channel with its own `http-user-agent` in the playlist keeps that one).

## Backup and restore

Settings → **Back up…** saves one `.relaybackup` file with every playlist (including the logins),
favorites, hidden and locked channels, custom names and order, guide choices, Recently watched,
Continue watching, reminders, scheduled recordings and settings. Keep it private: the logins are
readable in it.

**Restore…** shows when the backup was made and its playlists, then **Restore and restart Relay**
replaces what's there. Guides and the movie catalogue download again after the restart. Use it to
move Relay to another PC or Mac, or to get your setup back after reinstalling.

## Your own favorites lists

Besides Favorites (**F**), you can make lists of your own (Sports, Kids, News…). They show as
groups right after All channels, marked ★.

- **Shift+F** on a channel (or right-click → Favorites lists…) ticks the lists it's in; **New list…**
  there, or **N** in the groups column, makes one.
- In a list: **Delete** takes the selected channel out, **Ctrl+↑ ↓** orders it, zapping stays inside it.
- In the groups column: **E** renames a list, **Ctrl+↑ ↓** moves it among your lists, **Delete** twice
  deletes it (the channels stay in the playlist).
- Each playlist keeps its own lists; they're in backups.

## Recording a series

In the guide, **Shift+R** (or **Record series**) on a show records every episode of it on that
channel: each airing in the guide is scheduled as listings arrive. A repeat of an episode that's
already recorded or scheduled (same episode number, or same episode title) is skipped. Cancelling
one airing (R, or Delete in Scheduled) doesn't bring it back. Recordings → **Series** lists them;
Delete there stops recording the series and cancels its upcoming recordings (finished ones stay).
Shift+R again in the guide does the same.

## Portal (MAC) logins

Settings → **Portal (MAC)** signs in to a Stalker / Ministra portal with the portal address and the
MAC address registered with your provider (e.g. `http://provider.example:8080/c/`). Relay finds the
portal's API at the usual places, loads its categories and channels, and its guide (3 days). Each
time a channel, movie or episode is opened Relay asks the portal for its address, and signs in again
when the session runs out. Live TV, the guide, favorites, multi-view, picture-in-picture and
recording work, and:

- **Movies and series** (V): the portal's movie and series sections, every category, loaded in the
  background a few pages at a time and kept for a day like an Xtream catalogue (Settings → Update
  movies fetches them again). Series work whether the portal lists episode numbers on the series,
  on each season, or lists each episode on its own. Continue watching, favorites and Next episode
  work as usual.
- **Catch-up**: channels the portal archives show ↺ in the guide, for as many days as the portal
  keeps. A past show plays by the portal's programme id (from its guide, or asked for that day);
  skipping and "next show" work as with other catch-up.

## Posters and details from TMDB

Plain M3U playlists usually give movies and series a name and nothing else. With your own free
key from The Movie Database (themoviedb.org → Settings → API: the short "API key" or the long
"API read access token"), Settings → **Posters and details from TMDB** fills in what's missing:

- posters in the grid for titles without one (what's on screen first),
- plot, cast, director, genres, running time, rating and a backdrop on the info screen,
- episode names, plots and pictures for series.

Provider tags are tidied away before searching ("EN - The Matrix (1999) [4K]" is looked up as
"The Matrix", 1999). Anything the playlist already has is kept. Answers are kept on disk (titles
found for 30 days, titles not found for 7), so TMDB is asked once per title. The key is checked
when you save it, kept in the system credential store where there is one, and included in
backups. The titles of those movies and series are sent to TMDB to look them up.

This product uses the TMDB API but is not endorsed or certified by TMDB.

## Interface size, keys and an external player

- **Interface size** (Settings): 85 % to 150 % for text, lists and buttons, for a TV across the room
  or a small laptop. It's used from the next start (**Restart Relay now** is right there).
- **Change keys…** (Settings): pick an action for live TV, press Enter (or click it), then the new
  key. Delete puts one key back; **Put all keys back** resets them all. A key that's already used
  is refused with what it's used for. Arrows, Enter, Esc, numbers, volume and the seek keys stay as
  they are. The menu bar, the shortcuts list (H) and the hints follow your keys.
- **External player** (Settings → Choose player…): another player's program, such as VLC, mpv or
  MPC-HC. **E** while watching (or in a movie) opens what's playing in it; Relay stops the channel
  (or pauses the movie) so the two don't use two connections. R plays the channel in Relay again.

## Trakt

Settings → **Trakt** keeps your Trakt history up to date and shows what you've watched on Trakt.

1. On trakt.tv, go to Settings → **Your API Apps** → **New application**. Name it Relay and use
   `urn:ietf:wg:oauth:2.0:oob` as the redirect URI. Save, and copy the **Client ID** and
   **Client Secret** into Relay.
2. Click **Sign in**. Relay shows a short code: enter it at trakt.tv/activate on your phone or
   computer. Relay notices by itself when you have.

Then:

- Movies and episodes you watch are sent to Trakt as you watch (start, pause, stop). Trakt adds
  them to your history once you've watched 80 %. **Sending what I watch** turns this off.
- **Mark watched** / **Mark not watched** in Relay updates your Trakt history too.
- What's watched on Trakt shows as watched in Relay: movies get the ✓ in the grid, episodes in
  the series' episode list, and Play goes to the next episode you haven't seen. Relay checks
  every 6 hours, when it starts, and with **Sync now**.
- Titles are matched by name and year, with provider tags such as "EN -" or "[4K]" ignored.
  Live TV isn't sent to Trakt.

The Client Secret and the sign-in are kept in Windows Credential Manager (the macOS Keychain).
**Sign out** withdraws Relay's access on Trakt too.

## Casting to a TV or Chromecast

**T** (or the cast button in the menu bar) lists the Chromecasts and smart TVs on your network.
Pick one and what's playing goes to it: the live channel, or the movie or episode from where you
are. Relay's own picture stops while casting (one connection to your provider), and comes back
when you stop.

- **↑ ↓**, the channel list and the guide change the channel on the TV.
- **Space** pauses and plays, **Esc** stops casting, **T** again moves it to another device.
- Smart TVs and other DLNA players (Samsung, LG, Sony, Philips, Kodi…) are found with SSDP;
  Chromecasts and Google TV with mDNS. If one isn't found (some networks block finding devices),
  **Add a device by address** takes a Chromecast's IP address, or a TV's description address
  (`http://…/description.xml`). Added devices are remembered; **Delete** takes one off the list.

How it works: the TV plays from a small server inside Relay (port 8790), which fetches the stream
from your provider (with the channel's user agent and referrer) and passes it on. So the TV doesn't
need to reach your provider itself, portal channels work, and the provider sees one connection.
Chromecasts can't play the MPEG-TS most live channels use, so for them Relay repackages live
channels as HLS on the fly (4-second pieces cut at key frames). A live channel takes a few
seconds to start on a Chromecast because of that.

The first time, Windows asks whether Relay may use the network: allow **private networks**, or
the TV can't reach Relay and devices aren't found. The TV and this computer must be on the same
network.

Limits: casting uses the provider's own stream, so channels with H.265 or MPEG-2 video only play
on devices that support them (most TVs do; older Chromecasts don't). Subtitles and audio track
choices aren't sent to the TV.

## Updates

Relay checks GitHub for a newer release a minute after it starts and then once a day (Settings →
Updates turns this off, and **Check now** checks straight away). When there's one, a card in the
bottom-right corner offers it: **Update now** downloads the installer, checks it against the size
and SHA-256 GitHub lists for it, and runs it quietly; Relay closes, updates in place and opens
again, with your playlists and settings kept. **Later** stops offering that version; ✕ hides the
card until next time. A desktop shortcut you removed stays removed.

While the repository is private, GitHub only shows its releases to people signed in, so Relay
needs a token: github.com → Settings → Developer settings → Fine-grained tokens → Generate new
token, with access to only the relay repository and **Contents: Read-only**. Paste it in
Settings → Updates (it's kept in Windows Credential Manager). Anyone else you give Relay to would
need one too, which is a reason to make the repository public before sharing it.

On macOS and Linux there's no installer to run yet: Relay tells you about the new version and
opens its release page.

## Using the mouse

- **Menu bar:** move the mouse over the picture and a bar appears along the top with Channels,
  Guide, Movies & Series, Recordings, Multi-view and Search, plus captions, picture-in-picture,
  sound, full screen, Settings and **?** (every keyboard shortcut). Each button shows its key
  when the window is wide enough, or in a tip when you hover. Click the logo and playlist name
  to switch playlist.
- **Player controls (live TV):** moving the mouse also brings up controls along the bottom: back
  10 s, pause/play, forward 10 s, a timeline of what's been kept (click or drag to jump; the
  right end is live) and **Live**. They also appear for a moment after you skip with the keys.
  While you're paused or behind live, a small reminder stays at the top left.
- **Player controls (movies, episodes, catch-up):** the same back / pause / forward buttons, the
  timeline, Audio, Subtitles, Next episode, and a ✕ to stop.
- **On the picture:** click for the channel list, double-click for full screen, right-click for
  what's on, scroll for volume.
- **Guide:** Watch, From the start, Record and Remind me buttons for the selected show; Now,
  Search and ✕. Click the small picture to go back to watching full screen.
- **Multi-view:** buttons along the bottom (when it opens or the mouse moves): previous/next
  channel in the selected tile, change channel, add, remove, layout and watch full screen.
  Click a tile to select it; double-click to watch it full screen.
- **Lists:** right-click a channel or group for favorite, rename, hide, lock and move.
- Every full-screen view has a ✕ in the top-right corner (same as Esc), Settings included
  (on its inner pages, such as hidden channels, the same button goes back a step).
- **Settings** opens without putting the cursor in a text box (so the S that opened it, or a
  stray key, can't change your playlist address): click a field, or press Tab / ↓ / Enter.

## Keyboard

| Key | Action |
|---|---|
| Enter or ← | Channel list (inside the list: ← → switch columns, Enter tunes, Esc closes) |
| ↑ ↓ / Page Up, Page Down | Change channel |
| 0–9 | Go to channel number |
| Backspace or L | Previous channel |
| I or → | Info bar; press again for the programme description |
| Space | Pause |
| M, + / − | Mute, volume (mouse wheel works too) |
| F or F11 | Fullscreen (double-click also works) |
| R | Retry the current channel |
| E | Open in the external player (Settings) |
| T | Cast to a TV or Chromecast |
| G | TV guide |
| V | Movies and series |
| Shift+← / Shift+→ | Rewind / skip 10 s on live TV (End: back to live) |
| B | Watch the show on now from its start (catch-up) |
| Ctrl+R | Record the show on now (again: stop) |
| Q | Multi-view (up to four channels; L inside switches layout) |
| W · X · Shift+W | Picture-in-picture · swap with the big picture · move to the next corner |
| H · ? · F1 | Keyboard shortcuts (all of them, by screen) |
| C | Closed captions / subtitles: on, next track, off (live TV starts with them off; once on, they stay on as you change channels) |
| A | Audio track (language) |
| Z | Picture shape (original, 16:9, 4:3, fill, stretch) |
| O | Picture and sound: lip-sync, deinterlacing, speed, sleep timer |
| U (in the channel list) | Guide listings for the selected channel: pick them, move their times |
| P | Switch playlist |
| K (in lists) | Lock / unlock a channel, group or movie category (parental PIN) |
| R / M (in the guide) | Record / remind me about the selected show |
| / or Ctrl+F | Search channels and shows |
| F / H (in the channel list or guide) | Favorite / hide the selected channel |
| E (in the channel list) | Rename the selected channel or group |
| Ctrl+↑ / Ctrl+↓ (in the channel list) | Move the selected channel or group |
| S | Playlist settings |

Mouse: click toggles the channel list, right-click toggles the info bar.

## Building

You need **Qt 6.5 or newer** with the Qt Quick, Quick Controls, Network, Concurrent, OpenGL
and SQL (SQLite) modules, **libmpv**, **zlib**, and CMake 3.21+. (It also builds with Qt 6.4.)

### macOS

```sh
brew install qt mpv pkg-config cmake
cmake -S . -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
open build/relay.app
```

### Windows with MSYS2 (no Qt account needed)

Install MSYS2 from msys2.org, open **MSYS2 UCRT64**, then:

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,qt6-base,qt6-declarative,mpv}
cd /c/dev/relay
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/relay.exe
```

Run it from the UCRT64 shell so it finds the Qt and mpv DLLs. MSYS2's mpv is GPL-licensed:
fine for development; use an LGPL libmpv build for a closed-source release.

#### A Relay you can install (no MSYS2 needed to run it)

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{qt6-tools,nsis}   # once
./packaging/package-windows.sh
```

This builds a release and gathers relay.exe with every Qt and mpv file it needs into
`dist/Relay/`, then makes:

- `dist/Relay-<version>-windows-x64.zip`: the folder, zipped; extract anywhere and run relay.exe.
- `dist/Relay-<version>-setup.exe`: an installer (when NSIS is installed). It installs for your
  user only (no administrator prompt) into `%LOCALAPPDATA%\Programs\Relay`, with a Start menu
  entry, an optional desktop shortcut and an Apps & features entry for uninstalling.

The script checks that nothing is missing, but test the result by double-clicking
`dist/Relay/relay.exe` in Explorer (not from the UCRT64 shell, which would supply missing DLLs).

**Updating.** Run the newer `setup.exe`. It closes Relay if it's running, removes the old program
files and installs the new ones, with no need to uninstall first. Your data isn't in the program
folder, so updates never touch it:

| What | Where |
|---|---|
| Settings and the playlist list | registry, `HKEY_CURRENT_USER\Software\Relay` |
| Logins | Windows Credential Manager, entries named `Relay/…` |
| Channels, favorites, locks, guide, watch progress | `%APPDATA%\Relay\Relay` |
| Images and the live TV buffer | `%LOCALAPPDATA%\Relay` |
| Recordings | `Videos\Relay` (or the folder chosen in Settings) |

Uninstalling asks whether to remove your data too (recordings are always kept). With the zip,
update by extracting the new one over the old folder.

The version number comes from `project(Relay VERSION …)` in CMakeLists.txt; raise it before
packaging a new release. **Releasing:** raise the version, commit, then `git tag v<version>` and
`git push origin v<version>`. GitHub builds it and publishes a release with the installer, and
installed copies of Relay offer it as an update. MSYS2's mpv is GPL-licensed: fine for yourself, but giving the packaged
app to others means following the GPL (or switching to an LGPL libmpv build).

### macOS package

`./packaging/package-macos.sh` makes `dist/Relay.app` (with Qt and mpv inside) and a `.dmg`.
To update, drag the new Relay over the old one; settings in `~/Library` are kept.

### Windows with the official Qt installer

1. Install Qt 6.8 with the **MinGW 64-bit** kit using the Qt online installer.
2. Download a libmpv development package, e.g. `mpv-dev-x86_64-*.7z` from the
   shinchiro mpv-winbuild-cmake releases, and extract it to `C:\libs\mpv`
   (it contains `include\mpv\`, `libmpv.dll.a` and `libmpv-2.dll`).
3. From the Qt MinGW command prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMPV_DIR=C:\libs\mpv
cmake --build build
windeployqt --qmldir qml build\relay.exe
build\relay.exe
```

`libmpv-2.dll` is copied next to `relay.exe` automatically.

> Using MSVC instead? If the mpv package has an `mpv.def` file, generate an import library:
> `lib /def:C:\libs\mpv\mpv.def /name:libmpv-2.dll /out:C:\libs\mpv\mpv.lib /machine:x64`.

### Linux

```sh
sudo apt install qt6-declarative-dev qt6-base-dev libmpv-dev zlib1g-dev cmake \
  libqt6sql6-sqlite qml6-module-qtquick-controls qml6-module-qtquick-dialogs
cmake -S . -B build && cmake --build build -j && ./build/relay
```

### Tests

```sh
cd build && ctest --output-on-failure
```

`test_sources` covers M3U parsing edge cases, Xtream Codes responses, grouping, zapping,
number entry and last-channel restore. `test_epg` covers XMLTV times and time zones, gzip
and chunked input, damaged files, name matching, stop-time inference and the full import.

## Builds on GitHub

Every push to `main` builds Relay on GitHub's Windows machines (`.github/workflows/windows.yml`):
MSYS2 UCRT64 with the same packages as above, the unit tests, then
`packaging/package-windows.sh`. The installer and the zip are kept with the run for 30 days:
**Actions → Windows build →** the run **→ Artifacts**. A change to the README alone doesn't start
a build. **Run workflow** on that page starts one by hand.

To publish a release:

1. Raise the version in `CMakeLists.txt` (`project(Relay VERSION 0.2.0 …)`) and push it.
2. Tag that version and push the tag:
   ```
   git tag v0.2.0
   git push origin v0.2.0
   ```
3. The build attaches the installer and the zip to a new GitHub Release, with notes listing the
   changes since the last one. A tag that doesn't match the version in `CMakeLists.txt` stops the
   build instead.

A build takes about 20 minutes. GitHub's free allowance of build minutes for private
repositories counts Windows minutes double.

## Command line

```
relay --playlist <url-or-file>   open (and remember) an M3U playlist
relay --fullscreen               start fullscreen
```

Troubleshooting environment variables: `RELAY_MPV_LOG=v` prints mpv's log,
`RELAY_HWDEC=no` turns off hardware decoding for problem GPU drivers.

## Project layout

```
src/
  app/        AppSettings (saved playlist, last channel), Session (load/save coordinator)
  sources/    Channel, M3uParser, XtreamParser, PlaylistService (async download + parse)
  models/     ChannelModel (groups, filtering, zapping order)
  player/     PlayerController (tune/zap/number entry, now/next), MpvItem (libmpv video in Qt Quick)
  epg/        XmltvParser (streaming, gzip), EpgMatcher, EpgStore (SQLite), EpgService
  main.cpp
qml/          Main, ChannelPanel, InfoBar, SetupScreen, NumberEntry, Toast, Theme, controls
tests/        unit tests
```

## Implementation notes

- Video renders through libmpv's OpenGL render API inside Qt Quick, so the app forces Qt's
  OpenGL backend (`main.cpp`). On macOS that uses Apple's OpenGL 4.1 core profile.
- Playlists download and parse on worker threads; the UI never blocks on a large list.
- A new playlist is saved only after it loads successfully.
- The guide lives in `guide.sqlite` in the app data folder (on Windows,
  `%APPDATA%\Relay\Relay`). A failed or mismatched guide update keeps the previous guide.
- Passwords, tokens and keys go to `SecretStore` (Windows Credential Manager, macOS Keychain);
  where there's no credential store (Linux for now), they stay in the settings file.

## What's next

- **User profiles** and **radio channels**.
- Casting: subtitles and audio track choices on the TV.
- Ready-made downloads for Mac and Linux.

## Reporting problems

Bugs and ideas: [Issues](../../issues). For a security problem, please use **Report a
vulnerability** on the [Security](../../security) tab instead, so it stays private until it's
fixed.

## License

Relay is free software under the [GNU General Public License v3.0](LICENSE): you can use, share
and change it, and if you share a changed version, you share its source under the same license.

The Windows download includes other people's open-source work, each under its own license:
[Qt](https://www.qt.io/licensing/) (LGPL v3), [mpv](https://github.com/mpv-player/mpv) (GPL as built
here), [FFmpeg](https://ffmpeg.org/legal.html) (GPL as built here), and the
libraries MSYS2 builds them with. Their source is available from those projects and from
[MSYS2](https://packages.msys2.org).

Movie and series details and images come from [TMDB](https://www.themoviedb.org). This product
uses the TMDB API but is not endorsed or certified by TMDB. Trakt is a trademark of Trakt, Inc.;
Relay isn't affiliated with Trakt, Chromecast (Google) or any IPTV provider.
