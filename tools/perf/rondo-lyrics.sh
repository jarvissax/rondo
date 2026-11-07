#!/usr/bin/env bash
# Timed lyrics gliding while a song plays (the music volume must be 0 first: it plays aloud otherwise):
# Library > Songs > the first song, the lyrics page, 45 s of lines, paused. Prints rondo's frame
# report; the per-frame CSV goes to private/perf/.
set -euo pipefail
source "$(dirname "$0")/y1.sh"
y1_push rondo inject
adb shell "getprop init.svc.surfaceflinger" | grep -q stopped || y1_stop_android
adb shell "kill \$(cat $DIR/rondo.pid) 2>/dev/null; sleep 0.5; cd $DIR; rm -f frames.csv; (RONDO_USB_PROMPT=0 RONDO_PLAYER_LOG=1 RONDO_SLOW=1 ./run.sh --csv $DIR/frames.csv > rondo.log 2>&1 </dev/null &); sleep 2.5"
I() { adb shell "$DIR/inject $*"; }
I wheel 1 300; sleep 0.5; I key center; sleep 1          # Library
I wheel 3 300; sleep 0.6; I key center; sleep 1.5        # Songs
I wheel 1 300; sleep 0.6; I key center; sleep 2          # the first song: Now Playing
I key center; sleep 45                                    # the lyrics page
I key play; sleep 1; I key center; sleep 0.5             # paused, back to Now Playing
adb shell "kill -INT \$(cat $DIR/rondo.pid); sleep 1; grep -v '^player' $DIR/rondo.log; grep '^player' $DIR/rondo.log | tail -2"
mkdir -p "$ROOT/private/perf"
stamp=$(date +%Y%m%d-%H%M%S)
adb pull "$DIR/frames.csv" "$ROOT/private/perf/rondo-lyrics-$stamp.csv" >/dev/null && echo "frames: private/perf/rondo-lyrics-$stamp.csv"
