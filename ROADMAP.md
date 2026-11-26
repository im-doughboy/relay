# Relay roadmap

Five releases close the gaps found by comparing Relay with TiviBro (November 2026), starting with
setting Relay up from your phone and ending with translations and Relay on LG TVs. The LG TV
(webOS) port runs alongside every release and ships with 1.0.

This file is the plan both people and Claude sessions work from. Bug fixes can go ahead at any
time. New features go in the release they belong to; anything that isn't here is agreed first and
added here before work starts.

## 0.6: Set up from your phone

Adding a provider becomes a phone job, built on the phone remote's web server and QR pairing.

1. **First screen:** with no playlist yet, Relay shows a QR code beside the usual form, with the
   address written out for a computer.
2. **On the phone:** the page opens on **Add a provider**, already paired: kind (Xtream Codes, M3U
   link, portal MAC), a name and the login.
3. **Paste the whole link:** a provider's get.php link fills in the server, username and password.
4. **Test first:** Relay signs in and answers on the phone in plain words ("1,240 channels, 18,000
   movies, guide found", "wrong password", "server not reachable").
5. **Save:** Relay adds the playlist and shows the progress on the screen and the phone.
6. **Extras:** a guide address, the user agent, and **Restore a backup** (upload a `.relaybackup`).
7. **Then a backup:** once the channels have loaded, Relay offers to make a backup (saved on the PC
   or downloaded to the phone), so another PC or TV is set up in one step. Skipping is allowed;
   Settings reminds once more after a week if there is still no backup.

Later, Settings → **Add or change a playlist on your phone** shows the same QR code.

Safety (decided): local network only, no access from outside. A one-time pairing code per QR code,
expiring after 15 minutes or once used. Logins go from the phone to Relay and are never shown back;
they stay in the system's credential store. Setup mode turns itself off once a playlist is added.

Done when:
- [ ] A new install on Windows and on the TV is set up without typing on the TV.
- [ ] A get.php link fills in all three fields.
- [ ] **Test** reports wrong logins, unreachable servers and expired accounts in plain words.
- [ ] A backup restores from the phone.
- [ ] After setup Relay offers a backup, and a second device is set up from it in under two minutes.
- [ ] Works on an iPhone and an Android phone with no app to install.

## 0.7: Reliability and trust

| Feature | What it does |
| --- | --- |
| Switch to a working copy | When a channel dies, move to the same channel in another quality (FHD, HD, SD) or another playlist, and say so. **Other versions** to switch or link copies by hand. |
| Logs without logins | Mask usernames, passwords, tokens, server names and the user's account name in `relay.log`. |
| Report a problem | Save the masked log with version, system and a few settings, and open a filled-in GitHub issue. On the TV through the phone page. |
| Encrypted backups | Lock `.relaybackup` files with a passphrase (AES-256-GCM). |
| Subscription warning | A reminder 3 days before an Xtream account ends. |

Done when: a dead channel with other copies moves to one within 10 seconds with a notice; a real
session's log has no login, server name or user name; a backup can't be opened without its
passphrase.

## 0.8: Better watching

| Feature | What it does |
| --- | --- |
| Dialogue boost and night mode | Voices over music (Off, Low, High) and evened-out loud scenes, with mpv's audio filters. |
| Skip intro, recap and credits | A button or automatic; times from chapters, then TheIntroDB and IntroDB. Skip credits brings up Up next. |
| Subtitle search | OpenSubtitles by title, season and episode in the Settings language; also .srt files and from the phone. |
| Watched at | When a movie counts as watched: 85, 90, 95 or 98%. |

## 0.9: Tidy big playlists

| Feature | What it does |
| --- | --- |
| Clean names | Strip country prefixes, quality tags, symbols and emoji, plus your own rules; keep the original for matching. |
| Logos for every channel | Fill in missing or broken logos from open logo projects (tv-logos, iptv-org), loaded from them. |
| Guide fixes | Fill channels without a guide from free public guides; detect and correct a guide in the wrong time zone. |
| Event channels | Read pay-per-view and sports-pass events from channel names into a short name, a guide entry at the right time, and **Events by time**. |
| Set-up tour | Questions built from the playlist (countries, East or West, local stations, duplicates); nothing changes until **Apply**; also on the phone page. |
| Bulk editing | The phone page grows a channel editor: hide, sort, rename and move many channels, then send to Relay. |

## 1.0: Reach

| Feature | What it does |
| --- | --- |
| Translations | Start with Spanish, Portuguese, French, German, Turkish and Arabic (right to left), following the system language. |
| Themes | A few full themes, true black for OLED, high contrast, an outline focus for viewing across the room. |
| Relay on LG TVs | The webOS port: same screens and settings, the TV's own player, a Homebrew Channel repository, updates from inside Relay. |
| Launch links | Open Relay on a channel, the guide or a search from other apps (`relay://` on Windows, launch parameters on the TV). |

## Later

Sports and My teams (live scores, a page per game, scores in a corner), your own media (SMB, DLNA,
WebDAV), user profiles, radio channels, waking the PC for recordings, matching the screen's frame
rate, subtitles when casting, and movie details without a TMDB token (needs a decision about
running a service).

## Keep our lead

One app on the PC and the TV; mpv plays nearly anything; multi-view up to four and
picture-in-picture; casting with the chosen sound track and a queue; all playlists in one
catalogue; whole-series recording; game controllers, PC remotes and an app-free phone remote;
stable releases with a tested installer. Every release keeps the Windows and webOS builds and
tests passing, and adds tests for what it brings.

## Open questions

- 0.7: should failover reach across playlists (a second provider connection) or stay inside one?
- 0.9: which open guide and logo sources are reliable enough, and what if one disappears?
- 1.0: which languages first? Does the TV version ship at 1.0 or as a beta first?
- Later: is a small Relay-run service acceptable for movie details without a token?
