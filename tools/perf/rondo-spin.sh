#!/usr/bin/env bash
# The native shell under a full-speed wheel: Library > Songs, 820 pulses 40 ms apart, back, then the
# Library menu and Artists. Prints rondo's frame report; the per-frame CSV goes to private/perf/.
set -euo pipefail
source "$(dirname "$0")/y1.sh"
y1_push rondo inject
adb shell "getprop init.svc.surfaceflinger" | grep -q stopped || y1_stop_android
adb shell "kill \$(cat $DIR/rondo.pid) 2>/dev/null; sleep 0.5; cd $DIR; rm -f frames.csv; (./rondo --csv $DIR/frames.csv > rondo.log 2>&1 </dev/null &); sleep 2.5"
I() { adb shell "$DIR/inject $*"; }
I wheel 1 300; sleep 0.5; I key center; sleep 1          # Library
I wheel 3 300; sleep 0.6; I key center; sleep 1          # Songs
I wheel 820 40; sleep 1.5                                 # a fast spin to the end
I wheel 820 40 ccw; sleep 1.5                             # and back
I key menu; sleep 1; I wheel 1 300; sleep 0.5; I key center; sleep 1   # Artists
I wheel 300 40; sleep 1.5
adb shell "kill -INT \$(cat $DIR/rondo.pid); sleep 1; cat $DIR/rondo.log"
mkdir -p "$ROOT/private/perf"
stamp=$(date +%Y%m%d-%H%M%S)
adb pull "$DIR/frames.csv" "$ROOT/private/perf/rondo-spin-$stamp.csv" >/dev/null && echo "frames: private/perf/rondo-spin-$stamp.csv"
