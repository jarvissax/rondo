# What's new in Rondo

Rondo's releases, newest first. Each one is on the
[releases page](https://github.com/jarvissax/rondo/releases), with the firmware (`rom.zip`) for
Innioasis Updater CE and the update for a Y1 that already runs Rondo.

## 2.0.0 (not released yet)

A new firmware: install it with Innioasis Updater CE and choose **Keep Data** to keep your settings
([moving from 1.0](docs/install.md#moving-from-rondo-10)).

### YouTube without a server

- **YouTube and YouTube Music straight from the Y1.** Settings › Server › Connect to: **Directly to
  YouTube** or **My server**. Directly, there is nothing to set up: home, search, albums, artists,
  playlists, up next, Shorts, lyrics and playback (Opus, or AAC where a song has no Opus) all come
  from YouTube itself. A Y1 without a server uses it on its own. Signed out for now: Library,
  Subscriptions, History and Headphone match still need your server.

### Rondo, rebuilt

- **Rebuilt, with Android's app layer gone.** Rondo is now a program of its own that draws straight to
  the screen, so it starts sooner and every screen opens and scrolls more smoothly. Everything looks
  and works as in 1.0.
- **60 frames a second.** The screen refreshes at 60 Hz instead of 42, so scrolling and slides are
  smoother and videos at 30 frames a second play evenly.
- **Motion like an iPod.** Screens slide in and out, lists glide, and artwork fades in.
- **Shorts swipe like on a phone:** the short slides up and away as the next one comes in.
- **Pocket-safe wake.** With the screen off, any press only wakes it; nothing else happens.
- **Wired headset buttons** work: play/pause, next, previous and volume, screen on or off.
- **Back where you were.** After a restart, the queue and the place in the song come back.

### Sound

- **Settings › Sound:** an equalizer with presets or ten bands of your own, kept for each output (the
  speaker, wired headphones, each Bluetooth device).
- **Headphone match:** a correction for your headphone model, from AutoEq, through your server.
- **Loudness boost** (peaks held, never clipped) and **volume leveling** (songs at one loudness).
- **Amplifier:** the Y1's own amplifier, as it ships or 2, 4 or 6 dB louder.
- **Opus for YouTube Music:** songs stream as YouTube's Opus where your server and the song offer it
  (Now Playing says "Streaming · Opus"), else AAC as before. Needs the 2.0.0 server.

### Battery and safety

- **Both processor cores, at the speed the work needs:** the second core and the lower clock steps
  are used again, as Android used them.
- **Wi-Fi saves power** with the screen off, and scans less often while it isn't connected.
- **Overheating and charger warnings are back:** "Too hot" shuts the Y1 down to cool off, and the
  charger and battery warnings (voltage too high, battery too hot, charging too long) appear as they
  did on the stock firmware, in the Y1's words.

### Smaller changes

- It starts about a second sooner, and screens open without a stutter, also when their content
  arrives as they slide in.
- Long Wi-Fi names and titles in the header end in "…" instead of running into the status icons.
- Emoji in titles show in color, and long titles wrap without breaking them apart.
- The Wi-Fi switch, the music volume of each output and the clock are kept without Android.
- **A fixed Wi-Fi address.** The Y1 shipped without one, so it showed up on the network as a new
  device at every start. Rondo now gives it one of its own, the same after any reinstall. Settings ›
  About shows it.
- **The date and the time in the status bar,** in your time zone, kept while the Y1 is off.
- **Settings › Date & time:** the time and the time zone each set automatically or by hand. Set
  automatically, the Y1 takes the time from the network and the time zone of wherever it is online,
  each time it joins Wi-Fi. By hand, you set the date, the time and the time zone yourself, and choose
  24-hour time or AM/PM. A new Y1 starts in UTC (it used to stay on China's time zone).
- **Current time zone rules.** The firmware's time zone data was from 2012, so clocks in Russia, Turkey,
  Mexico and other places that changed their rules since were an hour off for part of the
  year. Rondo's firmware has IANA's 2026 data.

### Updates

- Updates are now signed Rondo bundles (`rondo-<version>.rondo`): Settings › Software update, the
  dashboard and the update page install them. An update that fails to start three times in a row is
  set aside and the firmware's own Rondo runs.

### Server

- The server streams YouTube's Opus audio as well, which 2.0 plays songs as. Update it with
  **Update** on its admin page once 2.0.0 is out; an older server keeps working, with AAC.
- The server tells the Y1 its time zone, from the Y1's public address
  ([privacy](docs/server.md#privacy)). With an older server the Y1 stays in UTC.

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
