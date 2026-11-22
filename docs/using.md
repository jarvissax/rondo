# Using Rondo

Rondo is built around the wheel and five buttons. The same few moves work everywhere, and a few
screens give them an extra meaning.

## The basics

| Control | What it does |
| --- | --- |
| **Wheel** | Moves through lists. On Now Playing, a video or the radio, it changes the volume. |
| **Center** | Opens or selects. |
| **Hold center** | Options for the highlighted item or the current screen. |
| **Menu** (top) | Back. |
| **Play/Pause** (bottom) | Plays and pauses, from any screen. |
| **Next / Previous** | Next or previous song, from any screen. |
| **Hold Next / Previous** | Fast-forward or rewind. |
| **Power** | Switches the screen off and on. **Hold it** to shut down or restart. |

When the screen is off, any press (a button, center or the wheel) only wakes it: nothing else
happens, so a press in your pocket never skips a song. Within 5 seconds of the screen dimming or
going dark by itself, a press wakes it and does what it does (after Power, a press still only
wakes it). A **wired headset's buttons** (play/pause, next, previous, volume) work with the screen
on or off, like Bluetooth headphones' buttons.

### The keyboard

Wherever you type (a search, a message, a playlist's name), the keyboard works like a TV remote:
the wheel walks the keys, Next and Previous move right and left, Play moves down, Menu moves up
(on the top row it closes the keyboard) and center types. **ABC** and **#+=** switch to capitals
and symbols. It takes the look of where it opens: YouTube's and YouTube Music's round keys,
WhatsApp's greens, Rondo's own everywhere else.

The **😊** key (on every keyboard but a Wi-Fi password's and the server's address, code and token)
opens the emoji: every emoji your Y1's font draws, in ten tabs along the top, **Recent** (your
last 30) and then smileys, people, animals, food, travel, activities, objects, symbols and flags.

- The wheel goes through the whole list, a fast spin a row at a time; **hold Next or Previous** to
  jump to the next or previous tab. Menu on the top row goes up to the tabs, where the wheel picks
  a tab; Menu there goes back to the letters.
- The line under the keyboard names the emoji you're on.
- **Tone** sets the skin tone for every emoji that has one (center: the next tone).
- **🔍** finds an emoji by its name: type part of it (heart, cat, flag, fruit) and what matches is
  the row on top; Menu goes up to it.
- **Del** takes off a whole emoji.

## Main menu

**Now Playing**, **Library**, **YouTube**, **YouTube Music**, **WhatsApp**, **FM Radio** and
**Settings**. The right side shows what's highlighted, or what's playing. **Settings › Display › Theme › Main menu** changes
the order and which entries show up.

<p align="center">
  <img src="images/main-menu.png" width="360" alt="The main menu in the dark look, WhatsApp highlighted">
  <img src="images/main-menu-light.png" width="360" alt="The main menu in the light look, YouTube Music highlighted">
</p>

## Library: music, videos, photos and files

Rondo looks through the whole microSD card (and the Y1's internal storage) for music, videos and
photos. You don't need a special folder layout. Songs are listed by title, artist and album,
using the files' tags. Without tags, a file named `Artist - Title.flac` is read the obvious way.

Library has four parts: **Music**, **Videos**, **Photos** and **Files**.

**Music** starts with Shuffle songs and Search, then **Browse** (Songs, Artists, Album artists,
Albums, Cover Flow, Genres), **Playlists**, **Listening** (Recently added, Most played,
Recently played, Top rated) and **The card** (Rescan, Organize). Every list of songs starts with
**Shuffle**.

<p align="center">
  <img src="images/cover-flow.png" width="360" alt="Cover Flow: album covers on a wide stage">
  <img src="images/photos.png" width="360" alt="Library › Photos: the pictures on the card as a grid">
</p>

- **Search** finds songs, artists, albums, videos and playlists as you type, on the wheel
  keyboard. Songs, Artists, Albums, Videos and Playlists each start with a Search of their own.
- **Album artists** and **Genres** group songs by their album artist (else their artist) and their
  genre tag. **Albums** show the album artist and the year; two albums of one name by different
  artists stay apart, and an album's songs play in disc and track order.
- **Cover Flow** shows the albums' covers on a wide stage, the one in front facing you and the
  others turned and set back, each above its reflection: turn the wheel to glide through them
  (Next and Prev jump ten), press center for the album's songs.
- **Recently added** lists the newest songs first.
- **Most played**, **Recently played** and **Top rated** (4 stars and up): a song counts as played
  once it's half over or 4 minutes in.
- **Playlists:** **New playlist**, then **Add songs** (or hold center on any song and choose
  **Add to playlist**). Hold center on a song in a playlist to move it or take it out, and on a
  playlist to rename or delete it. Playlists are `.m3u8` files in the card's `Playlists` folder,
  so music players on a computer open them too. Downloaded YouTube Music playlists are with them
  (in `Playlists/YouTube Music`), marked YouTube Music and with their cover. A picture beside a
  playlist with its name (`Road trip.jpg` beside `Road trip.m3u8`) is its artwork; Photos leaves the
  `Playlists` folder out. **Smart playlists**, made on the
  [dashboard](dashboard.md#library) from rules (a genre, a year, what you play and rate), are listed
  below them and pick their songs anew each time you open one.
- **Hold center on a song** to play it now, play it next, add it to the queue or a playlist, or
  delete it from the card.
- **Videos** lists all the videos on the card, the newest first, and each folder that has some.
  YouTube videos you downloaded are there too, in `Videos/YouTube`.
- **Photos** shows the JPEG and PNG pictures on the card and the internal storage, by folder (and
  all of them, the newest first), as a grid: turn the wheel to pick one, press center to see it full
  screen. There the wheel (or Next and Prev) slides from one to the next, center shows its name and
  date, and Play starts or stops a slideshow. Pictures from a camera show the right way up.
- **Files** is a file manager for the card and the internal storage: every folder and file, not
  only music. Center opens a folder, plays a song (the folder's songs after it, an album's in disc
  and track order), a video or a photo.
  **Hold center** on a file or folder to **Rename** it, **Copy** or **Move** it to another folder
  (on either storage: between the two it copies in the background, and the list says how far it
  is), or **Delete** it. Each folder starts with **New folder**. Songs keep their plays, ratings and
  playlists when they move.
- **Rescan the card** after copying files to it another way than the USB drive.
- **Organize the card** puts every song in `Music/Artist/Album (Year)/01 - Title` (a `Disc 1`
  folder for each disc of a multi-disc album), episodes in `Videos/Shows/Series (Years)/Season 01/`
  and other videos in `Videos/Movies/Title (Year)/`, the way Jellyfin, Plex and Kodi expect, from
  the tags. A series' years come from its folder's name (`Silo (2023-)`); the dashboard's
  Organize looks up the ones the names don't say. It
  shows how many files would move and where (**See the moves**) and moves nothing until you choose
  **Organize**. Lyrics (`.lrc`), subtitles and a one-album folder's cover picture go along, and so
  do your playlists, plays, ratings and the queue.
- **Formats:** every common one for music: MP3, AAC and M4A/M4B (with ALAC), FLAC, Ogg Vorbis,
  Opus, WAV, AIFF, WMA (and WMA Lossless), APE, WavPack, AC-3 and E-AC-3, DTS, DSD (DSF and DFF),
  TTA, TAK, Musepack, MKA, CAF, AMR, MP2 and AU; surround is mixed down to stereo. MP4 (H.264) and
  similar for video, with `.srt`, `.vtt` and `.ass` subtitles beside it. **Covers** come from the
  file's embedded artwork. **Lyrics** come from the file's tags or from an `.lrc` file with the
  same name next to it, and timed lyrics scroll along. Songs on the card never fetch anything from
  the internet.

### Adding music

Plug the Y1 into a computer and choose **Use as USB drive**: the microSD card shows up on the
computer like a memory stick. Copy files onto it, eject it on the computer, then choose **Turn off
USB drive** on the Y1. While the card is on the computer, music from it is paused. Unplugging the
cable gives the card back to the Y1 as well. The dashboard can also upload music over Wi-Fi.

## YouTube Music and YouTube

Both work as soon as the Y1 is on Wi-Fi: Rondo talks to YouTube itself, with nothing to set up.
(If you'd rather it went through a [server](server.md) of your own, connect the server in
**Settings › Server**, then choose **Through my server** in **Settings › YouTube › Connect to**.) They open on a home page, with tabs across the top: turn the
wheel on the tab row and press center to switch. YouTube Music's home is shelves, as in the app:
each section a row of covers that slides along as you turn the wheel, then on into the next. **Search** uses an on-screen keyboard you type on
with the wheel.

- **Signed in**, you also get **Library** (your playlists and Liked Music) in YouTube Music, and
  **Subscriptions** and **History** in YouTube, whose home is then picked for you. Sign in with a
  code in **Settings › YouTube › YouTube account** (see [Settings](#settings)); through a server,
  the server's own account counts. YouTube Music's own home and history also need a cookies.txt (on
  the dashboard's YouTube page, or the server's admin page).
- **Hold center on a song** to play it now or next, add it to the queue, **Start radio** (songs
  like this one) or **Download** it. Now Playing's options (hold center) have **Download** too, for
  the song playing. An album has **Download all** at its top.
- **A playlist** has **Download playlist** at its top: all its songs (a mix, which never ends:
  the ones it shows when you open it), and a playlist of them in Library › Playlists, in the order
  YouTube has, under its name and marked YouTube Music, with its cover (YouTube's picture of it).
  It's a file in the card's `Playlists/YouTube Music` folder, its cover a `.jpg` beside it, and has
  the songs on the card so far, so it plays from the first one. A song you have already is the same file, whatever playlists it's in. **Update
  download** (there, or holding center on it in Library) gets its new songs and takes YouTube's
  order; a song taken off it on YouTube leaves the playlist but stays on the card. Library plays,
  shuffles and deletes it, and doesn't edit it: deleting it asks whether its songs go too (not
  those another downloaded playlist has, or that you downloaded by themselves).
- **Downloaded songs** are saved to the card the way the library keeps music, in
  `Music/YouTube Music/Artist/Album (Year)/01 - Title.m4a`: YouTube's own AAC (nothing re-encoded),
  with the song's artist, album, year, track number, cover and lyrics (timed where LRCLIB has them)
  in its tags. **YouTube Music › Home › Downloads** lists the downloaded playlists, the songs on
  their way and those on the card, also with no connection: center plays a song from the card,
  hold center deletes one (from the playlists it's in too). Library › Music has them too, and
  Organize leaves the folder as it is.
- **Pause and resume:** **Pause downloads** at the top of Downloads pauses them all (videos and
  songs: they're one queue, one at a time), and center on one pauses, resumes or cancels it. A
  paused one keeps what it has and goes on from there, after a restart too: the queue is kept on
  the Y1, and what came is saved on the card as it comes. They wait by themselves, and go on when
  they can, while Wi-Fi is off, while the card is the computer's (USB drive) and when the card is
  full; the rows say why.
- Songs stream as YouTube's Opus, or its AAC where a song has no Opus; Now Playing says which
  ("Streaming · Opus").
- Videos play at the quality set in **Settings › YouTube › Video quality** (Auto picks up to
  360p, the screen's size; up to 720p works). A video without that height plays the next one down.
- **Hold center on a video** to download it to the card (720p, 480p or 360p) with its subtitles, to
  watch with no connection. **YouTube › Home › Downloads** lists them (also with no connection):
  center plays one, hold center deletes it, and one on its way can be paused, resumed or canceled
  there. They're saved in `Videos/YouTube` on the card, so Library › Videos has them too.

<p align="center"><img src="images/downloads.png" width="360" alt="YouTube › Home › Downloads: the videos saved on the card"></p>

### Shorts

YouTube's home page starts with **Shorts**: one short at a time, looping, picked for your account
when you're signed in. **Next / Previous** move to the next or previous short,
the **wheel** is the volume and **center** pauses. Moving on slides like on a phone: the short
slides up and away as the next one comes in from below. The next few shorts load while you watch,
so moving on takes about a second.

## WhatsApp

WhatsApp works through your own [Rondo server](server.md) (3.3.0 or newer; reacting needs 3.4.0): the server becomes one
of your WhatsApp's linked devices, like WhatsApp Web on a computer, and the Y1 reads and sends
through it. Connect the Y1 to the server in **Settings › Server**, then link WhatsApp once on the
server's admin page: **WhatsApp › Get a code**, and type the code on your phone under WhatsApp's
**Settings › Linked devices › Link a device › Link with phone number instead**
([more](server.md#whatsapp)). Your recent chats come over in a minute or two.

<p align="center">
  <img src="images/whatsapp.png" width="360" alt="WhatsApp's chats: pictures, last messages, times, ticks and unread counts">
  <img src="images/whatsapp-chat.png" width="360" alt="A group chat: a photo with its caption and reactions, and a reply">
</p>

- **Chats:** the newest first (pinned ones on top), each with its picture, its last message (a mic
  and the length for a voice message, an icon for a photo or a video), the time, ticks on yours and
  the unread count. **Hold center** on a chat for its profile: the picture, the name, the number
  and the about. At the top:
  - **Search**: type a name (any case, with or without accents) or a number (three digits or more,
    wherever they are in it) and press **Search** for the chats and contacts that match; Menu goes
    back to the keyboard. It doesn't search messages.
  - **New chat** lists your contacts (Next and Previous jump a letter), with a Search of its own
    at the top for contacts only.
- **A chat** shows its messages as bubbles, the newest at the bottom, with the day between them;
  in a group, each one's sender. Replies show what they answer, and reactions sit under the
  bubble. Turn the wheel to go through the messages; up past the first, earlier ones load. Under
  them are three buttons: **Message**, **Voice** and **Photo**.
- **Center on a message** opens what it holds: a photo full screen, a video in the player, a voice
  message played (the music pauses and comes back after; center again stops it). On a text, or
  holding center on any message: the reactions on top, then **Reply**, open or play it, the
  sender's profile (in a group) and the chat's profile. A message that couldn't be sent says so:
  center sends it again.
- **Reacting:** the top row is WhatsApp's 👍 ❤️ 😂 😮 😢 🙏 and **+**. The wheel goes along it and
  then down the menu (Play jumps between them); center reacts, in place of your earlier one. Yours
  has a gray circle, and center on it takes it back. **+** slides up every emoji, as the keyboard
  has them (Recent first, the tabs, Tone, Find): center on one reacts with it, Menu on the tabs
  closes it. Once you've reacted, **Take back** is there too.
- **Message** opens the keyboard (see [The keyboard](#the-keyboard)), in WhatsApp's colors. What you
  typed and didn't send is still there next time. **Send** (the paper plane) sends it; from a
  message's **Reply**, it goes as a reply.
- **Voice** records with the Y1's built-in microphone at once (what's playing pauses): center
  sends it, Menu throws it away. Up to 15 minutes.
- **Photo** lists the photos and videos on the card, the newest first: center shows the one you
  picked, and center again sends it.

<p align="center">
  <img src="images/whatsapp-reply.png" width="360" alt="A reply quoting the message it answers, and a voice message">
  <img src="images/whatsapp-voice.png" width="360" alt="Recording a voice message: the microphone and the time">
</p>

Your messages' ticks are as on your phone: a clock while it goes, one tick sent, two delivered,
two blue read (or played).

- **No notifications.** The Y1 doesn't say a message came: it's there when you open WhatsApp. The
  server never shows you as online, so your phone keeps its own notifications.
- **Opening a chat marks it read**, as on your phone (blue ticks, if you have them on).
- **On the card:** the Y1 keeps your recent chats, and the photos, videos and voice messages you
  opened, in a hidden folder (`.rondo/whatsapp`), up to 1 GB, the oldest going first. They open at
  once, also without a connection.
- **Not yet:** opening documents (open them on your phone), calls and status updates.
- **WhatsApp's terms don't allow unofficial apps** like this one, and WhatsApp may close accounts
  that use them. You can unlink any time, on the server's admin page or on your phone under
  **Linked devices**.

## Now Playing

<p align="center">
  <img src="images/now-playing.png" width="360" alt="Now Playing with the cover">
  <img src="images/lyrics.png" width="360" alt="Now Playing with the lyrics">
</p>

- **Wheel:** volume. **Center:** switch between the cover and the lyrics.
- **Next / Previous:** the next or the song before; the cover slides in from that side.
- **Hold center** for options: scrub through the song, see what's up next, shuffle and repeat,
  and for a song on the card its **rating**: center gives it one star more (after five, none).
- **Hold Next / Previous** to scrub: the wheel then moves the position, and center jumps there.
  Menu cancels.

## Watching a video

<p align="center">
  <img src="images/video.png" width="360" alt="A video, scrubbing with the wheel">
  <img src="images/subtitles.png" width="360" alt="A video playing with subtitles">
</p>

- **Wheel:** volume. **Center:** scrub. **Next / Previous:** 10 seconds forward or back.
- **Hold center** for the options: **Quality** (YouTube), **Subtitles** (Off, or one of the video's
  tracks) and **Download** (YouTube: to the card at the height picked). A YouTube video's tracks are
  its own and the speech recognition's; a video on the card has the subtitle files beside it
  (`Movie.srt`, `Movie.en.srt`, `Movie.en.forced.ass`…). The language picked last is shown first
  in the next video.

## FM radio

Plug in **wired headphones** first: their cable is the antenna.

- **Wheel:** volume. **Center:** switch the wheel to tuning, in 0.1 MHz steps, and back.
- **Next / Previous:** find the next station up or down.
- **Play/Pause:** mute.
- **Hold center** for options: your saved stations, **Scan** (finds every station, about 20
  seconds), save or remove this one, play through the **speaker** (the headphones play too: the
  Y1's speaker is fed from their amplifier, and they stay in as the antenna), and the **region**. "Most of
  the world" is 87.5–108 MHz; Japan is 76–95 MHz. In the stations list, hold center on a saved
  station to rename, move or remove it, or on a found one to save it.

Station names come from RDS where the station sends it. Starting YouTube or a song from the card
turns the radio off.

## Settings

<p align="center">
  <img src="images/settings.png" width="360" alt="Settings, in groups">
  <img src="images/accent-colors.png" width="360" alt="Choosing an accent color">
</p>

Settings has the **Dashboard** at the top, then **Connections** (Wi-Fi, Bluetooth), **Server**
(your Rondo server), **YouTube** (where it comes from, the account, video quality), **Sound**,
**Display** (theme, brightness, screen off, subtitles, lyrics), **Controls** (the wheel) and
**System** (date and time, updates, About, shut down), and **Language** at the bottom.

| Setting | |
| --- | --- |
| **Dashboard** | Starts the dashboard; see below. |
| **Connections › Wi-Fi** | Join networks. Hold center on a saved network to forget it. |
| **Connections › Bluetooth** | Pair and connect headphones or speakers. |
| **Server** | The [Rondo server](server.md) this Y1 uses: its address, a **Connection test**, what uses it (YouTube when you choose it, WhatsApp always), **Connect to a server** with its address and the code from its admin page, and **Use a token instead**. Connecting one leaves YouTube's own choice as it is. |
| **YouTube › Connect to** | **Directly to YouTube** (no server needed), or **Through my server** once one is connected in **Settings › Server**. A switch shows the new source's home and tabs at once. |
| **YouTube › YouTube account** | Directly to YouTube: **sign in with a code**, as a smart TV does. The Y1 shows a code; open google.com/device on your phone or computer, enter it and allow access, and the screen closes by itself. That brings YouTube's home for you, Subscriptions and History, and your YouTube Music playlists and Liked Music. YouTube Music's own home and history also need a cookies.txt (on the dashboard's YouTube page). The sign-in stays on the Y1; choose the row again to sign out. Through your server, this row isn't there: the server's own account counts. |
| **YouTube › Video quality** | Auto (up to 360p), or 720p, 480p or 360p. A video without that height plays the next one down. |
| **Sound** | See [Sound](#sound) below. |
| **Display › Theme** | The accent color, light or dark, the text size, and the main menu's entries and order. |
| **Display › Brightness**, **Screen off after** | |
| **Display › Subtitles** | Subtitles on or off, the language looked for first, and their look: size, text color and opacity, background color and opacity, edges (drop shadow, raised, depressed, outline). A line at the top shows them as they'll look. **Use YouTube's look** goes back to YouTube's own (white on a 75 % black box). |
| **Display › Open lyrics automatically** | Shows lyrics instead of the cover when a song has them. |
| **Controls › Wheel acceleration** | Long, fast spins move further. |
| **Controls › Wheel vibration** | A small buzz on every wheel step: off, light, normal or strong. |
| **Controls › Click sound** | A tick on every wheel step. |
| **System › Date & time** | The time and the time zone, each set automatically (from the network, and the time zone of wherever the Y1 is online) or by hand, and 24-hour time or AM/PM. |
| **System › Software update** | Checks GitHub for a newer Rondo and installs it over Wi-Fi, keeping everything, or says when a release needs Innioasis Updater CE ([more](install.md#updating-rondo)). No server needed. |
| **System › About** | Rondo's version, the battery, free space on the card and the internal storage, Wi-Fi, the IP and MAC addresses, the time zone and where YouTube comes from. |
| **System › Shut down** | Shut down or restart. Holding the Power button opens the same choices. |
| **Language** | English or Español, each in its own name. The change is at once, everywhere (no restart), and the dashboard follows it. The very first start asks for it before anything else. To help translate, see [Translating Rondo](translating.md). |

<p align="center">
  <img src="images/theme.png" width="360" alt="Settings › Display › Theme">
  <img src="images/power.png" width="360" alt="Holding Power: shut down or restart">
</p>

### Sound

<p align="center"><img src="images/equalizer.png" width="360" alt="Settings › Sound › Edit equalizer: a slider for each of the ten bands"></p>

**Settings › Sound** shows the output music plays on now (**Playing on**: the speaker, wired
headphones or a Bluetooth device). Each output keeps its own sound:

- **Equalizer:** a preset, your own bands (**Custom**), or a preset you saved.
- **Edit equalizer:** a slider for each of the ten bands, ±12 dB. The wheel sets the gain, Next
  and Previous pick the band, center saves the bands as a preset of your own (for every output),
  and holding center sets them flat.
- **Headphone match** (headphones only): a correction for your headphone model from
  [AutoEq](https://github.com/jaakkopasanen/AutoEq)'s measurements. Search by model; this one
  needs a [server](server.md), which looks the model up.
- **Loudness boost:** up to 12 dB louder, with the peaks held so they never clip.

Under **Every output**:

- **Volume leveling:** songs at one loudness, measured as they play and while the Y1 charges.
- **Amplifier:** the Y1's own amplifier, Standard as it ships, or +2, +4 or +6 dB louder for
  headphones and the speaker; the loudest steps add some hiss.
- **Volume limit:** the loudest step the wheel goes to, on every output (No limit as it ships).

## Apple's emoji, from your own Mac

Rondo draws emoji with Google's Noto Color Emoji. With a Mac you can give your Y1 Apple's instead,
converted from your Mac's own copy of the font. Apple's emoji are Apple's, so the font isn't part of
Rondo, this repository or its releases: each Mac makes its own.

1. On the Mac, in a copy of this repository: `node tools/emoji/make-font.cjs`. It writes
   `emoji.ttf` (about 13 MB, only the one size the Y1 uses) to the folder you're in.
2. With the Y1 connected over USB: `adb push emoji.ttf /data/rondo/emoji.ttf`, then restart Rondo
   (hold Power › Restart).

Rondo takes `emoji.ttf` in its data folder over its own emoji, draws them at the same size and in the
same place, and keeps it through updates. To go back: `adb shell rm /data/rondo/emoji.ttf` and restart.
About a hundred emoji of people facing right don't draw with it, and the newest emoji only come with
a newer macOS: the keyboard offers only the emoji the font draws.

## The dashboard

**Settings › Dashboard** turns the Y1 into a small web server for as long as that screen is
open. It shows an address, a QR code of it and a 6-digit code. Open the address in a browser on
the same Wi-Fi and type the code. From there you can:

- see what's playing and control it, and shut down, restart or update Rondo,
- browse, search, upload and delete music, videos and photos, manage the card's files and
  folders, organize the card, and make playlists and smart playlists,
- change the theme, the sound, the menu and every setting above,
- manage Wi-Fi, Bluetooth and the FM radio, sign in to YouTube, download YouTube videos, and
  connect a server (its own **Server** page).

Leaving the Dashboard screen on the Y1 (Menu) stops it. See [The dashboard](dashboard.md) for
each page.
