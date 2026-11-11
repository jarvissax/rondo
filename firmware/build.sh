#!/usr/bin/env bash
# Builds the Rondo firmware for the Innioasis Y1, Type A (rom.zip, for Innioasis Updater).
#
#   firmware/build.sh path/to/rondo.apk [firmware/out/rom.zip]          Rondo 1.0.x (firmware level 1)
#   firmware/build.sh path/to/rondo-X.rondo [firmware/out/rom.zip]      Rondo 2.0 (firmware level 2)
#
# Rondo 2.0 (a bundle from tools/native/bundle.sh) runs without Android's Java layer: no app is left in
# /system/app, Rondo goes in as /system/rondo, and the boot image's init.rc starts it instead of zygote
# (tools/firmware/BootImage.java); SurfaceFlinger only sets the panel up, then firmware/rondo/start stops
# it. The boot logo shows until Rondo's first frame.
#
# It starts from the community's base ROM (stock Android 4.2.2 with the Y1's radios enabled) and
# takes out everything that isn't Rondo or Android's own background services: Rockbox, the stock
# Innioasis player, the Rockbox/stock switching scripts, Android's Settings, keyboard, browser,
# FM app, wallpapers and the rest. Rondo goes in as the only app with a screen, so the Y1 boots
# straight into it. The images are edited in place with debugfs, so no mounting or root is needed.
#
# Needs curl, unzip, zip, java (17+) and e2fsprogs (debugfs, e2fsck):
#   macOS: brew install e2fsprogs        Debian/Ubuntu: sudo apt install e2fsprogs
set -euo pipefail

APK="${1:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${2:-$HERE/out/rom.zip}"
# shown as the build name; defaults to the app's version in app/app/build.gradle.kts
RONDO_VERSION="${RONDO_VERSION:-$(sed -n 's/.*versionName = "\(.*\)".*/\1/p' "$ROOT/app/app/build.gradle.kts")}"
# this firmware's number (firmwareLevel in the same file): updates over Wi-Fi, the dashboard or the
# update page install only on a firmware at least as new as the release needs (SoftwareUpdate.kt)
FIRMWARE_LEVEL="$(sed -n 's/^val firmwareLevel = \([0-9][0-9]*\).*/\1/p' "$ROOT/app/app/build.gradle.kts")"
WORK="$HERE/work"

BASE_URL="https://github.com/y1-community/y1-ata-rom/releases/download/0.1/rom.zip"
BASE_SHA256="d3b79ca460a3d3ec2c41ba57893127a0f9a90278e374dd33cdb5de5bb46e367e"

# Android's own background services, kept because the system or Rondo relies on them. Everything
# else in /system/app goes, SystemUI included: on this firmware it opens its own touch-only "USB
# connected" screen whenever a computer is plugged in, and Rondo does its other visible jobs (the
# wheel's click, the low battery warning, the wallpaper Android waits for before it ends the boot
# animation) itself.
KEEP_APPS=(
  SettingsProvider        # where Android keeps its settings
  DefaultContainerService # installs and updates apps (Rondo updates over USB)
  MtkBt                   # MediaTek's Bluetooth stack
  KeyChain                # certificate storage used by the system
  DrmProvider             # media DRM framework, queried by the media stack
  DownloadProvider        # Android's download service (no screen)
  TelephonyProvider       # carrier database the framework expects, even without a modem
  UserDictionaryProvider  # text input dictionary the framework expects
  FusedLocation           # location service the framework binds to
  MTKThermalManager       # keeps the chip within temperature limits
  BatteryWarning          # low battery and charger warnings
  Provision               # marks the device as set up on first boot, then disables itself
)

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

[[ -n "$APK" && -f "$APK" ]] || fail "Usage: firmware/build.sh path/to/rondo.apk|rondo-X.rondo [out.zip]"
NATIVE=0
if [[ "$APK" == *.rondo ]]; then
  NATIVE=1
  # 2.0's version and firmware number are the bundle's own (native/core/version.h)
  RONDO_VERSION="$(sed -n 's/.*kRondoVersion = "\(.*\)".*/\1/p' "$ROOT/native/core/version.h")"
  FIRMWARE_LEVEL="$(sed -n 's/.*kFirmwareNeeded = \([0-9][0-9]*\).*/\1/p' "$ROOT/native/core/version.h")"
  KEEP_APPS=()
fi
[[ -n "$FIRMWARE_LEVEL" ]] || fail "No firmwareLevel in app/app/build.gradle.kts"
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
  base="${f%.*}"
  keep=0
  for k in "${KEEP_APPS[@]+"${KEEP_APPS[@]}"}"; do [[ "$base" == "$k" ]] && keep=1; done
  if [[ $keep == 0 ]]; then dfs "$SYS" "rm /app/$f"; removed+=("$f"); fi
done
echo "Removed from /system/app: $(printf '%s\n' "${removed[@]}" | sed 's/\.[^.]*$//' | sort -u | tr '\n' ' ')"
# Rockbox's library, its key layout and the Rockbox/stock switching script
for f in /lib/librockbox.so /usr/keylayout/Rockbox.kl /etc/init.d/99Y1ButtonScript; do
  exists "$SYS" "$f" && dfs "$SYS" "rm $f" && echo "Removed $f"
done
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
if [[ $NATIVE == 1 ]]; then
  # the bundle's files (after its header: the signature, then a .tar.gz) as /system/rondo
  rm -rf "$WORK/bundle" && mkdir -p "$WORK/bundle"
  tail -c +73 "$APK" | tar -xzf - -C "$WORK/bundle"
  [[ -x "$WORK/bundle/rondo" && -f "$WORK/bundle/start" ]] || fail "$APK doesn't hold Rondo"
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
else
  put "$SYS" "$APK" /app/Rondo.apk
  echo "Rondo is /system/app/Rondo.apk."
fi

say "4/7  Boot screens"
java "$ROOT/tools/bootscreen/BootScreen.java" "$WORK/bootscreen" "$WORK/rom/logo.bin" >/dev/null
cp "$WORK/bootscreen/logo.bin" "$WORK/rom/logo.bin"
if [[ $NATIVE == 1 ]]; then
  # no SurfaceFlinger, so no boot animation: the logo stays until Rondo draws
  exists "$SYS" /media/bootanimation.zip && dfs "$SYS" "rm /media/bootanimation.zip"
  echo "Rondo's logo is in (it shows until Rondo's first frame)."
else
  put "$SYS" "$WORK/bootscreen/bootanimation.zip" /media/bootanimation.zip
  echo "Rondo's logo and boot animation are in."
fi

say "5/7  System properties"
debugfs -R "dump /build.prop $WORK/build.prop" "$SYS" 2>/dev/null
sed -i.bak -e "s/^ro.build.display.id=.*/ro.build.display.id=Rondo${RONDO_VERSION:+ $RONDO_VERSION}/" -e '/^ro.rondo.firmware=/d' "$WORK/build.prop"
echo "ro.rondo.firmware=$FIRMWARE_LEVEL" >> "$WORK/build.prop"
# 2.0: SurfaceFlinger only sets the panel up at boot (firmware/rondo/start), with no boot animation
[[ $NATIVE == 1 ]] && echo "debug.sf.nobootanimation=1" >> "$WORK/build.prop"
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
if [[ $NATIVE == 1 ]]; then
  exists "$SYS" /rondo/rondo || fail "/system/rondo/rondo is missing"
  exists "$SYS" /rondo/start || fail "/system/rondo/start is missing"
  [[ -z "$left" ]] || fail "/system/app still holds $left"
  echo "Rondo 2.0 is in /system/rondo; init.rc starts it (BootImage.java checked zygote is off)."
fi
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
