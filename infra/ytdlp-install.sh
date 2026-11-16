#!/bin/sh
# yt-dlp's unpacked build (one folder, not one file) for this machine, on the data volume, with
# YTDLP_BIN a link to it. The one-file build unpacks itself on every run, about 1.3 s of every
# resolve on a VPS; the unpacked one starts in about 0.65 s. yt-dlp can't update this build itself
# (-U), so this script does, from the nightly channel the server follows (updates.ts):
#
#   ytdlp-install.sh          install it when it's missing, can't run, or YTDLP_BIN is still the one-file build
#   ytdlp-install.sh update   the same, and a newer nightly when one is out
#
# Each version gets its own folder (yt-dlp-<version>) and the link switches in one step, so a resolve
# that's running keeps its files; the version before stays until the next update.
set -eu
BIN="${YTDLP_BIN:-/data/bin/yt-dlp}"
HOME_DIR="$(dirname "$BIN")"
REPO=yt-dlp/yt-dlp-nightly-builds
case "$(uname -m)" in
  x86_64 | amd64) ASSET=yt-dlp_linux.zip ;;
  aarch64 | arm64) ASSET=yt-dlp_linux_aarch64.zip ;;
  *)
    echo "[yt-dlp] this machine ($(uname -m)) isn't supported: Rondo's server needs x86-64 or 64-bit ARM" >&2
    exit 1
    ;;
esac
mkdir -p "$HOME_DIR"

installed() { [ -L "$BIN" ] && "$BIN" --version >/dev/null 2>&1; }

if installed; then
  [ "${1:-}" = update ] || exit 0
  have=$("$BIN" --version)
  want=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1 || true)
  if [ -z "$want" ] || [ "$want" = "$have" ]; then
    touch -h "$BIN"  # checked: the next check is a day from now
    echo "[yt-dlp] $have is the latest"
    exit 0
  fi
fi

tmp=$(mktemp -d "$HOME_DIR/.yt-dlp-new.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
echo "[yt-dlp] downloading $ASSET"
curl -fsSL "https://github.com/$REPO/releases/latest/download/$ASSET" -o "$tmp/yt-dlp.zip"
mkdir "$tmp/dir"
unzip -q "$tmp/yt-dlp.zip" -d "$tmp/dir"
exe=$(find "$tmp/dir" -maxdepth 1 -type f -name 'yt-dlp*' | head -1)
[ -n "$exe" ] || { echo "[yt-dlp] no program in $ASSET" >&2; exit 1; }
chmod a+rx "$exe"
version=$("$exe" --version)  # it runs on this machine
dest="$HOME_DIR/yt-dlp-$version"
previous=$(readlink "$BIN" 2>/dev/null | sed 's|/[^/]*$||' || true)
rm -rf "$dest"
mv "$tmp/dir" "$dest"
ln -sfn "$dest/$(basename "$exe")" "$BIN.new"
mv -f "$BIN.new" "$BIN"  # one rename: the old file or link is replaced at once
# what's neither this version nor the one before goes
for old in "$HOME_DIR"/yt-dlp-*; do
  if [ -d "$old" ] && [ "$old" != "$dest" ] && [ "$old" != "$previous" ]; then rm -rf "$old"; fi
done
echo "[yt-dlp] $version (unpacked)"
