# What's new in Rondo

Rondo's releases, newest first. Each one is on the
[releases page](https://github.com/jarvissax/rondo/releases), with the firmware (`rom.zip`) for
Innioasis Updater CE and the update for a Y1 that already runs Rondo.

## 2.0.0 (November 11, 2026)

A new firmware, with Rondo rebuilt as a program of its own, and YouTube that no longer needs a
server. Install it with Innioasis Updater CE and choose **Keep Data** to keep your settings
([moving from 1.0](docs/install.md#moving-from-rondo-10)). From then on, updates come over Wi-Fi
straight from GitHub.

### YouTube without a server

- **YouTube and YouTube Music straight from the Y1.** Settings › YouTube › Connection: **Directly
  to YouTube** or **My server**. Directly, there is nothing to set up: home, search, albums,
  artists, playlists, up next, Shorts and playback come from YouTube itself, and lyrics from
  YouTube Music and LRCLIB. A Y1 without a server uses it on its own.
- **Sign in to YouTube directly, with a code** like a smart TV: on the Y1 (Settings › YouTube ›
  YouTube account) or the dashboard's YouTube page. It brings YouTube's home, Subscriptions and
  History, and YouTube Music's playlists and Liked Music; a cookies.txt (dashboard) adds YouTube
  Music's own home and history. Both stay on the Y1.
- **Your own server is optional now.** With **My server**, the Y1 reaches YouTube only through
  it, as in 1.0, and it adds **Headphone match** (below). A server you already have keeps working.

### Subtitles and downloads

- **Subtitles** on YouTube videos and on the videos on the card: hold center while a video plays and
  pick a track under **Subtitles**. YouTube's tracks are the video's own and the speech
  recognition's; on the card they're the .srt, .vtt and .ass files beside the video, named as Jellyfin
  and Plex name them ("Movie.en.srt"). The last language picked is shown first next time.
- **Their look, your way:** Settings › Display › Subtitles (and the dashboard's General page, with a
  preview): size, text color and opacity, background color and opacity, and edges (drop shadow,
  raised, depressed, outline). It starts as YouTube's own, white on a 75 % black box, and **Use
  YouTube's look** goes back to it. Through your server, YouTube's subtitles need the 2.0.0 server.
- **Download YouTube videos** to watch with no connection, at 720p, 480p or 360p (the next height
  down when a video lacks it), with their subtitles: hold center on a video in a list, choose
  **Download** in a playing video's options, or paste a link on the dashboard's YouTube page. They're
  saved in Videos/YouTube and show in Library › Videos and in **YouTube › Home › Downloads** (there
  even with no connection), where you can cancel one on its way or delete one. Organize leaves them
  where they are.

### Rondo, rebuilt

- **Rebuilt, with Android's app layer gone.** Rondo is now a program of its own that draws straight to
  the screen, so it starts sooner and every screen opens and scrolls more smoothly. Everything looks
  and works as in 1.0.
- **60 frames a second.** The screen refreshes at 60 Hz instead of 42, so scrolling and slides are
  smoother and videos at 30 frames a second play evenly.
- **Motion like an iPod.** Screens slide in and out, lists and their highlight glide, and artwork
  fades in. On Now Playing, the next song's cover slides in from the right and the one before from
  the left.
- **Shorts swipe like on a phone:** the short slides up and away as the next one comes in.
- **Pocket-safe wake.** With the screen off, any press only wakes it; nothing else happens. Within
  5 seconds of the screen dimming or going dark by itself, a press wakes it and does what it does
  (after Power, a press still only wakes it).
- **Wired headset buttons** work: play/pause, next, previous and volume, screen on or off.
- **Back where you were.** After a restart, the queue and the place in the song come back.

### Library

- **Library in four:** Music (in groups: Browse, Playlists, Listening, the card), Videos (all, the
  newest, by folder), Photos and **Files**, a file manager for the card and the internal storage:
  open, rename, copy, move (between the two storages too, in the background), delete, new folder.
  Plays, ratings and playlists follow what moves.
- **A richer library:** Album artists and Genres. Albums show their year, two albums of one name by
  different artists stay apart, and songs play in disc and track order. Most played, Recently
  played and Top rated; rate a song in Now Playing (hold center) or on the dashboard. Rondo reads
  every song's tags once more after the update.
- **Cover Flow** (Library › Music › Browse) on the Y1 and the dashboard: the albums' covers on a
  wide stage, the one in front facing you.
- **Photos** on the Y1 and the dashboard: the pictures on the card by folder, full screen, sliding
  from one to the next, with a slideshow; camera photos turned the right way up.
- **Smart playlists:** songs picked by rules (a genre, a year, a folder, ratings, plays, when they
  were added or last played), made on the dashboard and played from Library › Playlists. They
  follow your library as it changes.
- **Organize the card** (Library, and on the dashboard): songs into Artist/Album (Year) folders,
  episodes into Shows and other videos into Movies, as Jellyfin, Plex and Kodi expect. You see every
  move first; lyrics, subtitles, covers, playlists, plays, ratings and the queue go along.
- **Every common music format:** WMA, APE, WavPack, AIFF, AC-3, E-AC-3, DTS, DSD, TTA, TAK,
  Musepack, MKA, CAF, AMR, MP2 and AU besides MP3, AAC, ALAC, FLAC, Ogg, Opus and WAV, with their
  tags, lengths and covers; surround mixes down to stereo.

### Sound

- **Settings › Sound:** an equalizer with presets or ten bands of your own, kept for each output (the
  speaker, wired headphones, each Bluetooth device). **Edit equalizer** has a slider for each band,
  as on the dashboard, and saves your bands as a preset.
- **Headphone match:** a correction for your headphone model, from AutoEq, through your server.
- **Loudness boost** (peaks held, never clipped) and **volume leveling** (songs at one loudness).
- **Amplifier:** the Y1's own amplifier, as it ships or 2, 4 or 6 dB louder.
- **Volume limit:** the loudest step the wheel can set, on every output.
- **Opus for YouTube Music:** songs stream as YouTube's Opus where the song has it (Now Playing
  says "Streaming · Opus"), else AAC as before. Through your server, this needs the 2.0.0 server.

### Settings and the clock

- **Settings in groups:** the Dashboard, then Connections, YouTube, Sound, Display, Controls and
  System.
- **A glyph and a color for every row:** no screen shows one twice; Shuffle and Repeat have theirs
  everywhere, and every list of songs starts with Shuffle.
- **The date and the time in the status bar,** in your time zone, kept while the Y1 is off.
  YouTube's headers keep only the battery, to leave room for their tabs.
- **Settings › System › Date & time:** the time and the time zone each set automatically or by hand. Set
  automatically, the Y1 takes the time from the network and the time zone of wherever it is online,
  each time it joins Wi-Fi. By hand, you set the date, the time and the time zone yourself, and choose
  24-hour time or AM/PM. A new Y1 starts in UTC (it used to stay on China's time zone).
- **Current time zone rules.** The firmware's time zone data was from 2012, so clocks in Russia, Turkey,
  Mexico and other places that changed their rules since were an hour off for part of the
  year. Rondo's firmware has IANA's 2026 data.
- **A fixed Wi-Fi address.** The Y1 shipped without one, so it showed up on the network as a new
  device at every start. Rondo now gives it one of its own, the same after any reinstall. Settings ›
  System › About shows it.

### The dashboard

- **Every setting:** a Sound page (each output's equalizer, your own presets, headphone match,
  loudness boost, volume leveling, the amplifier, the volume limit), Subtitles, Date & time, and
  the choice between YouTube directly and your server, with the YouTube sign-in and downloads.
- **In groups:** Connections (Wi-Fi, Bluetooth, YouTube) and Settings (Sound, Theme, General); its
  Library in Music, Videos, Photos and Files, with Shuffle, sorting by genre, year, plays and
  rating, and Cover Flow.
- **A file manager:** the dashboard's Files makes folders, renames, moves and deletes (several at
  once), uploads into any folder and downloads files.
- **On a phone:** five tabs (Overview, Library, Radio, YouTube, Settings) instead of nine, the
  Library's search on a row of its own, the outputs in Sound one per line. The server's admin page
  fits a phone too.
- **Uploads** of photos from the Library go to Pictures, and videos to Movies (never into Music).
- The Overview shows the Wi-Fi address and the time zone.

### Battery and safety

- **Both processor cores, at the speed the work needs:** the second core and the lower clock steps
  are used again, as Android used them.
- **Wi-Fi saves power** with the screen off, and scans less often while it isn't connected.
- **Overheating and charger warnings are back:** "Too hot" shuts the Y1 down to cool off, and the
  charger and battery warnings (voltage too high, battery too hot, charging too long) appear as they
  did on the stock firmware, in the Y1's words.

### Updates

- **Software update comes from GitHub, with or without a server.** Settings › System › Software
  update checks the update page itself and installs a new Rondo over Wi-Fi, so a Rondo without a
  server gets its updates there too. The dashboard's **Install** downloads them from GitHub as
  before, and the update page installs them over USB.
- **Signed updates:** an update is now a signed Rondo bundle (`rondo-<version>.rondo`), and Rondo
  installs only its own. An update that fails to start three times in a row is set aside and the
  firmware's own Rondo runs.
- Rondo 1.0 hears of 2.0 on its dashboard (and, with a server, in Settings › Software update) and
  points to Innioasis Updater CE, since 2.0 needs its new firmware.

### Fixed

- Shorts: moving to the next or previous short showed the one before restarting without sound for a
  moment; the new short now shows from its own first picture.
- A video quality the video lacks fell to 360p; it now plays the next height down (720p → 480p).
- Videos shorter than 10 seconds had no thumbnail.

### Smaller changes

- It starts about a second sooner, and screens open without a stutter, also when their content
  arrives as they slide in.
- Long Wi-Fi names and titles in the header end in "…" instead of running into the status icons.
- Emoji in titles show in color, and long titles wrap without breaking them apart.
- **Artwork that comes every time.** Card videos show their picture again, in Rondo and on the
  dashboard. Covers that don't arrive at once (Wi-Fi, a busy moment) are asked for again, on the Y1
  and on the dashboard. On the dashboard, the song playing from YouTube Music shows its cover.
- The Wi-Fi switch, the music volume of each output and the clock are kept across restarts.

### Server

- The server streams YouTube's Opus audio and the videos' subtitles as well, and looks up
  Headphone match's corrections (from AutoEq). Update it with **Update** on its admin page; an
  older server keeps working, with AAC, and without subtitles or Headphone match.
- The server tells the Y1 its time zone, from the Y1's public address
  ([privacy](docs/server.md#privacy)). With an older server the Y1 stays in UTC until you set the
  time zone by hand.

## 1.0.2 (November 3, 2026)

Nothing changes on the Y1 itself; this release is about the server.

### Server

- **Update with one click.** When a newer Rondo is out, the server's admin page offers **Update**:
  the server downloads the new version and restarts on it in a minute or two, keeping your
  settings, YouTube account and connected Y1. A small companion container, `rondo-updater`, does
  the work; it only ever touches the Rondo server's own container.
- **Ready-made server images.** Installing the server now downloads it instead of building it, for
  regular PCs and 64-bit ARM (Raspberry Pi 4 and 5, many NAS boxes), which makes the first start
  much quicker on small machines.
- **Moving an older server over:** a server set up before 1.0.2 built itself and has no updater
  yet. Update it by hand once ([how](docs/server.md#keeping-it-running)); after that, **Update** on
  the admin page does it.

## 1.0.1 (November 3, 2026)

An update over Wi-Fi, the dashboard or USB: no reinstall needed, and everything on your Y1 stays.

### Fixed

- **Click sound stays off.** Turning off **Settings › Click sound** (or the switch in the
  dashboard) now lasts through a restart. Before, the Y1's own sound profile turned the click
  back on every time it started.

### Updates

- **Every release now comes with its firmware.** The `rom.zip` that Innioasis Updater CE installs
  is built automatically for each release, so a new install always gets the newest Rondo.
- **Rondo knows which updates it can install.** The firmware now carries a version of its own.
  If a future release changes the firmware itself (not only Rondo), Settings › Software update,
  the dashboard and the update page say so and point you to Innioasis Updater CE, instead of
  offering an install that couldn't work. The Y1 refuses such an update however it arrives.
- **One place for news of a release.** The Y1 (through your server), the dashboard and the update
  page all learn about a new Rondo from the update page, and only once its firmware is ready, so
  they always agree with each other and with Innioasis Updater CE.

### Server

- The server's version is now 1.0.1 as well. It reads Rondo's updates from the update page and
  tells a Y1 when a release needs a new firmware. Update it as described in
  [Keeping it running](docs/server.md#keeping-it-running).

### Documentation

- A new README with screenshots, and updated guides for installing, using, updating and the
  dashboard.

## 1.0.0 (November 2, 2026)

The first release of Rondo as a firmware: the Y1 starts straight into Rondo, with nothing else on
it. Install it with Innioasis Updater CE (Y1, Type A, then Rondo).

- iPod-style menus for the click wheel, with optional vibration and a click on every step, and
  wheel acceleration for long lists.
- Your music and videos from the microSD card, with covers, lyrics, search and playlists, and
  the card as a USB drive when the Y1 is plugged into a computer.
- YouTube Music and YouTube, streamed live through your own Rondo server, including Shorts, with
  or without a YouTube account.
- FM radio with station names, scanning and saved stations.
- Bluetooth headphones and speakers, and Wi-Fi, set up from the wheel.
- A dashboard in your phone's browser for music, playlists, settings, the theme, Wi-Fi,
  Bluetooth, the radio and updates.
- Light and dark looks, accent colors, text sizes and a main menu you can reorder.
- Shut down and restart from Settings, or by holding the Power button.
- Updates without reinstalling: from the Y1 over Wi-Fi, the dashboard, or the update page over
  USB.
