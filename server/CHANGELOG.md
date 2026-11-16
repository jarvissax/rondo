# What's new in the Rondo server

The server's releases, newest first (tags `server-v<version>`, on the
[releases page](https://github.com/jarvissax/rondo/releases)). Until 3.0.1 the server shared Rondo's
version, and its changes are in Rondo's [CHANGELOG.md](../CHANGELOG.md).

## 3.5.0 (not released yet)

- **Finding a video's streams takes half as long:** about 2 s instead of 4-5 s on a VPS, so a song
  or video the Rondo hasn't played before starts and downloads that much sooner. yt-dlp asks YouTube's
  visionos client first, which gives plain links (no player script to download and run), and asks
  again with its own choice of clients when that fails or misses the sound or the picture
  (`YTDLP_PLAYER_CLIENT=` in the server's environment leaves it to yt-dlp from the start). Each
  lookup's time is in the log.
- **yt-dlp's unpacked build:** it starts in about 0.65 s instead of 1.3 s (the one-file build unpacks
  itself on every run). The container installs it on the data volume and updates it from yt-dlp's
  nightly builds once a day, and the admin page's **Update yt-dlp** does the same
  (`infra/ytdlp-install.sh`); a server moves to it on its next start.
- **The next song is found ahead of time:** when the Rondo asks for a song's Up next, the server
  finds the streams of the song after it in the background. `/resolve/<id>?ahead=1` does that for
  any song or video and answers at once (Rondo 3.1.0 asks for the next song while one plays, and for
  the one you rest on in a list).
- **Home opens at once:** a home page older than 2 minutes is shown (for up to 15 more) while a fresh
  one loads; the same search within 2 minutes comes from memory.
- **The admin page's icons sit centered in their tiles** by how they look (as on the Rondo), and
  WhatsApp's logo is the size of the others.

## 3.4.0 (November 16, 2026)

- **Reacting from the Rondo** (Rondo 3.0.5): `POST /wa/chats/:jid/react` reacts to a message with
  any emoji, or takes your reaction back, and every message says your own reaction (`mine`).
- **Fixed: a reaction counted twice.** WhatsApp tells of a reaction twice, once without saying who
  sent it; the server now counts each person once, also for reactions it kept before.
- **The admin page's Overview is about your Rondo and this server:** "Everything works" means your
  Rondo is connected, and the picture shows the two of them. YouTube (which the Rondo may use
  directly) and WhatsApp keep their rows under Status, which say when they need something.
- **Update on the admin page works on a server first set up from a local build:** it moves the
  server to the published image.

## 3.3.0 (November 15, 2026)

- **WhatsApp** (Rondo 3.0.4). The server becomes one of your WhatsApp's linked devices, like
  WhatsApp Web, and keeps your chats, contacts and the photos, videos and voice messages they hold
  for the Y1, in `/data/whatsapp` on its volume. Link it on the admin page's new **WhatsApp**
  section: your number, then the code it shows typed on your phone (Linked devices › Link with
  phone number instead); it says whose WhatsApp it is and how many chats came over, and **Unlink**
  ends it and deletes what it kept. The server never shows you online and sends no notifications.
  Rondo reads and sends through new `/wa/` routes (chats, messages, contacts with their about,
  pictures, media, sending text, voice messages and photos or videos, and a feed of changes); see
  [docs/dev/server-api.md](../docs/dev/server-api.md). Your WhatsApp number never goes to the logs.
  WhatsApp's terms don't allow unofficial apps like this one, and WhatsApp may close accounts that
  use them.
- **The admin page:** **YouTube account** is now **YouTube**, and YouTube and WhatsApp show their own
  logos on their tiles; the Overview has a WhatsApp row; the seven sections fit the phone's tab bar.

## 3.2.0 (November 14, 2026)

- **YouTube Music downloads get their full details** (Rondo 3.0.3): `GET /music/song/:id` gives a
  song's artist, album, year, track number and cover, from its queue row and its album's page.
  Rondo works without it, but a song downloaded through an older server is named from its row,
  without its year and track number.

## 3.1.1 (November 12, 2026)

- **`/healthz` no longer sends the `api` number** that 3.1.0 added. Nothing reads it: Rondo tells
  whether a server is new enough from the server's version, against the oldest server it works with.
  Nothing else changes, so this update is optional.

## 3.1.0 (November 12, 2026)

- **A version of its own.** The server is released apart from Rondo on the Y1 from now on, so a
  Rondo release no longer asks you to update the server. The admin page's Overview says when a
  newer server is out.
- **`/healthz` says the server's version,** which Rondo checks against the oldest server it works
  with. (It also sends an `api` number, which nothing reads; 3.1.1 drops it.)
