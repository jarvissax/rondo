#!/usr/bin/env bash
# Builds the Rondo firmware for the Innioasis Y1, Type A (rom.zip, for Innioasis Updater).
#
#   firmware/build.sh path/to/rondo-X.rondo [firmware/out/rom.zip]
#
# Rondo (a bundle from tools/native/bundle.sh) runs without Android's Java layer: no app is left in
# /system/app, Rondo goes in as /system/rondo, and the boot image's init.rc starts it instead of zygote
# (tools/firmware/BootImage.java); SurfaceFlinger only sets the panel up, then firmware/rondo/start stops
# it. The boot logo shows until Rondo's first frame.
#
# It starts from the community's base ROM (stock Android 4.2.2 with the Y1's radios enabled) and
# takes out everything that isn't Rondo or Android's own background services: Rockbox, the stock
# Innioasis player, the Rockbox/stock switching scripts, every Android app, and the stock
# firmware's root backdoors (zxly's aggdll and wifi_init, SPWall). Rondo goes in as
# /system/rondo, so the Y1 boots straight into it. The images are edited in place with debugfs, so no mounting or root is needed.
#
# Needs curl, unzip, zip, java (17+) and e2fsprogs (debugfs, e2fsck):
#   macOS: brew install e2fsprogs        Debian/Ubuntu: sudo apt install e2fsprogs
set -euo pipefail

BUNDLE="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${2:-$HERE/out/rom.zip}"
# the build name's version and this firmware's number, the bundle's own (native/core/version.h):
# updates over Wi-Fi, the dashboard or the update page install only on a firmware at least as new
# as the release needs (kFirmwareNeeded)
RONDO_VERSION="$(sed -n 's/.*kRondoVersion = "\(.*\)".*/\1/p' "$ROOT/native/core/version.h")"
FIRMWARE_LEVEL="$(sed -n 's/.*kFirmwareNeeded = \([0-9][0-9]*\).*/\1/p' "$ROOT/native/core/version.h")"
WORK="$HERE/work"

BASE_URL="https://github.com/y1-community/y1-ata-rom/releases/download/0.1/rom.zip"
BASE_SHA256="d3b79ca460a3d3ec2c41ba57893127a0f9a90278e374dd33cdb5de5bb46e367e"

# What the base ROM inherits from Innioasis's firmware and runs as root, none of it needed
# (docs/device/README.md, "No root backdoors"): zxly's agent aggdll (a hidden su, a
# silent app installer, setuid root) and the copy of itself it keeps as net_server; wifi_init, which it
# drops, and which fetches a file over plain HTTP and runs it as root, with wifi_check, its download
# tool, and AndroidBoot.ttf, its state file; mp, aggdll's hidden copy of pm; SPWall (libsp.so), a
# service that runs any command as root; and the paths the same family writes its downloads and
# helpers to. native/services/base_rom.cpp removes the same list from firmwares already flashed.
ROOT_DAEMONS=(
  /bin/aggdll /bin/net_server /bin/wifi_init /bin/wifi_check /bin/libsp.so /bin/mp /fonts/AndroidBoot.ttf
  /bin/wifi_looper /bin/wifi_pm /bin/pm_temp /bin/am_temp /bin/su_zxly
  /lib/libsu2update.so /lib/libsu2update.so.st /lib/libkey.so
)
say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

[[ -n "$BUNDLE" && -f "$BUNDLE" && "$BUNDLE" == *.rondo ]] || fail "Usage: firmware/build.sh path/to/rondo-X.rondo [out.zip]"
[[ -n "$RONDO_VERSION" && -n "$FIRMWARE_LEVEL" ]] || fail "No kRondoVersion or kFirmwareNeeded in native/core/version.h"
for p in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
  [[ -x "$p/debugfs" ]] && { PATH="$p:$PATH"; break; }
done
# a real JDK: JAVA_HOME, else Homebrew's openjdk@17 (macOS's /usr/bin/java is only a placeholder)
for j in "${JAVA_HOME:-}" /opt/homebrew/opt/openjdk@17 /usr/local/opt/openjdk@17; do
  [[ -n "$j" && -x "$j/bin/java" ]] && { PATH="$j/bin:$PATH"; break; }
done
java -version >/dev/null 2>&1 || fail "Missing a Java runtime (JDK 17+)"
for c in curl unzip zip debugfs e2fsck shasum; do
  command -v "$c" >/dev/null || fail "Missing $c (see the top of this script)"
done

# debugfs prints its banner on stderr; commands go in through a file, one per line
dfs() { local img="$1"; shift; printf '%s\n' "$@" > "$WORK/cmds"; debugfs -w -f "$WORK/cmds" "$img" >/dev/null 2>"$WORK/debugfs.log" || { cat "$WORK/debugfs.log" >&2; fail "debugfs failed on $img"; }; }
exists() { debugfs -R "stat $2" "$1" 2>/dev/null | grep -q "^Inode:"; }
names() { debugfs -R "ls -p $2" "$1" 2>/dev/null | awk -F/ '$6 != "" && $6 != "." && $6 != ".." {print $6}'; }
# copy a file into an image, owned by root, mode 0644
put() { local img="$1" src="$2" dst="$3"; local dir="${dst%/*}" name="${dst##*/}"
  exists "$img" "$dst" && dfs "$img" "rm $dst"
  dfs "$img" "cd ${dir:-/}" "write $src $name" "sif $name uid 0" "sif $name gid 0" "sif $name mode 0100644"; }

say "1/7  Base ROM"
mkdir -p "$WORK" "$(dirname "$OUT")"
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"  # zip runs from inside the ROM
if [[ ! -f "$WORK/base-rom.zip" ]] || [[ "$(shasum -a 256 "$WORK/base-rom.zip" | cut -d' ' -f1)" != "$BASE_SHA256" ]]; then
  curl -fL --retry 8 --retry-delay 3 --retry-all-errors -o "$WORK/base-rom.zip" "$BASE_URL"
fi
[[ "$(shasum -a 256 "$WORK/base-rom.zip" | cut -d' ' -f1)" == "$BASE_SHA256" ]] || fail "The base ROM doesn't match the expected checksum"
rm -rf "$WORK/rom" && mkdir -p "$WORK/rom"
unzip -q "$WORK/base-rom.zip" -d "$WORK/rom"
SYS="$WORK/rom/system.img"
DATA="$WORK/rom/userdata.img"
echo "Base ROM ready."

say "2/7  Taking out everything that isn't Rondo"
removed=()
for f in $(names "$SYS" /app); do
  dfs "$SYS" "rm /app/$f"
  removed+=("$f")
done
echo "Removed from /system/app: $(printf '%s\n' "${removed[@]}" | sed 's/\.[^.]*$//' | sort -u | tr '\n' ' ')"
# Rockbox's library, its key layout and the Rockbox/stock switching script
for f in /lib/librockbox.so /usr/keylayout/Rockbox.kl /etc/init.d/99Y1ButtonScript; do
  exists "$SYS" "$f" && dfs "$SYS" "rm $f" && echo "Removed $f"
done
# the stock firmware's root backdoors (ROOT_DAEMONS above)
for f in "${ROOT_DAEMONS[@]}"; do
  exists "$SYS" "$f" && dfs "$SYS" "rm $f" && echo "Removed /system$f"
done
# aggdll put a setuid-root program of its own in am's place and kept Android's am script as am2: the
# script goes back. pm is Android's own, made writable by anyone: both get Android's owner and mode.
if exists "$SYS" /bin/am2; then
  debugfs -R "dump /bin/am2 $WORK/am" "$SYS" 2>/dev/null
  grep -q 'exec app_process $base/bin com.android.commands.am.Am' "$WORK/am" || fail "/system/bin/am2 isn't Android's am script"
  put "$SYS" "$WORK/am" /bin/am
  dfs "$SYS" "rm /bin/am2"
  echo "Removed /system/bin/am2: Android's am script is /system/bin/am again."
fi
for f in /bin/am /bin/pm; do dfs "$SYS" "sif $f uid 0" "sif $f gid 2000" "sif $f mode 0100755"; done
# the stock button layout for every input device (the base ships Rockbox's as Generic.kl)
debugfs -R "dump /usr/keylayout/Stock.kl $WORK/Stock.kl" "$SYS" 2>/dev/null
put "$SYS" "$WORK/Stock.kl" /usr/keylayout/Generic.kl
echo "Generic.kl is now the stock layout."
# current time zone rules: the base ships tzdata 2012h (Russia, Turkey, Mexico and more have changed
# since); firmware/zoneinfo is IANA's, packed by tools/firmware/zoneinfo.mjs
for f in zoneinfo.idx zoneinfo.dat zoneinfo.version; do
  put "$SYS" "$HERE/zoneinfo/$f" "/usr/share/zoneinfo/$f"
done
echo "Time zones: tzdata $(cat "$HERE/zoneinfo/zoneinfo.version")."
# the data image only holds Rockbox's data, the stock player's APK and the switching scripts
for f in /data/com.innioasis.y1.apk /data/switch-to-stock.sh /data/update/update.sh /data/initialized; do
  exists "$DATA" "$f" && dfs "$DATA" "rm $f"
done
for d in /data/update /data/org.rockbox/lib /data/org.rockbox; do
  if exists "$DATA" "$d"; then
    for f in $(names "$DATA" "$d"); do dfs "$DATA" "rm $d/$f"; done
    dfs "$DATA" "rmdir $d"
  fi
done
echo "Cleared the data image."

say "3/7  Adding Rondo"
# the bundle's files (after its header: the signature, then a .tar.gz) as /system/rondo
rm -rf "$WORK/bundle" && mkdir -p "$WORK/bundle"
tail -c +73 "$BUNDLE" | tar -xzf - -C "$WORK/bundle"
[[ -x "$WORK/bundle/rondo" && -f "$WORK/bundle/start" ]] || fail "$BUNDLE doesn't hold Rondo"
exists "$SYS" /rondo || dfs "$SYS" "mkdir /rondo"
(cd "$WORK/bundle" && find . -type d ! -name . | sed 's|^\./||' | sort) | while read -r d; do
  exists "$SYS" "/rondo/$d" || dfs "$SYS" "mkdir /rondo/$d"
  dfs "$SYS" "sif /rondo/$d uid 0" "sif /rondo/$d gid 0" "sif /rondo/$d mode 040755"
done
(cd "$WORK/bundle" && find . -type f | sed 's|^\./||' | sort) | while read -r p; do
  put "$SYS" "$WORK/bundle/$p" "/rondo/$p"
  [[ -x "$WORK/bundle/$p" ]] && dfs "$SYS" "sif /rondo/$p mode 0100755"
done
dfs "$SYS" "sif /rondo uid 0" "sif /rondo gid 0" "sif /rondo mode 040755"
echo "Rondo $RONDO_VERSION is /system/rondo ($(du -sh "$WORK/bundle" | cut -f1))."
# init starts it, not the Java layer
export PATH="${JAVA_HOME:+$JAVA_HOME/bin:}$PATH"
java "$ROOT/tools/firmware/BootImage.java" "$WORK/rom/boot.img" "$WORK/boot-native.img"
cp "$WORK/boot-native.img" "$WORK/rom/boot.img"

say "4/7  Boot screens"
java "$ROOT/tools/bootscreen/BootScreen.java" "$WORK/bootscreen" "$WORK/rom/logo.bin" >/dev/null
cp "$WORK/bootscreen/logo.bin" "$WORK/rom/logo.bin"
# no SurfaceFlinger, so no boot animation: the logo stays until Rondo draws
if exists "$SYS" /media/bootanimation.zip; then dfs "$SYS" "rm /media/bootanimation.zip"; fi
echo "Rondo's logo is in (it shows until Rondo's first frame)."

say "5/7  System properties"
debugfs -R "dump /build.prop $WORK/build.prop" "$SYS" 2>/dev/null
sed -i.bak -e "s/^ro.build.display.id=.*/ro.build.display.id=Rondo${RONDO_VERSION:+ $RONDO_VERSION}/" -e '/^ro.rondo.firmware=/d' "$WORK/build.prop"
echo "ro.rondo.firmware=$FIRMWARE_LEVEL" >> "$WORK/build.prop"
# SurfaceFlinger only sets the panel up at boot (firmware/rondo/start), with no boot animation
echo "debug.sf.nobootanimation=1" >> "$WORK/build.prop"
put "$SYS" "$WORK/build.prop" /build.prop
echo "Build name: $(grep ro.build.display.id "$WORK/build.prop"), firmware $FIRMWARE_LEVEL"

say "6/7  Checking"
for img in "$SYS" "$DATA"; do
  e2fsck -fn "$img" >/dev/null 2>&1 || fail "$(basename "$img") has file system errors after editing"
done
left="$(names "$SYS" /app | sed 's/\.[^.]*$//' | sort -u | tr '\n' ' ')"
echo "Left in /system/app: $left"
for bad in rockbox innioasis Rockbox.kl 99Y1ButtonScript LatinIME Settings.apk FMRadio; do
  if names "$SYS" /app | grep -qi "^$bad" || names "$DATA" /data | grep -qi "$bad"; then fail "$bad is still in the firmware"; fi
done
cmp -s <(debugfs -R "cat /usr/keylayout/Generic.kl" "$SYS" 2>/dev/null) "$WORK/Stock.kl" || fail "Generic.kl isn't the stock layout"
for f in "${ROOT_DAEMONS[@]}" /bin/am2; do
  if exists "$SYS" "$f"; then fail "/system$f is still in the firmware"; fi
done
for f in /bin/am /bin/pm; do
  st="$(debugfs -R "stat $f" "$SYS" 2>/dev/null)"
  grep -qE "Mode: +0755 " <<<"$st" && grep -qE "User: +0 +Group: +2000( |$)" <<<"$st" || fail "/system$f doesn't have Android's owner and mode (root:shell, 0755)"
done
debugfs -R "cat /bin/am" "$SYS" 2>/dev/null | grep -q 'com.android.commands.am.Am' || fail "/system/bin/am isn't Android's am script"
echo "The stock firmware's root backdoors are out (aggdll, wifi_init, SPWall and what they left)."
exists "$SYS" /rondo/rondo || fail "/system/rondo/rondo is missing"
exists "$SYS" /rondo/start || fail "/system/rondo/start is missing"
[[ -z "$left" ]] || fail "/system/app still holds $left"
echo "Rondo is in /system/rondo; init.rc starts it (BootImage.java checked zygote is off)."
echo "File systems are clean; only Rondo and Android's core are left."

say "7/7  Packaging"
rm -f "$OUT" "$WORK/cmds" "$WORK/debugfs.log"
# what Innioasis Updater CE shows in its install prompt, and its picture (both optional; the
# Updater keeps them next to the scatter file and prunes the base ROM's Windows tools)
cat > "$WORK/rom/fw_package.ini" <<EOF
[fw_package]
name = Rondo
version = ${RONDO_VERSION}
model = Y1
type = Type A
description = An iPod-style player for the Y1: the music, videos and photos on the card, YouTube Music and YouTube (straight from YouTube, or through your own Rondo server), and FM radio. Installing replaces everything on the player itself; the microSD card is left alone.
EOF
cp "$HERE/updater.png" "$WORK/rom/updater.png"
(cd "$WORK/rom" && zip -q -9 -r -X "$OUT" .)
echo "$(du -h "$OUT" | cut -f1)  $OUT"
echo "SHA-256: $(shasum -a 256 "$OUT" | cut -d' ' -f1)"
