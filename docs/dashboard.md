# The dashboard

The dashboard is a web page served by the Y1 itself. Use it from a phone or computer to upload
music, change settings and look after the Y1 without turning the wheel through every menu. It
needs no internet and no server, only the same Wi-Fi as the Y1.

<p align="center">
  <img src="images/dashboard.png" width="560" alt="The dashboard's Overview page on a computer">
  <img src="images/dashboard-phone.png" width="180" alt="The dashboard on a phone">
</p>

## Opening it

1. On the Y1, open **Settings › Dashboard**. It shows an address like `http://192.168.1.118:8080`,
   a QR code of it, and a 6-digit pairing code.
2. Open that address in a browser on the same Wi-Fi, or scan the QR code with your phone.
3. Type the pairing code.

The dashboard runs only while that screen is open on the Y1. Press Menu on the Y1 to stop it.
The pairing code changes every time. If someone types five wrong codes, the dashboard pauses
pairing for half a minute.

## Overview

What's playing, with play/pause, skip and a volume slider, plus the battery, how full the card
is, the Wi-Fi network and the Y1's Wi-Fi address, where YouTube comes from and the time zone, and
Rondo's version, with **Restart** and **Shut down** buttons.

When a newer Rondo is out, a row says so, with **Install**: your browser downloads the update and
sends it to the Y1, which installs it and restarts with everything kept (the dashboard stops with
it). **Update from a file**, under the Rondo row, does the same with a `rondo-<version>.rondo` you
downloaded (Rondo 1.0: an APK).

The sidebar has the pages in groups: **Overview**, **Library** and **FM Radio**, then
**Connections** (Wi-Fi, Bluetooth, YouTube) and **Settings** (Sound, Theme, General). On a phone the
tabs at the bottom are Overview, Library, Radio, YouTube and **Settings**, which lists the rest.

## Library

Everything on the card, as songs, albums (Cover Flow: arrow keys, the mouse wheel or a drag move
through the covers, and the album's songs are listed below) or videos, with search and sorting by title, artist, album, album
artist, genre, year or date added, and by what you listen to: most played, recently played and
rating. A song's details show its tags and plays, with stars to rate it.

- **Upload** music, videos and photos straight onto the card, several at a time: music goes to
  Music, videos to Movies (or the Videos folder you have), photos to Pictures; **Organize** then
  puts them where Jellyfin expects them. Expect about 3 MB a second over the Y1's Wi-Fi.
- **Play** a song on the Y1, or **delete** it.
- **Rescan** after copying files onto the card some other way.
- **Photos:** every picture on the card by folder; click one to see it large, with the arrow keys,
  a slideshow (Space) and Download.
- **Files:** the card as it is, folder by folder (and the internal storage). Make folders, rename,
  move and delete files and folders, several at once with their checkboxes, upload into the folder
  you're in (pictures, lyrics and subtitles too) and download a file to your computer. Songs and
  videos keep their plays, ratings and playlists when they move, and a moved folder takes them all.
- **Organize:** every file into the folders Jellyfin expects (as **Library › Organize the card** on
  the Y1), with the full list of moves to look through first; nothing moves until you press
  **Organize**.
- **Playlists:** make, rename and delete playlists of songs on the card, add songs (from the
  playlist, or **Add to playlist** in a song's details), put them in order and play them on the Y1.
  They're the same playlists as in the Y1's Library: `.m3u8` files in the card's `Playlists` folder.
- **Smart playlists:** songs picked by rules instead of by hand: a title, artist, album, album
  artist, genre or folder that is, contains or starts with something; a year, rating, play count or
  length at least or at most a number; added or last played in the last so many days. Match all
  rules or any, choose the order (random, by artist, most played, best rated…) and a limit. The Y1
  works the songs out whenever you open one, so it follows what you add, play and rate; it lists
  them under **Library › Playlists**. They're `.smart.json` files beside the playlists.

## Theme

<p align="center"><img src="images/dashboard-theme.png" width="640" alt="The dashboard's Theme page, with a preview of the Y1's menu"></p>

The Y1 redraws while you change things here.

- **Accent:** the color of the selection bar, from a few presets or any color you pick.
- **Appearance:** light or dark menus.
- **Text size:** small, medium or large.
- **Main menu:** show, hide and reorder the entries. Settings is always there.

## General

The same settings as on the Y1 (see [Using Rondo](using.md#settings)): video quality,
brightness, screen timeout, lyrics, wheel acceleration, wheel vibration and the click sound. The
Theme page above matches the Y1's **Settings › Display › Theme**.

**Subtitles**, as the Y1's Settings › Display › Subtitles: on or off, the language looked for first,
size, text color and opacity, background color and opacity, and edges, with a preview that changes as
you pick; **Use YouTube's look** goes back to YouTube's own.

**Date & time**, as on the Y1: the time and the time zone each set automatically or by hand, and
24-hour time. By hand, set the date and the time (or take your computer's) and pick the time zone.

## Sound

The Y1's **Settings › Sound**. Each output keeps its own sound: pick the speaker, wired headphones
or a Bluetooth device (the one music plays on now is marked), then:

- **Equalizer:** a preset, or ten bands you drag yourself; the sound follows as you drag. **Save as
  a preset** keeps your bands for every output; **Flat** sets them back.
- **Headphone match:** find your headphone model in AutoEq's measurements (through your server)
  and Rondo corrects its sound.
- **Loudness boost:** louder, the peaks held and never clipped.

For every output: **volume leveling**, the Y1's **amplifier**, and the **volume limit**, the loudest
step the wheel and the dashboard can set.

## Wi-Fi

The network the Y1 is on, your saved networks and the ones nearby. Join a network with its
password, forget one, or refresh the list. Typing a password here is much quicker than on the
wheel.

## Bluetooth

Turn Bluetooth on or off, search for headphones and speakers, pair and connect them, and forget
devices you no longer use. Put the headphones in pairing mode before you search.

## FM Radio

What the radio is playing, with the station name and signal strength, and controls to tune,
seek up and down, scan the whole band, change the volume, mute, and switch between headphones
and the speaker. You can also name, reorder and delete saved stations, save the ones a scan
found, and set the region. Wired headphones must be plugged into the Y1, because they're the
antenna.

## YouTube

Where YouTube Music and YouTube come from: **Directly to YouTube** (nothing to set up) or **My
server**, the [Rondo server](server.md) the Y1 uses, and whether it's reachable. The Y1's own
**Settings › YouTube** does the same on the Y1.

- **YouTube account** (directly to YouTube): **Sign in with a code**, as the server's admin page
  does: **Get a code**, open google.com/device, enter the code and allow access; the page updates by
  itself. That brings YouTube's home, Subscriptions and History, and YouTube Music's playlists and
  Liked Music. The Y1 can do the same itself (Settings › YouTube › YouTube account).
- **YouTube Music home and history** (optional): a **cookies.txt** from a browser, as the
  server's admin page takes it. Open a private window, sign in at youtube.com, export the cookies with a
  cookies.txt browser extension, close the window at once, then **Upload cookies.txt** here. The
  page says who is signed in. The file stays on your Rondo (only its youtube.com lines are sent) and
  is never shown; **Sign out** deletes it. A separate Google account for your Rondo is the safest
  choice. Signed in, YouTube Music has your Library and your own home, and YouTube your
  Subscriptions, History and its picks for you. With a server, the server's own account counts.
- **Video quality:** Auto, or a fixed height (it was under Settings before).
- **Downloads:** paste a YouTube link (or a video id), pick 720p, 480p or 360p and press
  **Download**: your Rondo saves it to Videos/YouTube on its card with its subtitles. The list shows
  each one on its way (**Cancel**), then the videos on the card (**Save** to your computer, or the
  bin to delete one with its subtitles). The Y1 has the same list in YouTube › Home › Downloads.

- **Connect to a server:** enter the server's address and the code from its admin page
  (**Device › Connect a Rondo**), then press **Connect**.
- **Connection test:** the Y1 calls the server and reports how long it took, whether yt-dlp is
  ready and whether an account is connected.
- **Use a token instead:** only for servers started with an `API_TOKEN` setting. Servers set up
  as in the server guide don't need it.

## For developers

The dashboard's API is documented in [docs/dev/dashboard-api.md](dev/dashboard-api.md). To work
on the page without a Y1, run the mock in `tools/dashboard-mock` (see
[Building Rondo](building.md#working-on-the-dashboard-or-admin-page)).
