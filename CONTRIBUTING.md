# Contributing to Rondo

Thanks for taking a look. Rondo is a small project for a small music player, and right now the
most useful thing anyone can do is try it on their own Y1 and say how it went. Code, docs and
ideas are welcome too.

## Trying it and telling me how it went

If you install Rondo, I'd love to hear about it, good or bad. Things that help the most:

- **The install and updates.** Did Innioasis Updater CE install Rondo without trouble? Which
  computer? Did an update work, and which way (Settings › Software update, the dashboard, USB)?
- **What broke or felt wrong.** A screen that misbehaves, a song that won't play, a button that
  does the wrong thing.
- **How the wheel feels.** Too fast, too slow, the vibration, the click sound.
- **Your setup.** Server at home or on a VPS, with or without a YouTube account, Bluetooth
  headphones, FM radio.
- **Battery life**, if you notice how long a charge lasts while streaming or playing from the card.

Open an [issue](https://github.com/jarvissax/rondo/issues/new/choose) for anything you find.
There are short forms for problems and for ideas.

## Reporting a problem

The more of this you include, the faster it gets fixed:

- what you did, what you expected, and what happened instead,
- Rondo's version (Settings › About on the Y1),
- whether YouTube goes straight from the Y1 (Directly to YouTube) or through a server at home or on
  a VPS, or you only use the card,
- Rondo's log, if you can get it with the Y1 plugged in:

  ```bash
  adb pull /data/rondo/rondo.log rondo-log.txt
  ```

- for server problems, the server's log: `docker logs rondo 2>&1 | tail -200`.

Please don't paste your `cookies.txt`, tokens or passwords anywhere. Neither log contains them,
but have a quick look before you post one anyway. Leave out your server's address too, if you'd
rather keep it to yourself.

A screenshot of the Y1 says a lot:

```bash
adb shell 'kill -USR1 $(cat /data/rondo/rondo.pid)' && sleep 1 && adb pull /data/rondo/shot.png
```

## Suggesting a feature

Describe what you'd like to do on the Y1, not only the fix you have in mind. Some things are out
of reach on purpose or by necessity:

- **Downloads only when you ask.** Rondo streams YouTube live; the only videos it saves are the ones
  you download yourself (YouTube › Download). Nothing is cached behind your back.
- **Wheel and five buttons only.** The Y1 has no touchscreen, so every screen has to work with
  the wheel.
- **Android 4.2.** Anything that needs a newer Android can't run on the Y1.

## Working on the code

Start with [Building Rondo](docs/building.md). It explains where everything lives, how to build
and run each part, and the mock servers for working on the dashboard and admin page without a Y1.

The ground rules:

- **Android 4.2's kernel and libraries.** Rondo 2.0 is a native program (C++, OpenGL ES 2) on the
  Y1's Android 4.2 system; everything it uses has to build with the NDK for API 17.
- **No media caching.** Audio and video stream as they're requested; only a video the user
  downloads is saved (on the Y1's card). Caching metadata (stream URLs, index ranges, thumbnails) is fine.
- **Two ways to YouTube.** Directly from the Y1, or through the user's own server, where no request
  from the Y1 goes to Google. Nothing outside the stream client assumes one or the other.
- **Two YouTube libraries, one job each.** youtubei.js for everything you see (home pages, search,
  playlists, up next). yt-dlp only to turn a video ID into stream URLs.
- **Test app changes on a real Y1.** There's no emulator for the wheel. For the server, run
  `npm test` and `npm run typecheck` in `server/`. For the update page, `npm run build` in
  `web-updater/`.
- **Match the code around you.** Kotlin with classic Views in the app, TypeScript on the server.
  Comments say why, not what. Text the user sees is short, plain and in American English.
- **Never commit secrets:** `cookies.txt`, tokens, `.env` files or signing keys.

### Pull requests

- Keep each one to one change.
- Say what changes for someone using Rondo, and how you tested it. For app changes, a screenshot
  from the Y1 helps.
- The checks on GitHub (the app build, the server tests, the installer build) have to pass.
- Write commit messages as a short summary of the change, with details underneath if they're
  needed.

## Docs

Fixes to the docs are as welcome as code. Keep them plain and short, written to the reader
("you"), in American English. If you followed a guide and got stuck, telling me where is already
a big help.

## Be kind

Everyone here is doing this in their spare time. Be patient and friendly, in issues and in reviews.

## License

By contributing, you agree that your contribution is released under the project's
[MIT License](LICENSE).
