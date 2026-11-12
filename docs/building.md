# Building Rondo

This page is for building Rondo from source and working on it. To just use it, see
[Installing Rondo](install.md).

## What's where

```
native/             Rondo itself: the program the firmware runs (C++17, NDK r23c, Android API 17)
  apps/rondo/       the program's start, its main loop and the processes it starts
  shell/            every screen, the dashboard's web server, downloads, subtitles, updates
  ui/               the GLES2 UI engine: drawing, text (FreeType, HarfBuzz), lists, motion, images
  player/           rondo-player: playback with FFmpeg and OpenSL ES, the equalizer and the rest
                    of the sound, video, FM radio, the wheel's click
  services/         rondo-core: Wi-Fi, Bluetooth, the clock and the time zone
  direct/           the YouTube client (direct mode), answering the server's API on the Y1 itself
  core/             the library, tags, playlists, smart playlists, Organize, settings, the update
                    bundle, the server client, the version
  platform/y1/      the Y1's hardware: display, input, power, vibration, USB drive, video surfaces
  tests/            the first hardware tests (s1-display … s7-power), and host/ for the Mac
  tools/            vsync measurement, input injection
  assets/           the fonts, the root certificates, the glyph tables, and dashboard/ (the
                    dashboard's page: index.html, app.js, app.css)
firmware/           build.sh, which makes the firmware (rom.zip) from the community's base ROM;
                    rondo/start, Rondo's start script; zoneinfo/, the time zone data
server/             the optional Rondo server (Node 22, TypeScript, Fastify)
  src/              routes, YouTube browsing, stream resolving, the range proxy, the admin API
  admin/            the admin page (plain HTML, CSS and JS)
  test/             unit tests (vitest)
infra/              Dockerfile, compose files, Caddyfile
web-updater/        the update page (WebUSB)
tools/
  native/           build.sh, deps.sh, sysroot.sh, bundle.sh, direct-host.sh
  firmware/         BootImage.java (the boot image's init.rc and panel), zoneinfo.mjs
  parity/, perf/    screenshots and timing runs on a Y1 over adb
  dashboard-mock/, admin-mock/, bootscreen/, set-version.sh
docs/               guides; device/ holds notes on the Y1, dev/ the API references
```

## Rondo (native/)

### What you need

| | |
| --- | --- |
| A Mac | the scripts use the NDK's macOS tools (`darwin-x86_64`) |
| Android SDK | with **NDK 23.2.8568313** (r23c, the last that builds for Android 4.2) and **CMake 3.22.1**, from Android Studio's SDK Manager. The scripts look in `~/Library/Android/sdk`; set `ANDROID_SDK` if yours is elsewhere. |
| A Y1 on adb | running Rondo's firmware, once: `tools/native/sysroot.sh` pulls its `/system/lib` |
| Node 22 | for signing the bundle |
| JDK 17 and e2fsprogs | for the firmware (below) |

### Build

```bash
tools/native/deps.sh       # once: FFmpeg, curl, OpenSSL, FreeType, HarfBuzz, SQLite and the rest
tools/native/build.sh      # everything into native/build/y1 (build.sh rondo: just Rondo)
```

`deps.sh` downloads each library from its own project, checks it against
`tools/native/deps.lock` and builds it for the Y1 into `native/third_party/` (not committed).
`build.sh` runs `sysroot.sh` first if `native/sysroot/` is missing: the Y1's own libraries
(SurfaceFlinger's client, binder, EGL, OpenSL ES, the media libraries), so Rondo links against the
exact ones it runs with, and Android 4.2.2's headers for them, fetched from AOSP. Neither is
committed.

The YouTube client also builds for the Mac, which is quicker for trying a change:

```bash
tools/native/direct-host.sh && native/build/host/dcli '/music/search?q=rondo&type=song'
```

The tests in `native/tests/host/` run on the Mac too; each has its build line at the top.

### Rules of the road

- **Android 4.2 (API 17), armeabi-v7a.** Rondo runs on the firmware's own Android 4.2.2
  libraries, so it can use nothing newer; NDK r23c is the last NDK that targets it. Everything
  else it needs is built in (`deps.sh`).
- **No Java.** Rondo draws straight to the framebuffer with GLES2 (no SurfaceFlinger, no window
  manager), reads the wheel from `/dev/input`, and does what Android's services did itself.
  [docs/device/native.md](device/native.md) has what that takes.
- **Three processes:** `rondo` draws and runs the screens; it starts `rondo-player` (sound and
  video) and `rondo-core` (Wi-Fi, Bluetooth, the clock, and the YouTube client's local server on
  `127.0.0.1:47811`). Image decoding runs in a helper process of its own, so the drawing process's
  memory stays quiet ([why](device/native.md#keeping-frames-on-time)).
- **Two ways to YouTube, one API.** Screens ask the same paths either way: the user's Rondo
  server, or the YouTube client in `native/direct/`, which answers the server's API on the Y1.
  The player takes a song's streams in order of preference, so it doesn't care which.
- **TLS:** curl with OpenSSL, trusting the roots it needs (ISRG for Let's Encrypt servers,
  Google Trust Services for YouTube), since Android 4.2's own list is out of date.
- **`-O2` explicitly.** With CMake 3.21 or newer, NDK r23's toolchain gives Release no `-O` at
  all; `build.sh` and `deps.sh` set it.
- **Screen copy is literal English** in the code, in American spelling.

GitHub Actions runs the server tests and builds the update page on every push (`.github/workflows/ci.yml`); Rondo itself is built on a Mac, because it links against
libraries pulled from a Y1.

## The firmware

`firmware/build.sh` turns the y1-community's [Android 4.2 base ROM](https://github.com/y1-community/y1-ata-rom)
(Type A) into the Rondo firmware, with the Rondo bundle you give it:

```bash
firmware/build.sh native/build/bundle/rondo-<version>.rondo    # writes firmware/out/rom.zip
```

It needs curl, unzip, zip, JDK 17 and e2fsprogs (`brew install e2fsprogs` on macOS, `apt install
e2fsprogs` on Debian and Ubuntu). It downloads the base ROM once (checking its SHA-256), then edits
the system and data images in place with debugfs, so nothing is mounted and no root is needed:

- everything in `/system/app` goes: Rockbox, the Innioasis player, SystemUI, Settings, the
  keyboard and every other Android app; Rockbox's button layout and switching scripts go, and the
  data image is emptied;
- the stock firmware's root backdoors go: zxly's `aggdll` and what it leaves (`wifi_init`,
  `wifi_check`, `net_server`, `mp`, a setuid `am`), and SPWall (`libsp.so`); Android's own `am`
  script is put back ([docs/device](device/README.md#the-rondo-firmware));
- the bundle's files go in as `/system/rondo`: the binary, FFmpeg's libraries, the fonts, the
  dashboard's page and the start script;
- `tools/firmware/BootImage.java` edits the boot image: in `init.rc`, zygote (Android's Java
  layer) is disabled, the backdoors' `aggsrv` and `spwall` services go, and a `rondo` service runs
  `/system/rondo/start`; the kernel's panel driver runs the screen at 60 Hz;
- Rondo's boot logo goes in (`tools/bootscreen/`), with no boot animation: the logo shows until
  Rondo's first frame;
- current time zone rules (`firmware/zoneinfo`, from IANA's data) replace the base ROM's from 2012;
- `build.prop` gets the build name `Rondo <version>` and the firmware number
  (`ro.rondo.firmware`, `kFirmwareNeeded` in `native/core/version.h`);
- `fw_package.ini` and `updater.png` tell Innioasis Updater CE what it's installing.

It checks both file systems afterwards and stops if anything is off. Install the result with
Innioasis Updater CE: **Settings › Install from a file…**. The Updater keeps an unpacked copy of
each package and reuses it for a file of the same name, so give each build its own name (or delete
the copy, see [docs/device](device/README.md#software)). It also deletes the zip once it has
unpacked it, so install a copy of a build you mean to publish.

At boot, `/system/rondo/start` waits for SurfaceFlinger to set the panel up and then stops it,
waits for mediaserver, has vold mount the card and the internal storage, and runs Rondo: the
update named in `/data/rondo/current`, else the firmware's own copy. It runs it again whenever it
ends (an update put in place, or a crash: back in about a second), and three quick ends in a row
set an update aside. Rondo's log is `/data/rondo/rondo.log`.

## The update bundle

A Rondo update is one file, `rondo-<version>.rondo`: `RONDOB1\n`, a 64-byte Ed25519 signature
over the SHA-256 of what follows, then a `.tar.gz` of the files `/system/rondo` holds, with
`VERSION` (`native/core/bundle.h`). `tools/native/bundle.sh` builds Rondo, then makes and signs
one:

```bash
RONDO_BUNDLE_KEY=/path/to/bundle-ed25519.pem tools/native/bundle.sh
# native/build/bundle/rondo-<version>.rondo
```

The key is an Ed25519 key in PEM, kept outside the repo; its public half is `kPublicKey` in
`native/core/bundle.h`, and a Rondo installs only bundles signed with it. For builds of your own,
make a key (`openssl genpkey -algorithm ed25519 -out bundle-ed25519.pem`), put its public half in
`bundle.h`, and build a firmware with your bundle: that firmware then takes your bundles as
updates.

To try a bundle on a Y1 running a firmware that takes it, install it the way the update page does:

```bash
adb push native/build/bundle/rondo-<version>.rondo /data/rondo/update.rondo
adb shell 'LD_LIBRARY_PATH=/system/rondo/lib /system/rondo/rondo --install /data/rondo/update.rondo /data/rondo'
adb shell 'kill $(cat /data/rondo/rondo.pid)'     # the start script runs the new one at once
```

`--install` checks the signature, unpacks the bundle into `/data/rondo/<version>` and names it in
`/data/rondo/current`. adb is on in the firmware; when the Y1 offers the USB drive, choose
**Not now**.

## The server

```bash
cd server
npm ci
npm test            # unit tests
npm run typecheck
```

To run it locally, put the settings in `server/.env` (it's gitignored):

```bash
YTDLP_BIN=/path/to/yt-dlp
CONFIG_FILE=./data/config.json
COOKIES_FILE=./data/secrets/cookies.txt
TV_TOKENS_FILE=./data/secrets/tv-oauth.json
```

Then `npm run dev` starts it on port 3000 and reloads on changes. Its log prints the setup code
for `http://localhost:3000/admin`. yt-dlp also needs [deno](https://deno.com) on the PATH for
YouTube's challenges. The Docker image (`infra/Dockerfile`) brings yt-dlp, deno and ffmpeg along.

To run your own build in Docker instead of the published image, add the build override to either
compose file: `docker compose -f infra/compose.home.yaml -f infra/compose.build.yaml up -d --build`.
It also leaves the updater off.

**The updater** (`src/updater/`, built into its own small image by `infra/updater.Dockerfile`)
runs next to the server with Docker's socket. When the admin page's **Update** asks (through
`src/serverUpdate.ts`, with a token the two share on a small volume), it pulls the image the
`rondo` container runs, and if it's new, recreates the container on it the way `docker compose`
would: same name, volumes, ports, networks and environment, with only the image's own defaults
taken from the new image. The old container waits, stopped, until the new one answers
`/healthz`, and comes back if it doesn't. It speaks Docker's API from 1.40 up (Docker 29 refuses
older clients) and touches no other container. `test/updater.test.ts` covers it with a pretend
Docker.

Two rules the server keeps:

- **youtubei.js for browsing, yt-dlp for streams.** Everything the user sees (home pages, search,
  playlists, up next) comes from youtubei.js. yt-dlp only turns a video ID into stream URLs.
- **No media on disk.** Audio and video are proxied as they're requested. Caching metadata (stream
  URLs, index ranges, thumbnails) is fine; caching media isn't. The one exception is on the Y1, not
  the server: a video the user downloads (YouTube › Download) is saved to the Y1's card, from the same
  /s/ streams, joined into one .mp4 there (`native/shell/downloads.cpp`).

The API is described in [docs/dev/server-api.md](dev/server-api.md).

## Working on the dashboard or admin page

Both pages are plain HTML, CSS and JavaScript, and each has a mock server with made-up data, so
you can work on them in a desktop browser without a Y1 or a real server:

```bash
node tools/dashboard-mock/server.mjs     # http://localhost:8090, prints the pairing code
node tools/admin-mock/server.mjs         # http://localhost:8095/admin, prints the password
```

Each mock lists its options (empty card, slow device, first run, signed out and so on) at the top
of its file. The dashboard's files are in `native/assets/dashboard/`; Rondo serves them
from its bundle (`bundle.sh` copies them in), so on a real Y1 a change needs a new bundle. The
admin page is in `server/admin/`. The dashboard's API is in
[docs/dev/dashboard-api.md](dev/dashboard-api.md).

## The update page

`web-updater/` is the page at jarvissax.github.io/rondo: plain HTML and CSS, plus a script
bundled with esbuild. It talks to the Y1 with [Tango](https://github.com/yume-chan/ya-webadb)
over WebUSB, checks that the Y1 runs the Rondo firmware (and which firmware number), copies the
newest `rondo.rondo` to `/data/rondo/update.rondo`, has Rondo install it (`rondo --install`) and
restarts Rondo (`src/updater.js`). On a 1.0.x firmware it installs `rondo.apk` with
`pm install -r` instead.

```bash
cd web-updater
npm ci
npm run dev          # http://localhost:8000; choose a bundle with "Use a file"
```

Run `adb kill-server` first; the browser can't claim the Y1 while adb holds it. One thing to know
about Android 4.2: its adb only has the old `shell:` service, not the `exec:` one Tango's
subprocess helpers use (that arrived in Android 5), so `updater.js` opens `shell:` sockets itself.

The page is published by `.github/workflows/release.yml` whenever a release is published, with
that release's bundle next to it as `rondo.rondo` and `latest.json` (version, file, size, bundle,
firmware number, release notes, publication time). Rondo's Software update and the dashboard's
**Install** button download updates from there too, because GitHub Pages lets any page fetch its
files and GitHub's release downloads don't. In the repository's settings, Pages must be set to
**GitHub Actions**.

## Updates

Three ways in, one way of installing: `bundle::install` (`native/core/bundle.h`) checks the
bundle's signature, unpacks it into `/data/rondo/<version>` and names it in `/data/rondo/current`;
Rondo then restarts, and `/system/rondo/start` runs the new one, with every setting kept.

- **Settings › System › Software update** asks GitHub, with or without a Rondo server: the YouTube
  client's local server answers `GET /update?firmware=<n>` and `GET /update/rondo.rondo` itself,
  from the update page's `latest.json` and `rondo.rondo` (`native/direct/routes.cpp`).
- **The dashboard**: the browser downloads `latest.json` and `rondo.rondo` from the update page's
  site and sends the bundle, or a file, to `PUT /api/update`.
- **The update page** copies it over USB and runs `rondo --install` itself.

All three learn of a release from `latest.json`, which appears only once the release's `rom.zip`
is built, so they agree with each other and with Innioasis Updater CE. An update that ends three
times in a row within 30 seconds of starting is set aside (`/data/rondo/current.failed`) and the
firmware's own Rondo runs.

A bundle replaces only what's in `/system/rondo`, so it can't change the firmware around Rondo.
A release that does raises the **firmware number**, `kFirmwareNeeded` in `native/core/version.h`
(2 for the 2.0 firmware): `firmware/build.sh` writes it into the firmware as
`ro.rondo.firmware`, and `latest.json` names it. A Y1 whose firmware is older than a release needs
is told to install that release with Innioasis Updater CE instead (with Keep Data, where the
Updater offers it, to keep its settings): Software update and the dashboard show no
Install button, and the update page won't start.

Rondo 1.0.x (the Android app 2.0 replaced; its source is at the
[v1.0.2](https://github.com/jarvissax/rondo/tree/v1.0.2) tag) installs updates as APKs with
Android's `pm install -r`, asks its server for them (`GET /update?firmware=1`), and runs on
firmware number 1. The server tells a 1.0.x Rondo of a 2.0 release as one that needs a new
firmware, so it points its owner to Innioasis Updater CE.

## Making a release

Rondo on the Y1 and the server are released apart, each with its own version and tags:
`v<version>` for Rondo (the firmware, the update bundle, the dashboard it serves and the update
page) and `server-v<version>` for the server and its updater. Most changes are the Y1's, and a
Rondo release no longer means a server update. When a change needs both sides, raise the
device–server protocol's number in both, `API_VERSION` in `server/src/version.ts` and
`kServerApiNeeded` in `native/core/version.h`, and release the server first: a Rondo that needs a
newer server says so (**Server needs an update**).

### Rondo (the Y1)

1. Set its version: `tools/set-version.sh device 3.0.2` (`native/core/version.h`, the update page's
   `package.json` and `package-lock.json`). If every Y1 has to get a change to the firmware around
   Rondo (`firmware/build.sh`, the boot image, anything outside `/system/rondo`), raise
   `kFirmwareNeeded` in `native/core/version.h` too, and the major version with it: that release
   installs only with Innioasis Updater CE (3.0.0 did, for the firmware without the stock firmware's
   root backdoors).
2. Build and sign the bundle with the bundle key, kept outside the repo:

   ```bash
   RONDO_BUNDLE_KEY=/path/to/bundle-ed25519.pem tools/native/bundle.sh
   ```

3. Try it on a Y1: install `native/build/bundle/rondo-<version>.rondo` over the previous release
   (the update page's **Use a file**, the dashboard's **Update from a file**, or adb as above) to
   check updating. When the firmware changes, also build it
   (`firmware/build.sh native/build/bundle/rondo-<version>.rondo`) and install `firmware/out/rom.zip`
   with Innioasis Updater CE, under a name of its own.
4. Add the release to `CHANGELOG.md`, in words a Y1 owner understands. Commit, tag `v<version>`,
   push both, and create a GitHub release for the tag as a draft, marked latest, with one asset:
   **`rondo-<version>.rondo`**. Say in the notes that it's for the **Y1, Type A**, and whether it
   needs Innioasis Updater CE (a new firmware number). Then publish it.
5. Publishing starts `.github/workflows/release.yml`, which checks that the tag, the device's
   versions and the bundle agree (its `VERSION`, and its signature against the public key in
   `native/core/bundle.h`), builds the firmware from that bundle and attaches **`rom.zip`** (the
   name Innioasis Updater CE reads as Y1 Type A) and **`updater.png`** (its picture for the
   release), then republishes the update page with the new `rondo.rondo` and `latest.json`. It
   takes about 15 minutes; if it fails, fix the cause and run it again from the Actions tab (it
   redoes the latest release).

### The server

1. Set its version: `tools/set-version.sh server 3.1.0` (`server/package.json` and
   `package-lock.json`).
2. Add the release to `server/CHANGELOG.md`. Commit, tag `server-v<version>`, push both, and
   publish a GitHub release for the tag with no assets, **not** marked latest (that stays Rondo's,
   and Innioasis Updater CE lists only releases with a `rom.zip`).
3. Publishing starts `.github/workflows/server-release.yml`, which checks that the tag and the
   server's version agree, runs the server's tests, and builds the server and updater images for
   x86-64 and 64-bit ARM as `ghcr.io/jarvissax/rondo-server` and `rondo-updater` (`<version>` and
   `latest`). The first time the images are published, set both packages to **Public** on GitHub
   (your profile › Packages › the package › Package settings), or servers can't download them.
4. Update your own server with **Update** on its admin page, which hears of the release from GitHub
   within 6 hours (`server/src/updates.ts`).

Innioasis Updater CE lists every release of this repository that has a `rom.zip`, so a new one
shows up there by itself (the Updater keeps its list for up to a day). Every bundle has to be
signed with the same key, or no Rondo will install it. Keep the key backed up somewhere safe.

**Rondo 1.0.x releases** carried `rondo-<version>.apk` instead, built from the Android app at the
[v1.0.2](https://github.com/jarvissax/rondo/tree/v1.0.2) tag; the workflow now takes only bundles.

## Extras

### Boot screens

`tools/bootscreen/BootScreen.java` draws Rondo's boot logo (`logo.png`, and `logo.bin`, the Y1's
`logo` partition with Rondo's logo in place of the first and last images) and a boot animation,
a spinning click wheel, as Android's `bootanimation.zip`:

```bash
java tools/bootscreen/BootScreen.java out/ [original-logo.bin]
```

The firmware build puts the logo in, and the firmware shows it until Rondo's first frame. The boot
animation was the 1.0.x firmware's; no firmware uses it now.

### Rondo 1.0

Rondo 1.0, the Android app (Kotlin, classic Views, ExoPlayer 2) that 2.0 replaced screen for
screen, is at the [v1.0.2](https://github.com/jarvissax/rondo/tree/v1.0.2) tag. It only runs on a
1.0.x firmware. [docs/device/README.md](device/README.md) has what 1.0 found out about Android
4.2 on the Y1.
