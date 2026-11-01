# Rondo dashboard

A web page for managing a Rondo device from a phone or computer on the same Wi-Fi: the offline
library, settings, theme, Wi-Fi, server connection. Served by the Y1 itself (no internet or server
needed). This page describes its API, for anyone working on the dashboard or scripting the Y1.

## How it runs on the device

- **Settings › Dashboard** starts a small HTTP server (NanoHTTPD) on port **8080** and shows the
  address (`http://<ip>:8080`), a QR code of it and a **6-digit pairing code**. The screen stays
  on while it is open (CPU and Wi-Fi must stay awake to answer); leaving the screen stops the
  server. Nothing listens otherwise (about 1–3 MB of RAM while it runs).
- The web files are in the APK: `app/app/src/main/assets/dashboard/` (served at `/`), plus the
  bundled font at `/fonts/Roboto-Regular.ttf` (from `assets/fonts/`).
- Changes apply on the device immediately (theme changes redraw the screens).

## Auth

- `POST /api/pair` `{"code": "123456"}` → `200 {"session": "<token>"}` (also `Set-Cookie:
  rondo_session=<token>; Path=/`), `403 {"error": "code"}` on a wrong code (after 5 wrong codes
  the server picks a new code).
- Every other `/api/*` call needs the session: header `X-Rondo-Session: <token>` or the cookie.
  Without it: `401 {"error": "pair"}` → the page shows the pairing screen.
- Sessions live in memory until the server stops; the pairing code changes each time it starts.
- The API token for the music server is **write-only**: it is never returned.

## API (JSON unless noted; errors are `{"error": "<short reason>"}` with 4xx/5xx)

### Status and player
- `GET /api/status` →
  ```json
  {"device": {"name": "Rondo", "version": "1.0.0", "model": "Innioasis Y1"},
   "battery": {"level": 87, "charging": false},
   "storage": {"freeBytes": 31234567890, "totalBytes": 62500000000},
   "wifi": {"ssid": "Home", "signal": 3, "ip": "192.168.1.118"},
   "player": {"title": "Günah Benim", "subtitle": "Eypio, Burak King", "art": "/api/art?path=…" ,
              "source": "device" | "youtube", "playing": true, "positionMs": 41000, "durationMs": 210000,
              "volume": 2, "maxVolume": 15} | null}
  ```
  `art` is a URL on the device for card songs, a `/api/thumb?u=` proxy URL for YouTube Music, or null.
- `POST /api/player` `{"action": "toggle" | "next" | "previous" | "seek" | "volume", "positionMs"?: n, "volume"?: n}` → the `player` object.
- `POST /api/player/play` `{"path": "/storage/sdcard0/Music/x.flac"}` → plays that song with the
  current library order as the queue → the `player` object.

### Offline library (microSD)
- `GET /api/library` →
  `{"songs": [{"path", "title", "artist", "album", "track", "durationSec", "size", "modified", "art": true}],
    "videos": [{"path", "title", "durationSec", "size", "modified"}], "scanning": false,
    "folders": ["/storage/sdcard0/Music", …]}`
- `GET /api/art?path=<file>&size=<px>` → `image/jpeg`, the embedded cover (or a frame for videos),
  scaled to about `size` px; `404` when there is none. Cacheable (`Cache-Control: max-age=86400`).
- `PUT /api/library/upload?name=<file name>&folder=<folder, default /storage/sdcard0/Music>` with
  the raw file as the body (`Content-Length` required; not multipart) → `{"path": "…"}`. Allowed
  extensions: flac mp3 m4a aac ogg opus wav mp4 m4v mkv 3gp webm lrc. An existing file of that
  name gets ` (2)` added. The library picks it up on the next scan.
- `DELETE /api/library?path=<file>` → `{"ok": true}` (only files inside the card folders above,
  only media/lrc extensions).
- `POST /api/library/rescan` → `{"ok": true}`; `GET /api/library` shows `scanning` meanwhile.

### Settings
- `GET /api/settings` →
  `{"videoQuality": null | 360 | 480 | 720, "brightness": 10..100, "screenTimeoutSec": 15|30|60|120|300|600,
    "lyricsOnOpen": false, "wheelAcceleration": "off" | "normal" | "fast"}`
- `PUT /api/settings` with any subset → the full settings.
- `"clickSound": bool`: Android's sound effects switch, which the Y1's system UI checks before its tick on each wheel step.
- `"wheelVibration": "off" | "light" | "normal" | "strong"`: a buzz on every wheel step, 15 / 25 / 40 ms plus Rockbox's fading boost; "normal" matches Rockbox's intensity 5.

### Theme
- `GET /api/theme` →
  ```json
  {"accent": "#1c5ccc", "appearance": "light" | "dark", "textScale": 1.25 | 1.4 | 1.55,
   "menu": [{"id": "music", "label": "Music", "visible": true}, {"id": "videos", …},
            {"id": "device", "label": "On this device", …}, {"id": "nowPlaying", "label": "Now Playing", …},
            {"id": "settings", "label": "Settings", "visible": true, "locked": true}],
   "presets": [{"name": "iPod Blue", "accent": "#1c5ccc"}, {"name": "Rondo Red", "accent": "#ff3b4f"}, …]}
  ```
  `accent` colors the selection bar and highlights in the iPod-style menus; the YouTube and
  YouTube Music screens keep their own dark look. `menu` order is the main menu order;
  `locked` items cannot be hidden.
- `PUT /api/theme` with any subset of `accent`, `appearance`, `textScale`, `menu` (ids in the new
  order with `visible`) → the full theme. The device redraws at once.

### FM radio
The Rondo's radio (`player/FmRadio.kt`) from the browser. Frequencies are tenths of MHz (1005 = 100.5).
- `GET /api/radio` → `{"headphones", "on", "starting", "frequency", "name", "text", "signal": 0..4, "stereo",
  "muted", "volume", "maxVolume", "speaker", "seeking", "scanning", "error"?: "headphones" | "chip", "band": "world" | "japan", "min",
  "max", "presets": [{"frequency", "name"}], "found": [{"frequency", "name"}]}` (`name` falls back to a
  saved or earlier heard RDS name; `found` leaves out saved stations).
- `PUT /api/radio` with any of `{"on", "frequency", "muted", "volume", "speaker", "band"}`; turning on pauses
  music. `volume` is the radio's own (MediaTek's FM stream, 0–13 here), not the music volume.
- `POST /api/radio/seek` `{"up": bool}`; `POST /api/radio/scan` (about 20 s; `scanning` until done).
- `PUT /api/radio/presets` `{"presets": [{"frequency", "name"}]}`: the saved stations in this order, names
  up to 40 characters. Each answers with the `GET` shape.
- The page polls every 1.5 s and rebuilds only when power, station, lists or settings change; signal,
  names and mute update in place (a rebuild under the pointer swallowed clicks).

### Bluetooth
The Rondo's Bluetooth screen from the browser; both use `BtSession` (`ui/screens/BluetoothScreens.kt`).
The Rondo searches while the page is open: every `GET` keeps it going, 30 s without one stops it.
- `GET /api/bluetooth` → `{"available": true, "state": "on" | "off" | "turning_on" | "turning_off", "searching": bool,
  "mine": [{"address", "name", "kind", "audio", "status": "connected" | "connecting" | "disconnected"}],
  "others": [{"address", "name", "kind", "audio", "pairing": bool}]}` (`mine`/`others` only while on).
- `PUT /api/bluetooth` `{"enabled": bool}`; `POST /api/bluetooth/search`.
- `POST /api/bluetooth/connect | disconnect | forget` `{"address": "AA:BB:…"}`; connect pairs first when the
  device isn't paired (a PIN request is answered with 0000). Each answers with the `GET` shape.

### Wi-Fi
- `GET /api/wifi` → `{"enabled": true, "current": {"ssid", "signal": 0..4, "ip"} | null, "joining": "<ssid>" | null, "lastError": "auth" | null,
  "saved": [{"id": 2, "ssid": "Home"}], "nearby": [{"ssid", "signal": 0..4, "security": "open" | "wep" | "psk" | "eap", "saved": bool}]}`
- `POST /api/wifi/join` `{"ssid": "…", "password"?: "…"}` → `{"ok": true}` (connection happens in
  the background; poll `GET /api/wifi`). Joining another network may drop the dashboard's own
  connection: the page should say so first.
- `DELETE /api/wifi/saved?id=<id>` → `{"ok": true}`

### Music server connection
- `GET /api/server` → `{"url": "https://music.example.com", "tokenSet": true}`
- `PUT /api/server` `{"url"?: "…", "token"?: "…"}` → same shape as GET (token never echoed).
- `POST /api/server/test` → `{"ok": true, "latencyMs": 180, "ytdlp": "2026.10.29…", "cookies": true}`
  or `{"ok": false, "error": "unreachable" | "unauthorized" | "<message>"}`.

