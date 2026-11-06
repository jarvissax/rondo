#!/usr/bin/env bash
# Restarts the native rondo on the Y1 and captures the states the 1.0.x references hold, with the same
# injector steps (tools/parity/capture.sh took them on 1.0.x), into private/parity/2.0/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
adb shell "kill \$(cat /data/local/tmp/rondo/rondo.pid) 2>/dev/null; sleep 0.5; cd /data/local/tmp/rondo; (./run.sh > rondo.log 2>&1 </dev/null &); sleep 2.5"
T=$HERE/shot.sh
$T mm-nowplaying >/dev/null
$T mm-library wheel 1 80 >/dev/null
$T mm-youtube wheel 1 80 >/dev/null
$T mm-music wheel 1 80 >/dev/null
$T mm-radio wheel 1 80 >/dev/null
$T mm-settings wheel 1 80 >/dev/null
adb shell '/data/local/tmp/rondo/inject wheel 4 120 ccw'
sleep 0.8
$T lib-menu-top key center >/dev/null
$T lib-menu-end wheel 9 120 >/dev/null
adb shell '/data/local/tmp/rondo/inject wheel 9 120 ccw'
sleep 0.8
$T lib-menu-songs wheel 3 120 >/dev/null
SETTLE=1.5 $T songs-top key center >/dev/null
$T songs-sel6 wheel 6 120 >/dev/null
# back to Library (Songs selected), then the next four lists
for name in artists albums recent videos; do
  adb shell "/data/local/tmp/rondo/inject key menu"
  sleep 0.8
  adb shell "/data/local/tmp/rondo/inject wheel 1 300"
  sleep 0.6
  SETTLE=1.8 $T $name-top key center >/dev/null
done
