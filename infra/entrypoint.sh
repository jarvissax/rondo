#!/bin/sh
# Keeps yt-dlp on the /data volume so its self-update survives container rebuilds.
set -eu
BIN="${YTDLP_BIN:-/data/bin/yt-dlp}"
mkdir -p "$(dirname "$BIN")" /data/secrets

# yt-dlp's standalone build for this machine: regular PCs and servers (x86-64), or 64-bit ARM
# (Raspberry Pi 4/5 on a 64-bit OS, many NAS boxes). 32-bit ARM has no standalone yt-dlp and no
# deno, which yt-dlp needs for YouTube.
case "$(uname -m)" in
  x86_64 | amd64) ASSET=yt-dlp_linux ;;
  aarch64 | arm64) ASSET=yt-dlp_linux_aarch64 ;;
  *)
    echo "[entrypoint] this machine ($(uname -m)) isn't supported: Rondo's server needs x86-64 or 64-bit ARM" >&2
    exit 1
    ;;
esac

# yt-dlp's unpacked build (ytdlp-install.sh): put in place when it's missing or still the one-file
# build, and a newer nightly fetched once a day
if [ -n "$(find "$BIN" -maxdepth 0 -mmin +1440 2>/dev/null)" ]; then
  /ytdlp-install.sh update || echo "[entrypoint] yt-dlp: the update didn't work, keeping this one" >&2
else
  /ytdlp-install.sh || echo "[entrypoint] yt-dlp: the unpacked build couldn't be installed" >&2
fi
# still nothing that runs (no network to GitHub's zip, say): the one-file build, as before
if ! "$BIN" --version >/dev/null 2>&1; then
  echo "[entrypoint] downloading yt-dlp ($ASSET)"
  rm -f "$BIN"
  curl -fsSL "https://github.com/yt-dlp/yt-dlp/releases/latest/download/$ASSET" -o "$BIN"
  chmod a+rx "$BIN"
  "$BIN" --update-to nightly || true
fi

echo "[entrypoint] yt-dlp $("$BIN" --version), deno $(deno --version | head -1), ffmpeg $(ffmpeg -version | head -1 | cut -d' ' -f3)"
exec node dist/main.js
