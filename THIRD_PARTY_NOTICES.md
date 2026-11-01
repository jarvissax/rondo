# Third-party software

Rondo is MIT-licensed (see [LICENSE](LICENSE)). It's built on the projects below, each under its
own license.

## In the Android app (and the APK)

| Project | License |
| --- | --- |
| [ExoPlayer](https://github.com/google/ExoPlayer) 2.19.1 | Apache-2.0 |
| [OkHttp](https://github.com/square/okhttp) 3.12 and Okio | Apache-2.0 |
| [AndroidX](https://developer.android.com/jetpack/androidx) (appcompat, core, recyclerview, media, multidex, lifecycle) | Apache-2.0 |
| [Kotlin](https://kotlinlang.org) standard library, kotlinx.coroutines, kotlinx.serialization | Apache-2.0 |
| [NanoHTTPD](https://github.com/NanoHttpd/nanohttpd) | BSD-3-Clause |
| [ZXing](https://github.com/zxing/zxing) (QR codes) | Apache-2.0 |
| [Roboto](https://github.com/googlefonts/roboto-3-classic) font | SIL Open Font License 1.1 (see `app/app/src/main/assets/fonts/NOTICE.txt`) |
| ISRG Root X1 and ISRG Root YR certificates ([Let's Encrypt](https://letsencrypt.org/certificates/)) | public certificates |

## In the server

| Project | License |
| --- | --- |
| [youtubei.js](https://github.com/LuanRT/YouTube.js) | MIT |
| [Fastify](https://github.com/fastify/fastify) and @fastify/rate-limit | MIT |
| [undici](https://github.com/nodejs/undici) | MIT |
| [sharp](https://github.com/lovell/sharp) (with libvips) | Apache-2.0 (libvips: LGPL-3.0) |

## In the web installer

| Project | License |
| --- | --- |
| [Tango](https://github.com/yume-chan/ya-webadb) (`@yume-chan/adb` and its WebUSB and credential packages) | MIT |
| [esbuild](https://github.com/evanw/esbuild) (build only) | MIT |

## Downloaded when the server's Docker image is built or started

These aren't part of this repository. The Docker image fetches them from their own projects.

| Project | License |
| --- | --- |
| [yt-dlp](https://github.com/yt-dlp/yt-dlp) | Unlicense |
| [Deno](https://github.com/denoland/deno) | MIT |
| [FFmpeg](https://ffmpeg.org) (Debian package) | LGPL-2.1+ / GPL-2.0+ |
| [Node.js](https://nodejs.org) | MIT |
| [Caddy](https://github.com/caddyserver/caddy) (with `compose.vps.yaml`) | Apache-2.0 |

YouTube and YouTube Music are trademarks of Google LLC; their logos are shown to identify the
services. Innioasis and Y1 are trademarks of their owner. Rondo isn't affiliated with or endorsed
by either.
