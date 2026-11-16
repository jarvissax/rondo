# What's new in the Rondo server

The server's releases, newest first (tags `server-v<version>`, on the
[releases page](https://github.com/jarvissax/rondo/releases)). Until 3.0.1 the server shared Rondo's
version, and its changes are in Rondo's [CHANGELOG.md](../CHANGELOG.md).

## 3.4.1 (not released yet)

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
