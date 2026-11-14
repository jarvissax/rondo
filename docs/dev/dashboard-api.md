# Rondo dashboard

A web page for managing a Rondo device from a phone or computer on the same Wi-Fi: the library on
the card, files, settings, sound, theme, Wi-Fi, Bluetooth, the radio, YouTube. Served by the Y1
itself (no server needed). This page describes its API, for anyone working on the dashboard or
scripting the Y1.

## How it runs on the device

- **Settings › Dashboard** starts a small HTTP server (`native/shell/dashboard_server.cpp`) on
  port **8080** and shows the address (`http://<ip>:8080`), a QR code of it and a **6-digit
  pairing code**. The screen stays on while it is open (CPU and Wi-Fi must stay awake to answer);
  leaving the screen stops the server, and so do 15 minutes without a request. Nothing listens
  otherwise.
- The web files are in `native/assets/dashboard/` (served at `/`; `index.html` stamps `app.js` and
  `app.css` with the build so a browser fetches new ones), and the bundled font at
  `/fonts/Roboto-Regular.ttf`. `tools/native/bundle.sh` puts them in Rondo's bundle.
- Changes apply on the device immediately (theme changes redraw the screens).

## Auth

- `POST /api/pair` `{"code": "123456"}` → `200 {"session": "<token>"}` (also `Set-Cookie:
  rondo_session=<token>; Path=/; SameSite=Strict`), `403 {"error": "code"}` on a wrong code. After
  5 wrong codes the device picks a new code and pauses pairing for 30 s:
  `429 {"error": "wait", "retryAfterSec": n}`.
- Every other `/api/*` call needs the session: header `X-Rondo-Session: <token>` or the cookie.
  Without it: `401 {"error": "pair"}` → the page shows the pairing screen.
- Sessions live in memory until the server stops; the pairing code changes each time it starts.
- The API token for the music server is **write-only**: it is never returned.

## API (JSON unless noted; errors are `{"error": "<short reason or a sentence>"}` with 4xx/5xx)

### Status and player
- `GET /api/status` →
  ```json
  {"device": {"name": "Rondo", "version": "2.0.0", "model": "Innioasis Y1", "power": true,
              "canUpdate": true, "firmware": 2},
   "battery": {"level": 87, "charging": false},
   "storage": {"freeBytes": 31234567890, "totalBytes": 62500000000},
   "wifi": {"ssid": "Home", "signal": 3, "ip": "192.168.1.118"} | null,
   "about": {"mac": "02:…" | null, "timeZone": "Europe/Lisbon · 17:52", "connectTo": "Directly to YouTube" | "Not set up" | "<server url>"},
   "player": {"title": "Northbound", "subtitle": "Glass Harbor", "path": "/storage/sdcard0/…" | null,
              "art": "/api/art?path=…&size=300" | "/api/thumb?path=…" | null,
              "source": "device" | "youtube", "playing": true, "positionMs": 41000, "durationMs": 210000,
              "volume": 2, "maxVolume": 13, "volumeLimit": 13} | null}
  ```
  `art` is a URL on the device for card songs, a `/api/thumb` proxy URL for YouTube Music, or null.
  `power`: the Rondo can shut down and restart. `canUpdate`: the firmware's Rondo takes updates.
  `firmware`: the firmware's number (`ro.rondo.firmware`: 2 for the 2.0 firmware).
- `POST /api/player` `{"action": "toggle" | "next" | "previous" | "seek" | "volume", "positionMs"?: n, "volume"?: n}`
  → the `player` object, with what was asked for (the player publishes it a moment later). The
  volume stops at `volumeLimit`.
- `POST /api/player/play` `{"path": "/storage/sdcard0/Music/x.flac", "queue"?: ["…", …]}` → plays that song
  with `queue` (a playlist's songs) or else the library's songs as the queue → the `player` object.
  Songs only: a video would open the video player over Settings › Dashboard.
- `POST /api/power` `{"action": "shutdown" | "restart"}` → `{"ok": true}`; the Rondo shuts down or
  restarts about a second later.

### Updates
- **Newer Rondo:** the page itself (the browser, not the Rondo) reads the update page's
  `https://jarvissax.github.io/rondo/latest.json` and shows a newer release under Overview ›
  Device, with Install only when the release's `firmware` isn't above `device.firmware`.
  `latest.json`: `{"version", "file": "rondo.rondo", "size", "bundle", "bundleSize", "firmware",
  "notes": "<release page>", "published"}`.
- `GET /api/update` → `{"version": "2.0.0", "canInstall": true, "firmware": 2, "problem": null}`.
- `PUT /api/update` with the bundle (`rondo-<version>.rondo`) as the body (`Content-Length`, like
  uploads) → `{"version": "2.0.1"}`. The Rondo checks its signature and unpacks it
  (`native/core/bundle.h`), answers, and restarts into it about 1.5 s later, which ends the
  dashboard. `400 {"error": "<why>"}` for a file that isn't a Rondo bundle or isn't signed with
  Rondo's key, `409 {"error": "firmware"}` on a Rondo that can't install updates, `507` when
  there's no room. The page downloads the bundle from the same site (`rondo.rondo`; GitHub Pages
  lets any page fetch its files).

### The library (the card)
- `GET /api/library` →
  `{"songs": [{"path", "title", "artist", "album", "track", "disc", "year", "albumArtist", "genre",
    "plays", "lastPlayed", "rating", "durationSec", "size", "modified", "art": true}],
    "videos": [{"path", "title", "durationSec", "size", "modified"}], "scanning": false,
    "folders": ["/storage/sdcard0/Music", …]}`
  `folders`: the storages' top folders (the card's first), where the page puts an upload by its
  kind (Music; Movies or Videos; Pictures).
- `GET /api/art?path=<file>&size=<px>` → `image/jpeg`, the embedded cover (or a frame for videos),
  scaled to about `size` px; `404` when there is none. Cacheable (`Cache-Control: max-age=86400`).
- `GET /api/thumb?path=/thumb?u=…` → a YouTube thumbnail, through the server or the Y1's own
  YouTube client.
- `PUT /api/library/upload?name=<file name>&folder=<folder, default /storage/sdcard0/Music>` with
  the raw file as the body (`Content-Length` required; not multipart) → `{"path": "…"}`. Allowed:
  every music format the library reads, videos (mp4 m4v mov mkv 3gp webm), lyrics and subtitles
  (lrc srt ass vtt), pictures (jpg jpeg png gif webp bmp) and playlists (m3u8 m3u); `415`
  otherwise, `507` when the card is full. An existing file of that name gets ` (2)` added. The
  library picks it up on the next scan.
- `DELETE /api/library?path=<file>` → `{"ok": true}` (only files on the card or the internal
  storage, only media extensions). The song also leaves every playlist.
- `PUT /api/library/rating` `{"path", "rating": 0..5}` → the song, as in `songs` above.
- `POST /api/library/rescan` → `{"ok": true}`; `GET /api/library` shows `scanning` meanwhile.

### Photos
- `GET /api/photos` → `{"photos": [{"path", "modified"}]}`, every JPEG and PNG on the card and the
  internal storage, the newest first (read again at most every 20 s).
- `GET /api/photo?path=<file>&size=<px, 64–1600, default 320>` → `image/jpeg`, at most that size,
  turned the way its camera says.

### Files
The card and the internal storage as they are, for the file manager.
- `GET /api/files?path=<folder, default the card>` →
  `{"path", "parent": "…" | null, "entries": [{"name", "kind": "folder" | "song" | "video" | "image" | "text" | "other",
    "size", "modified", "title"?, "artist"?}], "roots": [{"path": "/storage/sdcard0", "name": "Card"}, {"path": "/storage/sdcard1", "name": "Internal storage"}]}`
  (hidden files left out).
- `GET /api/files/download?path=<file>` → the file as it is (`Content-Disposition: attachment`).
  `GET /api/files/view?path=<picture>` → a JPEG or PNG as it is, to show in the browser.
- `POST /api/files/folder` `{"path": "<folder>", "name": "<new folder>"}` → that folder's listing.
- `POST /api/files/rename` `{"path": "<file or folder>", "name": "<new name>"}` → its folder's listing.
- `POST /api/files/move` `{"paths": […], "to": "<folder>", "copy"?: bool}` →
  `{"moved": n, "errors": ["…"], "background": bool, "status": "…"}`. On one storage a move is
  immediate; between the card and the internal storage the files are copied, then deleted, in the
  background (`background: true`; `status` says how far). `409` while another copy runs. Songs
  keep their plays, ratings and playlists.
- `POST /api/files/delete` `{"paths": […]}` → `{"deleted": n, "errors": ["…"]}` (folders with all
  they hold).

### Organize
Library › Organize the card: songs into `Music/Artist/Album (Year)/`, episodes into
`Videos/Shows/Series (Years)/Season 01/`, other videos into `Videos/Movies/Title (Year)/`, as
Jellyfin, Plex and Kodi expect (`native/core/organize.h`).
- `GET /api/organize` → `{"songs", "videos", "sidecars", "covers", "alreadyThere",
  "moves": [{"kind": "song" | "video" | "lyrics" | "subtitles" | "cover", "from", "to"}], "status": {…},
  "titles": [title…], "lookup": {…}}` (paths relative to their storage). Nothing moves yet. `titles`:
  the series and movies whose years a lookup can find, `{"key", "series", "name", "years", "files"}`
  (`years` "" unknown, "2016", "2023-" or "2010-2022"; a series still running is asked again).
- `POST /api/organize/lookup` → starts looking the titles up (TVmaze for series, then Wikidata;
  Wikidata for movies); `GET /api/organize/lookup` → `{"running", "finished", "done", "total",
  "matches": [{"title": title, "found": [{"name", "years", "detail"}], "pick": i | -1, "failed"}]}`.
- `POST /api/organize/plan` `{"names": {"<key>": {"name", "years"}}}` → the plan with those names.
- `POST /api/organize` `{"names": {…}}` → the status; `409 {"error": "<why>"}` when it can't start.
- `GET /api/organize/status` → `{"running", "finished", "done", "total", "moved", "errors": ["…"]}`.

### Downloads
- `GET /api/downloads` → `{"dir", "jobs": [{"id", "kind": "video" | "song", "title", "channel",
  "quality", "state", "percent", "stage", "error", "file"}], "files": [video…], "songs": [{"file", "id",
  "title", "subtitle"}]}`.
- `POST /api/downloads` `{"video": "<link or id>", "quality": 480, "song": false}` → the same; with
  `"song": true` a YouTube Music song, saved to `Music/YouTube Music/Artist/Album (Year)/NN - Title.m4a`.
- `POST /api/downloads/cancel` `{"id"}`; `DELETE /api/downloads?file=<path>` (a video with its
  subtitles, or a song).

### Fill in
The dashboard's Fill in (`native/shell/fill_in.h`): missing song details and lyrics looked up
online and saved into the files. One job at a time; it runs on the Y1.
- `GET /api/fill` → `{"summary": {"songs", "noGenre", "noYear", "noAlbum", "noTrack", "albums",
  "unsupported"}, "status": status}`.
- `POST /api/fill/details` `{"countries": ["gb"]}` → status: the iTunes Stores to ask besides the
  songs' own and the US one, then MusicBrainz, album by album. Nothing is saved yet.
- `POST /api/fill/save` `{"skip": [id…]}` → status: the details found, saved into the songs (but
  the proposals skipped).
- `POST /api/fill/lyrics` → status: LRCLIB's lyrics for every song without any, saved at once.
- `POST /api/fill/cancel` → status.
- `GET /api/fill/status[?proposals=1]` → `{"kind": "details" | "save" | "lyrics", "running",
  "finished", "canceled", "done", "total", "stopped", "proposed", "notFound", "failed", "saved",
  "found", "synced", "had", "instrumental", "beside", "errors": ["<file>: <why>"], "proposals"}`;
  with `proposals=1`, `[{"id", "artist", "album", "foundArtist", "foundAlbum", "source", "genre",
  "year", "newAlbum", "songs", "tracks"}]` (what each saves: `genre`, `year`, `newAlbum` set when
  they change; `tracks` songs given a number), else `null`. 409 `{"error": "<why>"}` when a job
  can't start.

### Playlists
Songs on the card in a chosen order, kept as `.m3u8` files in the card's `Playlists` folder
(`native/core/playlists.h`): songs relative to that folder (`../Music/x.flac`), so music players
on a computer open them too, and they survive reflashing. The file name is the playlist's name (up
to 60 characters; FAT can't hold `/ \ : * ? " < > |`, which become spaces). The Rondo's
Library › Playlists works on the same files.
- `GET /api/playlists` → `{"playlists": [{"name", "paths": ["/storage/sdcard0/Music/x.flac", …], "missing": 1,
  "durationSec": 2950}]}`; `missing` counts songs no longer on the card (kept in the file).
- `POST /api/playlists` `{"name": "Road trip", "paths"?: […]}` → the playlist; `409` when the name is taken.
- `PUT /api/playlists?name=<name>` `{"name"?: "new name", "paths"?: […]}` → the playlist: a rename and/or the
  whole new song list (reordered, added to, removed from). Only audio files on the card are kept.
- `DELETE /api/playlists?name=<name>` → `{"ok": true}` (the songs stay on the card).
- Errors are `{"error": "<a sentence to show>"}`.

### Smart playlists
Songs picked by rules, worked out whenever they're opened (`native/core/smart.h`), kept as
`<name>.smart.json` beside the playlists. A playlist:
`{"name", "match": "all" | "any", "rules": [{"field", "op", "value"}], "sort", "limit": 0}`.
- Fields `title`, `artist`, `album`, `albumArtist`, `genre`, `folder` take `is`, `isNot`,
  `contains`, `notContains`, `startsWith` and text; `year`, `rating`, `plays`, `length` (seconds)
  take `is`, `isNot`, `atLeast`, `atMost` and a number; `added`, `lastPlayed` take `inLast`,
  `notInLast` and a number of days.
- `sort`: `random`, `title`, `artist`, `album`, `added`, `lastPlayed`, `plays`, `rating` or `year`.
  `limit`: songs at most (0: no limit).
- `GET /api/smart` → `{"playlists": [{…, "count", "durationSec"}]}`.
- `GET /api/smart/songs?name=<name>` → the playlist with `paths` (its songs now).
- `POST /api/smart/preview` `{"playlist": {…}}` → what those rules pick now (up to 50 `paths`), not saved.
- `PUT /api/smart` `{"playlist": {…}, "oldName"?: "…"}` → the list (saved; renamed from `oldName`).
- `DELETE /api/smart?name=<name>` → the list.

### Settings
- `GET /api/settings` →
  `{"videoQuality": null | 360 | 480 | 720, "brightness": 10..100, "screenTimeoutSec": 15|30|60|120|300|600,
    "lyricsOnOpen": false, "wheelAcceleration": "off" | "normal" | "fast",
    "wheelVibration": "off" | "light" | "normal" | "strong", "clickSound": true,
    "subtitles": true, "subLang": "" | "en" | …, "subSize": 1, "subColor": 0, "subOpacity": 100,
    "subBack": 7, "subBackOpacity": 75, "subEdge": 0,
    "subChoices": {"sizes": […], "colors": [{"name", "hex"}], "edges": […], "languages": [{"code", "name"}]}}`
  The subtitle settings are indexes into `subChoices`, as Settings › Display › Subtitles has them.
- `PUT /api/settings` with any subset → the full settings.
- `"wheelVibration"`: a buzz on every wheel step, 15 / 25 / 40 ms plus Rockbox's fading boost;
  "normal" matches Rockbox's intensity 5.

### Sound
Settings › Sound (`native/core/sound.h`): each output keeps its own sound.
- `GET /api/sound` →
  `{"current": "speaker" | "headphones" | "bt:<address>", "leveling": bool, "levelingNote": "…",
    "freqs": [31, 62, …, 16000], "presets": [{"id", "name", "note", "gains": [10 × dB]}],
    "userPresets": [{"name", "gains"}],
    "outputs": [{"key", "label", "preset", "presetName", "gains", "custom", "boostDb", "match": {"name", "source"} | null}],
    "amplifier": 0..3, "volumeLimit": 13, "maxVolume": 13}`
- `PUT /api/sound` with `"output"` and any of: `"preset"` (a preset's `id`, `"custom"`,
  `"user:<name>"`, or `"autoeq"`), `"custom": [10 gains, ±12 dB]`, `"savePreset": "<name>"` (with
  `custom`), `"deletePreset": "<name>"`, `"boostDb": 0 | 3 | 6 | 9 | 12`, `"leveling": bool`,
  `"amplifier": 0..3` (Standard, +2, +4, +6 dB), `"volumeLimit": n` → the `GET` shape.
- `GET /api/sound/match?q=<model>` → AutoEq's headphone models matching it, through the Rondo
  server (`/eq/search`); `502` without one, `404` from a server too old to have it.
- `POST /api/sound/match` `{"output", "id", "name", "source"}` → that model's correction for the
  output (`/eq/preset`), then the `GET` shape.

### Date & time
- `GET /api/datetime` → `{"autoTime", "autoZone", "clock24", "now": <epoch s>, "zone", "city",
  "offsetSec", "zones": [{"id", "city", "gmt"}]}`.
- `PUT /api/datetime` with any of `{"autoTime", "autoZone", "clock24", "zone", "now"}` → the `GET`
  shape. `zone` and `now` need the automatic setting off (`400` with a sentence otherwise).

### Theme
- `GET /api/theme` →
  ```json
  {"accent": "#1c5ccc", "appearance": "light" | "dark", "textScale": 1.25 | 1.4 | 1.55,
   "menu": [{"id": "nowPlaying", "label": "Now Playing", "visible": true}, {"id": "device", "label": "Library", …},
            {"id": "videos", "label": "YouTube", …}, {"id": "music", "label": "YouTube Music", …},
            {"id": "radio", "label": "FM Radio", …}, {"id": "settings", "label": "Settings", "visible": true, "locked": true}],
   "presets": [{"name": "iPod Blue", "accent": "#1c5ccc"}, {"name": "Rondo Red", "accent": "#ff3b4f"}, …]}
  ```
  `accent` colors the selection bar and highlights in the iPod-style menus; the YouTube and
  YouTube Music screens keep their own dark look. `menu` order is the main menu order;
  `locked` items cannot be hidden.
- `PUT /api/theme` with any subset of `accent`, `appearance`, `textScale`, `menu` (ids in the new
  order with `visible`) → the full theme. The device redraws at once.

### FM radio
The Rondo's radio from the browser. Frequencies are tenths of MHz (1005 = 100.5).
- `GET /api/radio` → `{"headphones", "on", "starting", "frequency", "name", "text", "signal": 0..4, "stereo",
  "muted", "volume", "maxVolume", "speaker", "seeking", "scanning", "error"?: "headphones" | "chip", "band": "world" | "japan", "min",
  "max", "presets": [{"frequency", "name"}], "found": [{"frequency", "name"}]}` (`name` falls back to a
  saved or earlier heard RDS name; `found` leaves out saved stations).
- `PUT /api/radio` with any of `{"on", "frequency", "muted", "volume", "speaker", "band"}`; turning on pauses
  music. `volume` is the radio's own (MediaTek's FM stream), not the music volume.
- `POST /api/radio/seek` `{"up": bool}`; `POST /api/radio/scan` (about 20 s; `scanning` until done).
- `PUT /api/radio/presets` `{"presets": [{"frequency", "name"}]}`: the saved stations in this order, names
  up to 40 characters. Each answers with the `GET` shape.
- The page polls every 1.5 s and rebuilds only when power, station, lists or settings change; signal,
  names and mute update in place (a rebuild under the pointer swallowed clicks).

### Bluetooth
The Rondo's Bluetooth screen from the browser. The Rondo searches while the page is open: every
`GET` keeps it going, 30 s without one stops it.
- `GET /api/bluetooth` → `{"available": true, "state": "on" | "off" | "turning_on" | "turning_off", "searching": bool,
  "mine": [{"address", "name", "kind", "audio", "status": "connected" | "connecting" | "disconnected"}],
  "others": [{"address", "name", "kind", "audio", "pairing": bool}]}` (`mine`/`others` only while on).
- `PUT /api/bluetooth` `{"enabled": bool}`; `POST /api/bluetooth/search`.
- `POST /api/bluetooth/connect | disconnect | forget` `{"address": "AA:BB:…"}`; connect pairs first when the
  device isn't paired. Each answers with the `GET` shape.

### Wi-Fi
- `GET /api/wifi` → `{"enabled": true, "current": {"ssid", "signal": 0..4, "ip"} | null, "joining": "<ssid>" | null, "lastError": "auth" | null,
  "saved": [{"id": 2, "ssid": "Home"}], "nearby": [{"ssid", "signal": 0..4, "security": "open" | "wep" | "psk" | "eap", "saved": bool}]}`
- `POST /api/wifi/join` `{"ssid": "…", "password"?: "…"}` → `{"ok": true}` (connection happens in
  the background; poll `GET /api/wifi`). Joining another network may drop the dashboard's own
  connection: the page should say so first.
- `DELETE /api/wifi/saved?id=<id>` → `{"ok": true}`

### YouTube: where it comes from
- `GET /api/server` → `{"url": "https://music.example.com" | "", "tokenSet": true, "direct": bool, "directChosen": bool}`.
  `direct`: the Rondo talks to YouTube itself (chosen, or no server set up); `directChosen`: the
  choice as made.
- `PUT /api/server` with one of: `{"direct": bool}` (switch; to a server only when one is set up),
  `{"url"?: "…", "code": "123456"}` (connect with the code from the server's admin page: the Rondo
  calls its `POST /device/pair`; `403 {"error": "code"}`, `400 {"error": "unreachable"}`), or
  `{"url"?: "…", "token"?: "…"}` (an address and/or an `API_TOKEN`). Connecting chooses the server.
  → the `GET` shape (the token never echoed).
- `POST /api/server/test` → `{"ok": true, "latencyMs": 180, "ytdlp": "2026.10.29…" | "direct", "playbackOk": true | null, "signedIn": bool}`
  or `{"ok": false, "error": "unreachable" | "unauthorized" | "HTTP <n>"}`: whichever source is in use.

### YouTube account (directly to YouTube)
With a server, these answer `{"mode": "server"}`: the server's admin page signs it in.
- `GET /api/youtube/tv` → `{"mode": "direct", "signedIn": bool, "name", "handle", "pending": {"url", "code", "expiresAt"} | null,
  "error": "denied" | "expired" | … | null}`: the code sign-in (`native/direct/tv_auth.h`).
- `POST /api/youtube/tv/begin` (a new code), `POST /api/youtube/tv/cancel`,
  `POST /api/youtube/tv/signout` (deleted here and revoked at Google) → the same shape. The page
  polls `GET` while a code waits.
- `GET /api/youtube/account` → `{"mode": "direct", "signedIn": bool, "name"?, "handle"?, "expired"?, "unchecked"?, "unreachable"?}`:
  who the cookies.txt signs in as (`unchecked`: no connection to ask YouTube).
- `PUT /api/youtube/cookies` `{"cookies": "<cookies.txt text>"}` (the page sends only its
  youtube.com lines) → the account; `400` when it isn't a cookies.txt with youtube.com cookies.
  `DELETE /api/youtube/cookies` signs out. The file stays on the Y1 (0600) and is never returned.

### YouTube downloads
Videos saved to `Videos/YouTube` on the card with their subtitles (`native/shell/downloads.h`).
- `GET /api/downloads` → `{"dir": "/storage/sdcard0/Videos/YouTube",
  "jobs": [{"id", "title", "channel", "quality", "state": "waiting" | "running" | "done" | "failed", "percent", "stage", "error", "file"}],
  "files": [{"file", "id", "title", "subtitle"}]}`.
- `POST /api/downloads` `{"video": "<link or id>", "quality": 720 | 480 | 360}` → the `GET` shape;
  `400` for something that isn't a YouTube link or id, `409` when it's on the card or on its way
  already.
- `POST /api/downloads/cancel` `{"id"}` → the `GET` shape.
- `DELETE /api/downloads?file=<path>` → the `GET` shape (the video and its subtitles).
