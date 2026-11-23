# What's new in Relay

Each version's section here becomes the text of its release on GitHub, and Relay shows it
under **What's new** when it offers the update. Write it for the people using Relay.

## 0.3.2

- **Type from your phone:** when a search or other text box is open in Relay, the phone remote
  shows a box to type in with your phone's keyboard, and **Enter** sends it. The new **Type**
  button opens Relay's search to get you started.
- **LB / RB switch tabs** in Movies & series: Movies, Series and Recordings, round and round
  (⏪ / ⏩ on the phone or a remote do the same there).
- **Movies and episodes in multi-view:** a **Multi-view** button on a movie's or series' page
  puts it in a tile next to your channels, and the multi-view button while a movie plays now
  keeps it playing in a tile. Space pauses the selected movie, LB / RB skip back and ahead, and
  **Watch full screen** carries on with it from where it got to. Its place is kept in Continue
  watching.
- **Casting, more reliable:**
  - Chromecast (and TVs with Chromecast built in, like LG): each piece of the stream now starts on
    a full picture, which TVs need to begin playing; when the provider drops the channel, Relay
    reconnects and tells the TV to carry on; and if the TV gives up on a live channel, Relay
    starts it again on the TV.
  - DLNA TVs (Samsung and others): a live channel uses one connection to the provider however
    many times the TV asks for it (many providers allow only one), starts on a full picture, and
    keeps going when the provider drops and Relay reconnects.
  - The log says more about what the TV did, and less about every piece it fetched.
- **Updates:** Relay checks when it starts and then every six hours (it was once a day).
- Fixed: on Linux and macOS, Relay ignored Ctrl+C and the system's request to quit while a game
  controller library was loaded.

## 0.3.1

- **Settings, reorganized:** a list of sections on the left (Playlist, TV guide, Playback &
  recording, Movies & series, Trakt, Controls, Parental controls, Updates & backup) and the
  chosen one on the right, instead of one long page. It works the same with a mouse, a remote,
  a controller or the phone: ↑ ↓ choose a section, → or OK goes in, arrows move between the
  options (↑ ↓ by row, ← → along it), Back returns to the list.
- **Getting around with a controller or the phone:** the Menu button (X on a controller) now
  opens Movies & series, Recordings and Settings too, from any screen.
- **A Back button on a movie's or series' page**, and the mouse's back (thumb) button goes back
  everywhere.
- Fixed: a harmless "Cannot open file" message on every channel change (from 0.3.0), and missing
  backdrops when a provider sends an incomplete picture address (TMDB fills them in now).

## 0.3.0

- **Movies and Series, redesigned:** they open like a streaming service now. A spotlight shows
  the selected title with its backdrop, details and plot, and its trailer plays quietly behind
  it after a moment. Under it are rows: Continue watching, Favorites, Recently added, Top rated,
  and each of your provider's categories, each ending in **See all** (the full grid, as before).
  The title page has a bigger backdrop and a **Trailer** button that plays it with sound.
  Settings can stop trailers starting by themselves.
- **Game controllers:** Xbox, PlayStation, Switch and other controllers work, by cable or
  Bluetooth. A is OK, B is back, Y opens the guide; Settings lists every button.
- **TV-style PC remotes:** Back, OK, Home, Menu, Guide, Info, Search, Favorites, Subtitles and
  Audio buttons now do what they say, and rewind/fast-forward buttons work.
- **Your phone as a remote:** turn it on in Settings, scan the QR code, enter the code once.
  No app needed; only phones on your home network that know the code can use it.
- **Right-click menus:** right-click the live picture for channel info, the guide, multi-view,
  picture-in-picture, casting and favorites. Right-click a channel in the list to watch it in
  multi-view. Menu items are easier to read.
- **Casting:** adding a Fire TV or Roku as a Chromecast now says plainly that it isn't one.

## 0.2.4

- **Casting to Samsung TVs:** many providers send channels as HLS (a playlist of short pieces),
  which Samsung TVs don't play. Relay now joins the pieces into one continuous stream the TV
  does play. It asks each TV what it plays and picks the way that works, including a second try
  if the TV refuses the first.
- **Cast and multi-view on movies and episodes:** the same icons as on live TV, next to Audio
  and Subtitles (T and Q work too). Multi-view keeps your place in the movie and opens live
  channels in tiles.
- **Smaller windows:** the movie controls no longer overlap (the play buttons move next to the
  time), and the Recordings tab is no longer hidden behind the order button.

## 0.2.3

- **Casting to Samsung and other smart TVs:** Relay now asks the TV which kinds of video it
  plays and describes the channel in the TV's own words. Samsung TVs showed *"An unexpected
  error has occurred"* for live channels because of this.
- **When a TV can't play something, Relay says so** and goes back to playing it here, instead
  of waiting on "Starting…".
- Relay's log now notes what the TV asked for and answered, which helps if a TV still refuses.
- **TV guide in a smaller window:** the programme description no longer runs over the Watch and
  Record buttons. It's shortened to the room there is, and when the buttons don't all fit they
  drop their key hints, then their names (hover to see them).

## 0.2.2

- **Fixed: Relay closing by itself as it starts on some PCs.** Before your playlist finished
  loading, Relay could read memory it shouldn't have; on some computers that made it close
  straight away, every time. If that happened to you, install this version by hand once
  (Relay couldn't stay open long enough to update itself); after that, updates are automatic.

## 0.2.1

- **Settings is tidier:** the GitHub token field is gone. Relay's releases are public now, so
  checking for updates needs no setup at all.
- **Licenses included:** the download now comes with Relay's license (GPL v3) and the licenses
  of everything it's built with (Qt, mpv, FFmpeg and more), in the `licenses` folder.

## 0.2.0

- **Automatic updates:** Relay checks for a new version once a day. When there is one, a card in
  the corner offers it: **Update now** downloads it, checks it, installs it and opens Relay
  again, with your playlists and settings kept. **Later** skips that version. Settings →
  Updates has **Check now** and can turn checking off.
- **A way out of Settings with the mouse:** a ✕ in the top-right corner closes Settings (on
  pages inside Settings, such as hidden channels, it goes back a step).

## 0.1.0

The first version: live TV, the TV guide, movies and series, catch-up, recordings, multi-view,
Trakt, casting and more. See the README for everything Relay does.
