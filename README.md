# Rondo

**A click-wheel firmware for the Innioasis Y1: the music on your card, YouTube Music, YouTube and
FM radio, all from the wheel.**

<p align="center">
  <img src="docs/images/main-menu.png" width="400" alt="Rondo's main menu, with the song that's playing on the right">
  <img src="docs/images/now-playing.png" width="400" alt="Now Playing, with the album cover and what's up next">
</p>

<p align="center">
  <a href="docs/install.md"><b>Install</b></a> ·
  <a href="docs/using.md"><b>Use</b></a> ·
  <a href="docs/dashboard.md"><b>Dashboard</b></a> ·
  <a href="docs/server.md"><b>Server</b></a> ·
  <a href="docs/troubleshooting.md"><b>Help</b></a> ·
  <a href="CHANGELOG.md"><b>What's new</b></a>
</p>

The Innioasis Y1 is a small music player with an iPod-style click wheel. Underneath, it runs
Android 4.2. Rondo is a firmware for it, built from the community's Android 4.2 base with
Android's whole app layer taken out: the Y1 starts straight into Rondo, a program of its own that
draws every screen itself, 60 times a second, with menus made for the wheel. Play the music on
your microSD card, stream YouTube Music and YouTube straight from the Y1, and listen to FM radio,
all without a touchscreen. You install it with Innioasis Updater CE, the same way as Rockbox or
the Y1's original software, and you can always go back.

## What it does

### Your music, from the card

Copy music onto the microSD card and Rondo sorts it by song, artist, album artist, album and
genre, using the files' tags. You get covers, Cover Flow, lyrics that scroll along with the song,
search as you type, ratings, and playlists you make on the Y1 or in your browser, including smart
playlists that pick their songs by rules. Library also has the videos and photos on the card, and
a file manager. **Organize the card** puts everything in the folders Jellyfin, Plex and Kodi
expect, after showing you every move, and the dashboard looks up a show's or a movie's years and
fills in a song's missing genre, year and lyrics. Plugging the Y1 into a computer turns the card
into a USB drive.

<table>
  <tr>
    <td align="center"><img src="docs/images/library.png" width="260" alt="Library: Music, Videos, Photos and Files"><br>The library</td>
    <td align="center"><img src="docs/images/playlist.png" width="260" alt="A playlist with album covers"><br>A playlist</td>
    <td align="center"><img src="docs/images/lyrics.png" width="260" alt="Lyrics following the song"><br>Lyrics that follow the song</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/cover-flow.png" width="260" alt="Cover Flow: album covers on a wide stage"><br>Cover Flow</td>
    <td align="center"><img src="docs/images/photos.png" width="260" alt="Library › Photos: the pictures on the card as a grid"><br>Photos</td>
    <td align="center"><img src="docs/images/video.png" width="260" alt="A video playing, scrubbing with the wheel"><br>Videos</td>
  </tr>
</table>

Every common music format: MP3, AAC/M4A, ALAC, FLAC, Ogg, Opus, WAV, AIFF, WMA, APE, WavPack,
AC-3, DTS, DSD and more. Lyrics come from the file's tags or an `.lrc` file next to it, and videos
show the subtitle files beside them. Nothing on the card ever needs the internet.

### YouTube Music and YouTube

Straight from the Y1 over Wi-Fi, with nothing to set up: home, search, artists, albums,
playlists, up next, lyrics, Shorts, and video up to 720p with subtitles. Sign in with a code, the
way you'd sign in a smart TV, for your own home, subscriptions, history and playlists, or use it
without an account. Download a video to the card to watch it with no connection. If you'd rather
the Y1 never talked to YouTube itself, a [Rondo server](#your-own-server-optional) of your own
can do that part instead.

<table>
  <tr>
    <td align="center"><img src="docs/images/youtube-music-search.png" width="400" alt="Searching YouTube Music"><br>Searching YouTube Music</td>
    <td align="center"><img src="docs/images/youtube-music-playing.png" width="400" alt="A song streaming from YouTube Music"><br>Streaming a song</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/youtube-search.png" width="400" alt="Searching YouTube"><br>Searching YouTube</td>
    <td align="center"><img src="docs/images/subtitles.png" width="400" alt="A video playing with subtitles"><br>A video with subtitles</td>
  </tr>
</table>

Streams play as they arrive. Nothing from YouTube is stored, except a video you choose to
download.

### Made for the wheel

Every screen works with the wheel and five buttons. Each wheel pulse is one step, like Rockbox,
with an optional buzz and click on every step and acceleration for long lists. Screens slide in
and out like an iPod's, and lists glide. Searches, Wi-Fi passwords and playlist names are typed
on a keyboard you move through with the wheel or the buttons. Choose a light or dark look, an
accent color and a text size, and which entries the main menu shows, in which order.

<table>
  <tr>
    <td align="center"><img src="docs/images/keyboard.png" width="260" alt="The on-screen keyboard"><br>The wheel keyboard</td>
    <td align="center"><img src="docs/images/main-menu-light.png" width="260" alt="The main menu in the light look"><br>The light look</td>
    <td align="center"><img src="docs/images/settings.png" width="260" alt="Settings, in groups"><br>Settings</td>
  </tr>
</table>

### Sound

An equalizer with presets or ten bands of your own, kept for each output: the speaker, wired
headphones and each Bluetooth device. A loudness boost whose peaks never clip, volume leveling,
the Y1's amplifier up to 6 dB louder, and a volume limit. With a server, **Headphone match**
corrects the sound for your headphone model.

<p align="center"><img src="docs/images/equalizer.png" width="400" alt="Settings › Sound › Edit equalizer: a slider for each of the ten bands"></p>

### FM radio, Bluetooth and Wi-Fi

The Y1's FM radio, with station names, a scan of the whole band, saved stations and a speaker
option; wired headphones are the antenna. Bluetooth headphones and speakers, and Wi-Fi networks,
are set up from the wheel as well. A wired headset's buttons work too.

### A dashboard in your browser

Open **Settings › Dashboard** on the Y1, then the address it shows on your phone or computer.
From there you can upload music, videos and photos, manage the files on the card, make
playlists and smart playlists, control what's playing, change the theme, the sound and every
other setting, manage Wi-Fi, Bluetooth, YouTube and the radio, and shut down, restart or update
the Y1. It runs on the Y1 itself, over your Wi-Fi.

<p align="center">
  <img src="docs/images/dashboard.png" width="560" alt="The dashboard's Overview page on a computer">
  <img src="docs/images/dashboard-phone.png" width="180" alt="The dashboard on a phone">
</p>

### Updates that keep everything

A new Rondo installs in about a minute, without reinstalling the firmware: from **Settings ›
System › Software update** over Wi-Fi (straight from GitHub, no server needed), with **Install**
in the dashboard, or over USB from [the update page](https://jarvissax.github.io/rondo/). Your
settings, Wi-Fi, playlists and music stay as they are. The rare release that changes the
firmware itself says so, and installs with Innioasis Updater CE instead.

## What you need

- An **Innioasis Y1, Type A**: one that shipped with Innioasis's own software 2.0.0 or later. Not
  sure which type yours is? See [Before you start](docs/install.md#before-you-start).
- A **computer** (macOS, Windows or Linux) with **Innioasis Updater CE**, and a USB-C cable that
  carries data, for the install.
- **2.4 GHz Wi-Fi** for YouTube, updates and the dashboard. The Y1 can't see 5 GHz networks.
- Optional: a **[Rondo server](docs/server.md)** of your own, on a computer at home that stays on
  (a 64-bit Raspberry Pi or a NAS is fine) or a small VPS. YouTube works without it.

## Getting started

1. **[Install Rondo](docs/install.md)** with Innioasis Updater CE: choose the Y1, Type A, then
   Rondo. It takes about 10 minutes, and your microSD card isn't touched.
2. **[First start](docs/install.md#first-start):** join Wi-Fi, copy your music onto the card, and
   look through Settings. YouTube Music and YouTube work as soon as the Y1 is online.
3. **Sign in to YouTube** if you like: **Settings › YouTube › YouTube account** shows a code to
   enter at google.com/device.
4. **[Learn the controls](docs/using.md)**, and open the **[dashboard](docs/dashboard.md)** on
   your phone.

### The controls at a glance

| Control | What it does |
| --- | --- |
| **Wheel** | Moves through lists. On Now Playing, a video or the radio, it changes the volume. |
| **Center** | Opens or selects. **Hold** it for options. |
| **Menu** (top) | Back. |
| **Play/Pause** (bottom) | Plays and pauses, from any screen. |
| **Next / Previous** | Next or previous song. **Hold** to fast-forward or rewind. |
| **Power** | Screen off and on. **Hold** it to shut down or restart. |

[Using Rondo](docs/using.md) has the details for every screen, and every setting.

## Your own server (optional)

<p align="center"><img src="docs/images/admin.png" width="640" alt="The server's admin page"></p>

You don't need a server: Rondo talks to YouTube itself. A Rondo server is for those who'd like
one anyway. With it, the Y1 never talks to YouTube or Google: the server browses YouTube for it,
finds the streams with yt-dlp (which keeps up with YouTube's changes and updates itself every
day) and passes them on. It has its own YouTube account, signed in on its admin page, and it adds
**Headphone match**. You run it with Docker at home or on a VPS, set it up from its admin page in
the browser and connect your Y1 with a six-digit code; a new version installs with one click
there. One server serves one Y1. See [Setting up a Rondo server](docs/server.md).

## How it works

```
 Innioasis Y1                  directly                         YouTube
┌───────────────┐  Wi-Fi  ──────────────────────────────────▶ ┌─────────┐
│ Rondo         │                                             │         │
│ music on SD   │         or through   ┌───────────────────┐  │         │
│ FM, Bluetooth │  Wi-Fi  ───────────▶ │ your Rondo server │─▶│         │
└───────────────┘                      │ (Node, yt-dlp)    │  └─────────┘
                                       └───────────────────┘
```

Rondo has its own YouTube client built in: it browses YouTube Music and YouTube the way their
apps do and plays the streams as they arrive. With **Settings › YouTube › Connection › My
server**, the Y1 talks only to your server instead, which browses YouTube with
[youtubei.js](https://github.com/LuanRT/YouTube.js), finds the streams with
[yt-dlp](https://github.com/yt-dlp/yt-dlp) and passes audio and video through to the Y1 in a form
it can play.

## The project

| Folder | What's in it |
| --- | --- |
| `native/` | Rondo itself: the program the firmware runs (C++ with the NDK, its own GLES2 UI, FFmpeg), with its YouTube client in `native/direct/` and the dashboard's page in `native/assets/dashboard/` |
| `firmware/` | The script that builds the firmware (`rom.zip`) from the community's base ROM, and Rondo's start script |
| `server/` | The optional Rondo server (Node 22, TypeScript, Fastify) |
| `infra/` | Docker and Caddy files for running the server |
| `web-updater/` | The update page (WebUSB), published on GitHub Pages |
| `tools/` | The native build and bundle scripts, mock servers for UI work, the boot screen generator, the version script |
| `docs/` | Guides, plus [notes on the Y1's internals](docs/device/README.md) and [API references](docs/dev/) |

Every release is built the same way: the signed Rondo bundle goes up, and GitHub Actions builds
the firmware from it and publishes the update page. To build it yourself, start with
**[Building Rondo](docs/building.md)**.

## Feedback and contributing

If you try Rondo on your Y1, I'd love to hear how it went, good or bad.

- **[Report a problem](https://github.com/jarvissax/rondo/issues/new?template=bug_report.yml)**
  if something doesn't work.
- **[Share an idea](https://github.com/jarvissax/rondo/issues/new?template=idea.yml)** for
  something you'd like Rondo to do.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** says what feedback helps most, how to get Rondo's log,
  and how to work on the code.

## Thanks

Rondo stands on other people's work:

- The **[y1-community](https://github.com/y1-community)**: its
  [Innioasis Updater CE](https://github.com/y1-community/Innioasis-Updater), which installs Rondo,
  and its [Android 4.2 base ROM](https://github.com/y1-community/y1-ata-rom), with the Y1's
  Wi-Fi switched on, which Rondo's firmware is built from.
- **[Rockbox's Y1 port](https://github.com/multiwirth/rockbox-y1)**, whose wheel handling Rondo
  copies, and the **[JJ Launcher](https://github.com/ismileblue/y1_launcher)** and
  **[Inniclassic](https://github.com/FabianZettl/inniclassic)** launchers, which showed how the
  Y1's buttons behave.
- On the Y1: **FFmpeg**, **curl**, **OpenSSL**, **FreeType**, **HarfBuzz**, **SQLite**,
  **libpng**, **libjpeg-turbo**, **yyjson**, **nghttp2**, and the **Roboto** and **Noto Color
  Emoji** fonts. On the server: **youtubei.js**, **yt-dlp**, **Fastify** and **sharp**.
  **[AutoEq](https://github.com/jaakkopasanen/AutoEq)**'s measurements are behind Headphone match,
  and **[LRCLIB](https://lrclib.net)** has many of the lyrics. See
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License and fine print

Rondo is released under the [MIT License](LICENSE).

Rondo is an independent project. It isn't affiliated with or endorsed by Innioasis, Google or
YouTube. YouTube and YouTube Music are trademarks of Google LLC. Rondo reaches YouTube the same way
other unofficial clients do, which YouTube's terms don't allow and which YouTube can break at
any time. Use your own account, and use it at your own risk. The pictures show openly licensed
media (Kevin MacLeod's music, Blender's open movies and public-domain photos) and synthetic test
albums made for the screenshots.
