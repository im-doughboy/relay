# What's new in Relay

Each version's section here becomes the text of its release on GitHub, and Relay shows it
under **What's new** when it offers the update. Write it for the people using Relay.

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
