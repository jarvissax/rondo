#!/usr/bin/env bash
# Installs Rondo on an Innioasis Y1 running Android 4.2 and makes it the home screen
# (docs/install.md). Rondo is an ordinary launcher app: nothing else on the Y1 is changed, and
# the launcher you had before stays installed.
#
#   tools/install.sh rondo-1.0.0.apk
#
# Needs adb (Android platform-tools) on the PATH, or ADB=/path/to/adb.
set -euo pipefail

APK="${1:-}"
ADB="${ADB:-adb}"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
fail() { printf '\n\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
sh_() { "$ADB" shell "$@" | tr -d '\r'; }

[[ -n "$APK" ]] || fail "Usage: tools/install.sh path/to/rondo.apk"
[[ -f "$APK" ]] || fail "Can't find $APK"
command -v "$ADB" >/dev/null || fail "adb isn't installed. Get Android platform-tools: https://developer.android.com/tools/releases/platform-tools"

say "1/3  Looking for the Y1"
"$ADB" get-state >/dev/null 2>&1 || fail "No Y1 found. Plug it in with a data cable, switch it on and try again."
[[ "$(sh_ getprop ro.product.model)" == "Y1" ]] || fail "The connected device isn't an Innioasis Y1."
[[ "$(sh_ getprop ro.build.version.sdk)" == "17" ]] || fail "This Y1 isn't on Android 4.2. Flash \"Android 4.2 (Dev)\" first (docs/install.md, step 1)."
echo "Found a Y1 on Android 4.2."

say "2/3  Installing Rondo"
"$ADB" install -r "$APK" | tr -d '\r' | tail -1

say "3/3  Making Rondo the home screen"
# Android asks which app should be the home screen. Choose Rondo and "Always" in that dialog by
# tapping through it over USB (the click wheel can't move through Android's own lists).
tap_text() {
  sh_ uiautomator dump /data/local/tmp/rondo-ui.xml >/dev/null 2>&1 || return 1
  local b
  b=$(sh_ cat /data/local/tmp/rondo-ui.xml | grep -o "text=\"$1\"[^>]*bounds=\"[^\"]*\"" | head -1 | grep -o 'bounds="[^"]*"') || return 1
  [[ -n "$b" ]] || return 1
  local n=(${b//[^0-9]/ })
  sh_ input tap $(( (n[0] + n[2]) / 2 )) $(( (n[1] + n[3]) / 2 ))
}
focus() { sh_ dumpsys window | grep mCurrentFocus; }
sh_ am start -a android.intent.action.MAIN -c android.intent.category.HOME >/dev/null
# wait for the home screen or Android's chooser (a freshly updated Rondo takes a few seconds)
for _ in 1 2 3 4 5 6 7 8 9 10; do
  sleep 1
  [[ "$(focus)" == *com.jarvissax.rondo* || "$(focus)" == *ResolverActivity* ]] && break
done
if [[ "$(focus)" == *com.jarvissax.rondo* ]]; then
  echo "Done: Rondo is already the home screen."
elif [[ "$(focus)" == *ResolverActivity* ]] && tap_text "Rondo" && sleep 1 && tap_text "Always"; then
  sleep 2
  echo "Done: the Y1 now starts in Rondo."
else
  echo "If the Y1 shows \"Complete action using\", choose Rondo, then Always."
fi
sh_ rm -f /data/local/tmp/rondo-ui.xml

say "Next: connect a server to stream YouTube Music and YouTube (docs/server.md)."
