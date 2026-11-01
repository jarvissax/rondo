# Building Rondo

This page is for building Rondo from source and working on it. To just use it, see
[Installing Rondo](install.md).

## What's where

```
app/                Android app (Kotlin, classic Views)
  app/src/main/java/com/jarvissax/rondo/
    ui/             screens, the row list, icons, the theme
    player/         playback service, ExoPlayer setup, FM radio, lyrics
    input/          wheel and button handling, vibration
    local/          the music and videos on the card
    net/            the server API, TLS for Android 4.2
    dashboard/      the dashboard's web server (the page itself is in assets/dashboard/)
server/             Rondo server (Node 22, TypeScript, Fastify)
  src/              routes, YouTube browsing, stream resolving, the range proxy, the admin API
  admin/            the admin page (plain HTML, CSS and JS)
  test/             unit tests (vitest)
infra/              Dockerfile, compose files, Caddyfile
tools/              install script, dashboard and admin mocks, boot screen generator
docs/               guides; device/ holds notes on the Y1, dev/ the API references
```

## The app

### What you need

| | |
| --- | --- |
| JDK | **17**. Newer JDKs (like the one bundled with recent Android Studio) can't run Gradle 8.9. |
| Android SDK | platform 34 and build tools 34, which Gradle downloads on the first build |
| A Y1 | there's no emulator for a click wheel |

Point Gradle at the SDK with `app/local.properties` (Android Studio writes it for you):

```properties
sdk.dir=/path/to/Android/sdk
```

### Build and run

```bash
cd app
./gradlew :app:assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Debug builds are signed with your machine's debug key, and releases with the release key. Android
won't install one over the other, so switch with `adb uninstall com.jarvissax.rondo` first. That
erases Rondo's settings.

Rondo logs under the tag `Y1`:

```bash
adb logcat -v time -s Y1
```

The Y1's log buffer is small, and ExoPlayer pushes lines out within seconds, so stream it to a
file while you test.

### Rules of the road

The Y1 runs Android 4.2 (API 17), which decides a lot:

- **minSdk 17.** Anything that needs API 21 or later is out: Jetpack Compose, Media3, OkHttp 4,
  Coil, recent Room. The app uses classic Views, **ExoPlayer 2.19.1** (the last ExoPlayer 2) and
  **OkHttp 3.12.13** (the last line that runs below API 21). `androidx.media` stays at 1.6.0,
  because 1.7.0 needs API 19.
- **TLS:** Android 4.2 has TLS 1.2 switched off and doesn't trust Let's Encrypt. `net/Tls.kt`
  turns it on and bundles the ISRG roots.
- **The screen** is 480×360 and reports mdpi, but it's dense (about 250 ppi). Sizes go through
  `dp()`, which scales by the text-size setting.
- **The GPU renderer** on 4.2 has quirks: shapes drawn under a scale come out blurry, and
  `TransitionDrawable` backgrounds stall. Comments in the code point these out where they matter.

[docs/device/README.md](device/README.md) collects what's known about the Y1 itself: the wheel's
key events, the FM chip, the audio paths and the video decoder's timestamp bug.

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

Two rules the server keeps:

- **youtubei.js for browsing, yt-dlp for streams.** Everything the user sees (home pages, search,
  playlists, up next) comes from youtubei.js. yt-dlp only turns a video ID into stream URLs.
- **No media on disk.** Audio and video are proxied as they're requested. Caching metadata (stream
  URLs, index ranges, thumbnails) is fine; caching media isn't.

The API is described in [docs/dev/server-api.md](dev/server-api.md).

## Working on the dashboard or admin page

Both pages are plain HTML, CSS and JavaScript, and each has a mock server with made-up data, so
you can work on them in a desktop browser without a Y1 or a real server:

```bash
node tools/dashboard-mock/server.mjs     # http://localhost:8090, prints the pairing code
node tools/admin-mock/server.mjs         # http://localhost:8095/admin, prints the password
```

Each mock lists its options (empty card, slow device, first run, signed out and so on) at the top
of its file. The dashboard's files are in `app/app/src/main/assets/dashboard/`; on a real Y1 you
need to rebuild the app to see changes. The admin page is in `server/admin/`.

## Making a release

1. Raise `versionCode` and `versionName` in `app/app/build.gradle.kts`, and `version` in
   `server/package.json`.
2. Build the release APK with the signing key. `RONDO_SIGNING` points at a `keystore.properties`
   kept outside the repo (with `storeFile`, `storePassword`, `keyAlias` and `keyPassword`):

   ```bash
   cd app
   RONDO_SIGNING=/path/to/keystore.properties ./gradlew :app:assembleRelease
   ```

3. Check it, then attach `app/build/outputs/apk/release/app-release.apk` to a GitHub release as
   `rondo-<version>.apk`:

   ```bash
   apksigner verify --min-sdk-version 17 app/build/outputs/apk/release/app-release.apk
   ```

Every update has to be signed with the same key, or it won't install over the previous version.
Keep the key backed up somewhere safe.

GitHub Actions builds the app and runs the server tests on every push (`.github/workflows/ci.yml`).

## Extras

### Boot animation

`tools/bootscreen/BootScreen.java` draws Rondo's boot animation, a spinning click wheel, as
Android's `bootanimation.zip`:

```bash
java tools/bootscreen/BootScreen.java out/
adb push out/bootanimation.zip /data/local/bootanimation.zip
```

Android plays `/data/local/bootanimation.zip` in place of the system's own. Nothing on the system
partition changes, and `adb shell rm /data/local/bootanimation.zip` brings the old one back.
