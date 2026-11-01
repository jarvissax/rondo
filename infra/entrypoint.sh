#!/bin/sh
# Keeps yt-dlp on the /data volume so its self-update survives container rebuilds.
set -eu
BIN="${YTDLP_BIN:-/data/bin/yt-dlp}"
mkdir -p "$(dirname "$BIN")" /data/secrets

if [ ! -x "$BIN" ]; then
  echo "[entrypoint] downloading yt-dlp standalone binary"
  curl -fsSL https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux -o "$BIN"
  chmod a+rx "$BIN"
  "$BIN" --update-to nightly || true
elif [ -n "$(find "$BIN" -mmin +1440 2>/dev/null)" ]; then
  echo "[entrypoint] yt-dlp older than 24h, updating"
  "$BIN" -U || true
fi

echo "[entrypoint] yt-dlp $("$BIN" --version), deno $(deno --version | head -1), ffmpeg $(ffmpeg -version | head -1 | cut -d' ' -f3)"
exec node dist/main.js
