# What's new in Relay

Each version's section here becomes the text of its release on GitHub, and Relay shows it
under **What's new** when it offers the update. Write it for the people using Relay.

## 0.6.0

- **Set up from your phone:** before your first playlist, Relay shows a QR code beside the form.
  Scan it with your phone's camera and Relay's page opens on the phone, already paired: type the
  playlist's address or login there and it goes into Relay. On a computer, open the address shown
  and enter the code. The QR code pairs one phone, changes every 15 minutes, and only works on
  your home network.
- **On the LG TV, the keyboard opens when you press OK** in a text box instead of as soon as one
  is selected, so it no longer covers the screen. Search still opens it straight away.

## 0.5.4

- **Starts faster:** Relay opens on the channel list it saved last time and fetches the new one
  in the background, so a big playlist no longer keeps you waiting at every start. A new list
  with the same channels changes nothing on screen. This works for playlists with an Xtream
  Codes login or a get.php link, and the saved list never holds your password.
- **Trakt works with new Trakt apps:** Trakt no longer gives new apps a Client Secret, so Relay
  signs in with the Client ID alone, and Settings → Trakt says where to make the app
  (developer.trakt.tv). If Relay was signed in to Trakt before Trakt's changes, sign in again
  once.
- **Full screen button for movies and series:** their controls have one now, like live TV's.
- **Movies & series with several playlists:** putting their catalogues together no longer freezes
  Relay for several seconds. Your own playlist's titles show at once, and the others join a
  moment later.
- **Channel numbers of 10,000 and up** fit in the channel banner.
- **Movies and series on the LG TV:** they no longer stop after half a minute with providers that
  cut off the TV's player (error 509). Relay now passes them on to the TV's player itself, and
  plays them directly if that doesn't start. The time shows while they play, and Continue
  watching picks up where you stopped. Opening Search while the movies are still loading no
  longer closes Relay.
- **Trakt shows what you're watching** for the whole movie or episode, not just its first ten
  seconds, and adds it to your history once when you finish it.
- **Casting a movie no longer eats memory:** when a provider sends the movie on to another
  server, Relay downloaded it far ahead of the TV or Chromecast playing it. It now keeps pace
  with the player, and asks the provider again when it doesn't answer.
- **Leaving a movie or an episode** opens its page again.
- **The source chooser** closes when the movie or episode closes; it used to stay on screen.
- **Searching Movies & series** looks through all of them, whichever category was open.
- Big playlists load with about half the memory, and Relay's log hides usernames as well as
  passwords, so it's safer to share.

## 0.5.3

- **Sound and subtitle languages:** Settings → Playback & recording has a sound language
  (English, Español, Français and more) that movies, series and channels start in when they
  have it, and a subtitle choice for movies and series (Off, or a language). Picking another
  track while watching still wins.
- **Casting keeps your sound track:** a movie or episode cast to a TV or Chromecast plays in the
  language you picked (or your Settings language) instead of the file's first track. The next
  episode goes to the TV in the same language.
- Relay now uses the Inter font on every computer, so it looks the same everywhere.

## 0.5.2

- **Browse while casting:** open Movies & series, the guide or anything else and a small
  casting panel stays in the corner: how far in, pause, skip, next, back to the casting screen,
  stop. Find something and **Add to the queue**, or **Play** it to send it to the TV straight
  away. Menu (and the phone's Menu) opens the menu while a movie is on the TV; play / pause on
  the phone or a remote reaches the TV from anywhere.
- **Fixed: the next episode didn't follow on the TV** after about 20 minutes. Relay kept a
  paused copy of the episode, the provider dropped that second connection, and Relay took it as
  the episode ending. Now only the TV plays it (one connection to the provider).
- The casting screen's "next" button has its own icon (it looked like skip ahead).
- **Guide with the phone or a controller:** ⏪ ⏩ (LB / RB) change the group (All channels,
  Favorites, your provider's groups…), and **Guide** while the guide is open goes to the group
  chooser, without scrolling up past channel 1.
- Phone remote: the **Type** button is gone (Search does the same: the box to type in appears
  whenever Relay has one open).

## 0.5.1

- **Casting movies and episodes:**
  - The TV's progress shows on Relay's screen (how far in, how long left), and you can **skip**:
    ← → back 10 s / ahead 30 s, ↑ ↓ a minute, click the bar to jump, or the ⏪ ⏩ buttons
    (the phone's and a controller's too).
  - When an episode ends on the TV, **the next one starts on the TV** by itself.
  - Stopping the cast carries on here from where the TV got to.
- **A queue (Up next):** **Add to the queue** on a movie's or series' page (Q on an episode in
  Episodes). What's queued plays next, here or on the TV, before the next episode; the cast
  screen shows what's up next, and N skips to it.
- **Phone remote and remotes:** Home, Guide, Movies, Search, Favorites and Settings now go
  straight there from anywhere, Settings included. **Menu** opens the menu and closes it again.
- **Movies & series with a mouse:** grab and drag a row of posters sideways to move along it,
  or up and down to move between rows.
- The phone remote keeps its address (port 8800) when Relay restarts right after an update,
  and the log now says when a phone is turned away and why.

## 0.5.0

- **Movies & series from all your playlists, as one:** with more than one playlist, their movies
  and series now show together, and a title several of them have shows **once**. Duplicates
  within one provider (the same film in an HD and a 4K category) merge too.
- **The best source plays:** 4K before FHD before HD (from the title or its category), your
  playlist order breaking ties. Your place, favorites and watched marks belong to the title,
  whichever source played it.
- **Switch source** any time: **Source** on a title's page, or **Source** in the player bar
  (it carries on from the same moment).
- **If a source won't play** (often an account already in use), Relay shows the other sources
  so you can pick one, marking the best, the one that failed and any that are busy recording.
- Settings → Movies & series turns this off, to see only the playlist you're on. (Playlists
  from a MAC portal aren't combined yet.)

## 0.4.0

- **Movies & series, redesigned** to feel like the big streaming apps on a TV:
  - **Tabs in the middle at the top** (Search, Movies, Series, Recordings), lit when you're on
    them; **Up** from the top of the page gets there with a remote, controller or the phone.
  - A **spotlight card** with a featured title, its trailer playing inside the card, and Play and
    More info. The page takes on the colours of what you're looking at.
  - Your provider's **categories as chips** under it.
  - **Rows that rise to the top** as you move down. The selected poster **widens into the
    title's picture and plays its trailer** after a moment, and what it is (genre, year, length,
    age rating, plot) shows under the row.
  - **Title logos** instead of plain names, wherever TMDB has one (with a TMDB key in Settings).
- **A title's page, redone:** what it is on the left, and a list of actions you go through with
  ↑ ↓ (Play or Resume with how far you got, Play from the beginning, Episodes, Watch the
  trailer, Favorites, Mark as watched, Watch in multi-view, Credits & more info). The trailer
  plays behind it (muted; M for sound); leave it a moment and the text steps back so you can
  watch, and any key brings it back.
- **Episodes** on their own page: seasons on the left, episodes with their pictures and plots on
  the right.
- **Credits & more info:** director or creators, writers, cast, genres, age rating, year and
  length.
- With a TMDB key, titles now also get their **age rating** (PG-13, TV-MA…) and **writers**.
- Trailers fill their space instead of showing black bars.

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
