# What's new in Rondo

Rondo's releases, newest first. Each one is on the
[releases page](https://github.com/jarvissax/rondo/releases), with the firmware (`rom.zip`) for
Innioasis Updater CE and the update for a Y1 that already runs Rondo. Since 3.0.2 the server has
releases of its own, in [server/CHANGELOG.md](server/CHANGELOG.md).

## Not released yet

### New

- **Podcasts, from their feeds** (a new **Podcasts** entry on the main menu, after Library; hide it in
  Settings › Display › Theme › Main menu): **Continue listening**, **New episodes** (what your shows
  put out since you subscribed, for a month), **Your shows**, **Save for later**, **Top shows**
  (Apple's charts for your country, by category), **Downloaded** and **Search podcasts** (Apple's
  directory, and Podcast Index's with a free key). The Y1 reads the feeds and plays the episodes
  straight from each podcast's host, with or without a Rondo server.
- **A show's page:** **Subscribe**, its **speed**, **Download new episodes** (on Wi-Fi, the latest 1
  to 10 kept on the card, played ones deleted if you like; nothing downloads by itself otherwise),
  **About this show**, and its episodes, newest first (a show told in order: episode 1 first), with a
  dot on the ones you haven't started and the time left on the ones you have. Thousands of episodes
  scroll as quickly as a few.
- **Episodes play as audiobooks do:** their place kept, a little earlier after a pause, chapters
  (the feed's chapter file or the MP3's own) or 30 s and 15 s skips, bookmarks, the sleep timer,
  Episodes up next and Back to your music; **speed is kept per show**. A **video episode** opens in
  the video player where you left it, and **Listen only** plays its sound with the screen off. An
  episode's options also have **Show notes**.
- **New episodes are looked for** when Podcasts opens and every 4 hours while the Y1 is on or
  playing. **Podcast settings** (Podcasts' last row) has **Look for new episodes** and the **Country**
  for the directory and the charts (the United States unless you choose another).
- **Downloads** go to `Podcasts/<show>` on the card, as the podcast's own file, beside YouTube
  Music's episodes.
- **The dashboard's Podcasts page** has these shows first (YouTube Music's below): search, Top shows,
  every list, a show's page with Subscribe, speed and Download new episodes, every episode's options,
  **Add by address** (a private or paid show's feed, read only by the Y1), **Import OPML** and
  **Export OPML** to move your shows from and to other podcast apps, the country, and without a
  server a Podcast Index key of the Y1's own.

- **Podcasts, from YouTube Music** (YouTube Music › **Podcasts**, a tab beside Home and Library):
  your saved shows, **Episodes for Later**, **New episodes**, what's downloaded, **Continue
  listening**, and search for shows and episodes (song search offers matching podcasts too; Home's
  shelves open shows and play episodes). A show's page has **Save to library**, its **speed** and its
  episodes newest first, with a dot on the ones you haven't heard and the time left on the ones you've
  started; hundreds load as you scroll.
- **Episodes play as audiobooks do:** each remembers where you stopped and starts a little earlier
  after a pause, Next and Previous move by YouTube's chapters (or skip 30 s and 15 s), with bookmarks,
  the sleep timer and Back to your music. **Speed is kept per show.** Your music waits.
- **Episodes up next:** Play next and Add to queue put episodes after the episode or book playing, or
  after the song playing; each starts where you left it.
- **Played marks are shared with YouTube Music on your phone**, both ways; an episode started on the
  phone opens about where you left it there.
- **Download** an episode to the card (`Podcasts/<show>`) to play it offline, chapters and place
  included. Episodes stay out of Songs and Shuffle songs.
- **The dashboard's Podcasts page** (a tab of its own): the same lists, a show's page with its speed,
  and every episode's options, played on the Y1.

Through a server, podcasts need **Rondo server 3.7.0** (not released yet); directly to YouTube they
work as they are. With an older server, YouTube Music › Podcasts says so and still plays what's on the
card.

The main menu's Podcasts need no server at all. With Rondo server 3.7.0 search finds Podcast Index's
shows too (with the key on its admin page), and big feeds come from it a page at a time.

### Changed

- YouTube Music's header shows the logo without the word "Music" and the battery alone when it has
  three tabs (Home, Library, Podcasts), as YouTube's does.

### Fixed

- **A book or an episode started after the card came back from a computer** (the USB drive) jumped
  to where the music had been at its first pause and play.
- **Big PNG pictures** (a 3000 × 3000 cover, common for podcasts and some albums) show instead of an
  empty square: they're read a row at a time, without needing 36 MB at once.

## 4.1.2 (November 24, 2026)

An update for the 4.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
4.1.1 does; it needs no server update.

### New

- **Audiobooks** (Library › Audiobooks): the books on the card, **Continue listening** first, each
  with its cover, author and time left. A book is everything in an `Audiobooks` folder (the stock
  firmware's, so its books carry over), any `.m4b`, or files tagged *Audiobook*; a folder of files
  is one book, its disc folders included. Books stay out of Songs, Shuffle songs, Artists, Albums,
  Genres and Organize.
  - **Where you stopped** is kept for every book, and a resume starts a few seconds earlier after
    a pause (more after a long one).
  - **Next and Previous move by chapter** (an `.m4b`'s chapters, or each file of a folder); a book
    without chapters skips 30 seconds forward and 15 back.
  - **Speed** from 0.75× to 3×, the voice's pitch kept, remembered per book.
  - **Bookmarks**, **Mark as finished**, and Now Playing with the chapter and the time left in it
    and in the book. A `cover.jpg` in a book's folder is its cover; Library's search finds books.
  - **Your music waits:** a book plays as its own queue, and the music comes back where it was when
    the book ends or with **Back to your music**.
- **Sleep timer**, for music too, in Now Playing's options: 15 to 60 minutes, or the end of the
  song or chapter, with a 10-second fade.
- **The dashboard** has Audiobooks too (progress, chapters, bookmarks, speed, uploads into
  `Audiobooks`), and the sleep timer on its Overview.

### Changed

- **Settings › Language** is now above System.
- **The dashboard's Library on a phone:** its buttons wrap instead of running off the screen, and
  the sections show their icons, the chosen one with its name.

## 4.1.1 (November 24, 2026)

An update for the 4.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
4.1.0 does; WhatsApp's own words come in your language with server 3.6.1.

### New

- **Turn the Y1 off from the main menu** ([#6](https://github.com/jarvissax/rondo/issues/6)): hold
  center there for 2 seconds. A bar fills while you hold; let go sooner and nothing happens. The Y1
  has no power button of its own (center only switches it on), so until now shutting down took
  Settings › System › Shut down. Holding center everywhere else still opens the options.
- **The update page speaks Spanish:** it starts in the browser's language when Rondo has it, with a
  language picker.

### Fixed

- **Now Playing's cover with Large text** ([#7](https://github.com/jarvissax/rondo/issues/7)): at the
  largest text size the cover never came, only the note in its place. It's there at every size now,
  and on a Y1 that tried Large before, the covers come back by themselves.
- **The guides named a Power button the Y1 doesn't have.** They now say how to turn it off.

## 4.1.0 (November 22, 2026)

An update for the 4.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
4.0.3 does; YouTube through your server comes in your language with server 3.6.0.

### New

- **Rondo speaks Spanish** ([#5](https://github.com/jarvissax/rondo/issues/5)), and any language
  someone adds. The very first start asks for the language before anything else, as the stock
  firmware does; Settings › Language changes it at any time, at once, without a restart. A Rondo
  updated from an earlier version asks once too. The dashboard speaks the Y1's language and can
  change it from its Settings page. English stays the default.
- **YouTube and YouTube Music in your language too:** what YouTube itself writes (home and shelf
  names, view counts, "2 days ago", playlist names) comes in the language Rondo speaks: directly from
  the Y1 at once, and through your Rondo server once it runs 3.6.0 (an older one keeps answering in
  English). Search stays in English, so it finds what it always found.
- **Every text in one file per language** (`lang/en.json`, `lang/es.json`): fixing a translation
  or adding a language is one file, and needs no Y1. [Translating Rondo](docs/translating.md) says
  how, including how to see the dashboard in your language on your own computer.

## 4.0.3 (November 21, 2026)

An update for the 4.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
4.0.2 does.

### Fixed

- **The Rondo server that kept going "Unreachable":** since 4.0.1, once a WhatsApp chat was marked
  read or a message or reaction was sent, some of the Y1's requests to the server after it went out
  wrong and the server turned them down. Settings › Server said "Unreachable", WhatsApp said the
  server was too old for it, and YouTube and YouTube Music said "not found", on and off. Every
  request now goes out as it should.

## 4.0.2 (November 21, 2026)

An update for the 4.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
4.0.1 does.

### Fixed

- **Bluetooth headphones that paired but didn't connect:** right after pairing, or when you chose
  paired headphones, the Bluetooth screen started looking for devices again while they connected,
  and the connection gave up after a few seconds ("not connected"). Nothing is searched for now
  while headphones pair or connect, so they connect right after pairing.
- **Headphones that didn't come back by themselves:** after a search, the Y1 stopped accepting
  connections until Bluetooth was turned off and on, so headphones taken out of their case or
  switched on again couldn't reconnect to it. The Y1 now stays open to them, and it no longer shows
  itself to every phone around while Bluetooth is on.
- **Headphones that started loud:** each time headphones connected, they took the volume of the
  speaker or the wired headphones, and earbuds taken out of their case start playing at once. They
  now keep a volume of their own: the one you last set while they were connected.

## 4.0.1 (November 18, 2026)

An update for the 4.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
4.0.0 does.

### New

- **YouTube Music playlists download as playlists:** Download playlist saves its songs and a
  playlist of them in Library › Playlists, in YouTube's order and with the playlist's cover. A song
  already on the card isn't downloaded twice, and downloading the playlist again brings it up to
  date. Deleting it asks whether its songs go too. Now Playing's options download the song playing.
- **Downloads pause and resume,** one or all, videos too, and go on after a restart or a power cut.
  They wait by themselves while Wi-Fi is off, while the card is the computer's or full, and go on
  afterwards. The dashboard has the same controls.
- **Emoji on every keyboard,** each in its app's look: Rondo's own, YouTube's and YouTube Music's,
  and WhatsApp's. Every emoji the Y1's emoji font draws, in tabs (recent ones, then Unicode's
  groups), with skin tones, and found by name.
- **WhatsApp's reaction bar:** a message's menu starts with 👍 ❤️ 😂 😮 😢 🙏, and + opens every emoji.

### Fixed

- **Voice messages that weren't sent:** one sent right after another WhatsApp action (marking a chat
  read, a reaction, a text) could send garbage instead of the recording, or close WhatsApp and lose
  it.
- **Voice messages with a headset:** the Y1 can't use a headset's microphone, and a voice message
  recorded with one held only a steady tone. With a headset in, Rondo now records from the Y1's own
  microphone (inside, beside the jack).
- **FM radio sounded doubled:** MediaTek's chip played the radio too, a moment ahead of Rondo's own.
  Only Rondo's plays now.
- **FM radio on the speaker:** choosing Speaker kept the sound in the headphones, or played only hiss.
  The radio now plays from the speaker, with a volume of its own for each output. The headphones
  play too: the Y1's speaker is fed from their amplifier, and they stay in as the antenna (the
  Output setting says so).

## 4.0.0 (November 17, 2026)

A new firmware that saves battery, most of all with the screen off. It installs only with Innioasis
Updater CE (Y1, Type A, Rondo 4.0.0); turn on **Keep my settings** so your settings, Wi-Fi networks,
server connection and paired headphones stay. Software update on 3.x shows 4.0.0 but can't install it
([Moving from Rondo 3](docs/install.md#moving-from-rondo-3)). It works with Rondo server 3.1.0 or
newer, as 3.1.2 does.

### Battery

- **Music with the screen off after charging from a computer:** after every unplug from a computer
  except the first since the Y1 started, part of its USB controller stayed powered, and that kept the
  chip out of its deepest rest between the moments music needs it: roughly 25-50 mA more for as long
  as the Y1 ran. The new firmware turns it off after every unplug.
- **The processor with the screen off** ran at its top speed the whole time. It now slows down when
  there's little to do, as it does with the screen on; playing music takes little: about 6-15 mA less
  while you listen.
- **Wi-Fi away from home:** with the screen off, it looks for a network you know every minute instead
  of every 15 seconds, and at once when the screen comes on.
- **Video quality:** Auto and 360p say "saves battery" (Settings › YouTube › Video quality, and the
  player's options). Auto is still 360p: a widescreen video fills 480 by 270 of the screen, and 720p
  keeps both of the processor's cores busy.

## 3.1.2 (November 16, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
3.1.1 does. Shorts, fixed and smoother, through a Rondo server and directly.

### Fixed

- **Shorts raced through the list:** moving on from a short whose sound was still opening left that
  short's "Can't play" on the next one, which Shorts then skipped too, and the one after it, until the
  list ran out. Each short now plays, and four in a row that really won't (no network, the server down)
  stop it with "Can't play Shorts right now"; Next tries again.
- **Shorts never run out:** more come as you go, a page that doesn't come is asked for again, and
  Next at the end of what has come waits for more. A list kept open for more than half an hour (or
  through a restart of the server) starts again from a fresh first page instead of ending. The
  "3 of 16+" count is gone.
- **Shorts move smoothly:** a short plays at 480p at most (the Shorts box is 202 by 360; at 720p one
  frame in ten came late), and the swipe to the next one keeps all its frames: the short you leave
  pauses at once, and the next one starts as the swipe ends.
- **YouTube directly, on a phone's hotspot:** a hotspot refuses many new connections, and each refusal
  failed a request, so a short could take 5-9 s to start. Rondo now asks again at once, as it always
  has with a Rondo server: about 3 s.

## 3.1.1 (November 16, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
3.1.0 does; Rondo server 3.5.1 fixes WhatsApp's unread counts and shows mentions by name.

### New

- **An emoji font of your own:** an `emoji.ttf` in Rondo's data folder is used instead of the bundled
  emoji, drawn at the same size and in the same place, and kept through updates. With a Mac you can
  make one from Apple's emoji ([how](docs/using.md#apples-emoji-from-your-own-mac)).

### Fixed

- **Recording a voice message:** the ring around the microphone grows and settles smoothly with your
  voice (it was redrawn ten times a second, in jumps), and the time under it stays in place.

## 3.1.0 (November 16, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
3.0.5 does.

### Changed

- **Songs and videos start sooner on YouTube and YouTube Music:** while a song plays, Rondo has the
  next one's streams found, and so for the song or video you rest on in a list for half a second.
  Play and Next then start without waiting for that: through a Rondo server about 1.2 s instead of
  6-7 s for a song it hadn't found yet, and directly it saves YouTube's lookup too. Only where the
  streams are is found ahead; nothing is downloaded. The next Rondo server (3.5.0) also finds a new
  song's streams in about 2 s instead of 5, and answers these early asks at once; the servers out
  now find them too, a moment slower.
- **Icons sit centered in their tiles**, on the Rondo and on its dashboard. Each is placed by how it
  looks, not just by its outline: Library no longer sits low, the gauge (Wheel acceleration) no longer
  sits high, WhatsApp's logo no longer leans left, play triangles sit a touch right, and an odd
  leftover pixel is split instead of always going right and down.
- **YouTube Music's and WhatsApp's logos are a little smaller on their tiles**, matching the other
  icons and the dashboard.
- **The dashboard's Theme preview shows Library's own icon** (the shelf) instead of a folder.

## 3.0.5 (November 16, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
3.0.4 does; WhatsApp needs your server at 3.3.0, and reacting at 3.4.0.

### New

- **React to WhatsApp messages:** **React** on a message's menu shows 👍 ❤️ 😂 😮 😢 🙏 🔥 👏 first,
  then every emoji of the keyboard. Center on one reacts with it; **Take back** (or your own emoji
  again) takes it back.
- **Emoji in your WhatsApp messages:** the keyboard's 😊 key opens four pages of emoji (faces, hands
  and hearts, things); ‹ and › turn them, and **Del** takes off a whole emoji.
- **Search in WhatsApp:** **Search** on top of the chats finds chats and contacts by name (any case,
  with or without accents) or by three or more digits of a number; **New chat** has a Search of its
  own for your contacts. Messages aren't searched.

### Fixed

- A WhatsApp reaction showed as two; with Rondo server 3.4.0 each person counts once.

## 3.0.4 (November 15, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
3.0.3 does; WhatsApp needs your own server at 3.3.0 or newer (its admin page offers it).

### New

- **WhatsApp**, through your own Rondo server, in the main menu between YouTube Music and FM Radio.
  Your chats and groups with their pictures, unread counts and last messages; messages as bubbles
  with senders, replies, reactions, photos, videos and voice messages. Write on the wheel keyboard
  (also as a reply), record a voice message with the Y1's built-in microphone, or send a photo or a
  video from the card after seeing it. Photos open full screen, videos in the player, and voice
  messages play in place of the music, which comes back after. New chat lists your contacts, and
  every contact and group has a profile with the picture, number and about. There are no
  notifications: what came is there when you open WhatsApp, and a chat you open is marked read. The
  Y1 keeps your recent chats and the media you opened on the card (up to 1 GB), so they open at
  once, also without a connection. The server is one of your WhatsApp's linked devices; you link it
  once on its admin page ([how](docs/server.md#whatsapp)).
- **Settings › YouTube › Connection test** checks YouTube from where it comes now: directly, a search
  and a song's streams; through your server, the server's playback check.

### Changed

- **The server has its own place:** **Settings › Server** on the Y1 and a **Server** page on the
  dashboard, because more than YouTube uses it now. There: its address, the connection test,
  connecting with a code or a token, and what uses it. YouTube's own **Settings › YouTube › Connect
  to** offers **Directly to YouTube**, and **Through my server** once a server is connected.
  Connecting a server no longer switches YouTube over by itself (a Y1 that never chose still uses
  it), and WhatsApp uses the server whatever YouTube's choice.
- **YouTube, YouTube Music and WhatsApp have colored tiles** like every other entry, with their logos
  in white, on the main menu, in Settings and on the dashboard.
- **A main-menu entry that's new in an update goes where it belongs** (WhatsApp under YouTube Music),
  after the entry before it, even when you've reordered the menu; it used to go just before
  Settings.
- **"Rondo server" everywhere**, no longer "music server": it does more than music now.

## 3.0.3 (November 14, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). It works with Rondo server 3.1.0 or newer, as
3.0.2 does; with your own server, update it to Rondo server 3.2.0 (its admin page offers it) so
YouTube Music downloads get their year and track number.

### New

- **The dashboard fills in what your songs' tags leave out** (Library › Fill in). A missing genre,
  year, album or track number is looked up album by album in the iTunes Store, then MusicBrainz;
  you see what was found for each album and untick any that look wrong, then it's saved into the
  songs themselves (FLAC, MP3, M4A), where Rondo, Jellyfin and any other player find it. Only
  what's missing is filled in.
- **Lyrics for every song that has none,** from LRCLIB and timed where it has them, saved into the
  song (or an `.lrc` beside it for a kind that can't hold them). Songs with lyrics keep theirs.
- **Organize looks up a show's or a movie's years** on the dashboard when the names don't say them
  (TVmaze for shows, Wikidata for movies): you pick the right one for each, and the folders keep
  the years from then on.
- **YouTube Music downloads.** Hold center on a song and choose **Download**, or **Download all** on
  an album or playlist: the song goes to `Music/YouTube Music/Artist/Album (Year)/01 - Title.m4a`,
  YouTube's own AAC with its artist, album, year, track number, cover and lyrics (timed where LRCLIB
  has them) in its tags, as Jellyfin reads them. **YouTube Music › Home › Downloads** plays them from
  the card, also with no connection; Library › Music has them too. The dashboard's Downloads saves
  songs as well. With your own server, update it to Rondo server 3.2.0 for the year and the track
  number (an older one names a song from its row).

### Changed

- **YouTube Music's Home looks like the app's:** each section is a titled shelf of covers (square for
  songs, albums and playlists, wide for videos, round for artists), Downloads above them. Turn the
  wheel to move along a shelf, which slides as you go, and on into the next one; the background
  takes the chosen cover's color. Library stays a list.
- **A song's menu has an icon for each action** (Play now, Play next, Add to queue, Start radio,
  Download).
- **Shows and movies go inside Videos,** the way Jellyfin expects them:
  `Videos/Shows/Silo (2023-)/Season 01/`, `Videos/Shows/The Walking Dead (2010-2022)/Season 01/`
  and `Videos/Movies/Arrival (2016)/`. A show still running is `(2023-)`, one that has ended
  `(2010-2022)`. Organize moves what's in the old top-level `Shows` and `Movies` folders and
  removes those folders once they're empty. Videos uploaded on the dashboard go to Videos.

### Fixed

- **A YouTube video downloads and plays in its original language.** On a video YouTube has dubbed,
  Rondo (connected directly to YouTube) could take a dub, such as the English one of a Turkish
  video; it takes the original track now. To get the original sound on a video downloaded before,
  download it again.
- **The dashboard's notes keep their distance** from what's above and below them.
- **Changing Repeat or Shuffle no longer stops the song for a moment** (Now Playing's options, hold
  center). Rondo decodes again only when the next song is already lined up; Play next from a song's
  menu is the same.
- **Library › Files slides in** like every other screen: the first time after a start, counting a
  big card's free space held it up until its slide was over.
- **The dashboard's Wheel acceleration and Wheel vibration** can be changed again (a choice's value
  read as nothing).

## 3.0.2 (November 12, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). With your own server, update it once to Rondo
server 3.1.0 (its admin page offers it); from then on, Rondo and the server update apart.

### Changed

- **Downloads are about three times faster.** A YouTube download now comes in four parts at once
  instead of one piece after another: a 10-minute 720p video took 50 seconds instead of 145, and a
  two-hour episode about 8 minutes.
- **A download survives a dropout.** When the Wi-Fi or the internet goes away for a while, the
  download waits and carries on where it stopped, instead of failing or starting over (Software
  update's download too).
- **The Y1 stays awake while a download runs,** unplugged with the screen off too.
- **Rondo and the server have their own version numbers now.** Most releases change only the Y1, so
  they no longer ask you to update your server. This Rondo works with Rondo server 3.1.0 or newer;
  with an older one, Settings › YouTube › Connection, the dashboard's server test and the YouTube
  screens say **Server needs an update**.

### Fixed

- **YouTube's pictures in search, the feeds and on a download's row.** YouTube sent them in a format
  the Y1 can't show, so they stayed empty.
- **A download's row says what it's doing:** Downloading video, Downloading sound, Getting subtitles
  and Saving (it said "Picture").

## 3.0.1 (November 12, 2026)

An update for the 3.0 firmware: Settings › System › Software update, the dashboard or the
[update page](https://jarvissax.github.io/rondo/). A Y1 still on 2.0 installs 3.0.1 with Innioasis
Updater CE, as for 3.0.0.

### Added

- **Charging shows on the battery.** On a charger or a computer, a green bolt shows next to the
  battery in the status bar and the YouTube headers, and the charge turns green.

### Fixed

- **YouTube and YouTube Music without internet.** With Wi-Fi off they said "Loading…" for a long time;
  now they say "No Wi-Fi connection" at once and load by themselves when Wi-Fi is back. On a Wi-Fi
  network without internet they give up after 30 seconds at most, and say they couldn't reach
  YouTube instead of "HTTP 502".
- **Sharper subtitles.** Their lines sat between two rows of pixels, which blurred every letter.

### Security

- **The rest of SPWall is gone too:** its packet filter, code injector and renamed iptables, which
  nothing started, and the stock firmware's distribution tag. Rondo removes them itself on a Y1 that
  updates, and the firmware leaves them out.

## 3.0.0 (November 12, 2026)

A new firmware without the stock firmware's root backdoors. It installs only with Innioasis
Updater CE (Y1, Type A, Rondo 3.0.0); turn on **Keep my settings** so your settings, Wi-Fi networks,
server connection and paired headphones stay. Software update on 2.0 shows 3.0.0 but can't install it
([Moving from Rondo 2.0](docs/install.md#moving-from-rondo-20)).

### Security

- **The stock firmware's root backdoors are gone.** Innioasis's firmware, and so every Rondo
  firmware before this one, ran three programs as root that nothing on the Y1 needs:
  - `aggdll`, the agent of an app pusher ("zxly"): a hidden `su` and a silent app installer, which
    also put a program of its own in place of Android's `am`;
  - `wifi_init`, the downloader it starts: it checks in with a server and runs what it downloads
    as root. That server doesn't answer today;
  - SPWall (`libsp.so`), a service that runs commands as root for other programs.

  The firmware leaves all three out, with what they left behind, and puts Android's own `am` back.
  Rondo also removes them itself if it ever finds them.

### Fixed

- **A Y1 installed again keeps the firmware's Rondo.** With Keep my settings, a Y1 that took an update over
  Wi-Fi before would start that older update instead of the Rondo the firmware brought; an update
  no newer than the firmware's own Rondo is now set aside.

### Changed

- **Rondo 1.0's Android app is out of the repository.** Its source stays at the
  [v1.0.2](https://github.com/jarvissax/rondo/tree/v1.0.2) tag; the dashboard's page, the font and
  the root certificates it shared with Rondo 2.0 moved to `native/assets/`.

## 2.0.1 (November 12, 2026)

A fix for Wi-Fi on a Y1 that installed 2.0.0 without keeping its data. Such a Y1 can't update over
Wi-Fi, so install 2.0.1 with Innioasis Updater CE (Rondo, 2.0.1) or over USB from the
[update page](https://jarvissax.github.io/rondo/) in Chrome or Edge. Any other 2.0.0 gets it through
Settings › System › Software update.

### Fixed

- **Wi-Fi after a fresh install.** On a Y1 installed without keeping its data, Wi-Fi never came on
  ("Wi-Fi didn't start"): Rondo powered the Wi-Fi chip on while its driver was still loading, the
  driver started twice, and the second one left the chip stuck until the next restart. Rondo now waits
  for the driver first ([#3](https://github.com/jarvissax/rondo/issues/3)).
- **Next and Previous in Now Playing slide smoothly.** The cover slid in a quarter of a second late and
  most of the way already done, and the new cover appeared only at the end: the slide now starts on the
  press, and the covers of the songs before and after are ready before you press.

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
