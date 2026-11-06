#!/usr/bin/env bash
# Runs a native program from native/build/y1 on the Y1 with the Java layer and SurfaceFlinger stopped,
# and helpers the perf scenarios share. Source it, or run `tools/perf/y1.sh push <names...>`.
#
#   push <names...>     strip and push to /data/local/tmp/rondo
#   stop-android        stop zygote (system_server, today's Rondo) and SurfaceFlinger
#   start-android       bring them back
#   shot <file.png>     ask the running uitest for a screenshot and pull it
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
export PATH=$PATH:$SDK/platform-tools
STRIP=$SDK/ndk/23.2.8568313/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip
DIR=/data/local/tmp/rondo

y1_push() {
  for n in "$@"; do
    "$STRIP" -o "$ROOT/native/build/y1/$n.stripped" "$ROOT/native/build/y1/$n"
    adb push "$ROOT/native/build/y1/$n.stripped" "$DIR/$n" >/dev/null
    adb shell chmod 755 "$DIR/$n"
  done
}
y1_stop_android() { adb shell 'stop zygote; sleep 2; stop surfaceflinger; sleep 1'; }
y1_start_android() { adb shell 'start surfaceflinger; sleep 1; start zygote'; }
y1_shot() {
  adb shell "kill -USR1 \$(cat $DIR/uitest.pid)"
  sleep 0.5
  adb pull "$DIR/shot.png" "$1" >/dev/null
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  cmd=${1:-}
  shift || true
  case $cmd in
    push) y1_push "$@" ;;
    stop-android) y1_stop_android ;;
    start-android) y1_start_android ;;
    shot) y1_shot "$1" ;;
    *) sed -n 2,9p "$0"; exit 2 ;;
  esac
fi
