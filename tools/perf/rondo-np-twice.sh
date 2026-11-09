#!/usr/bin/env bash
# Now Playing opened twice in each of three runs (the music volume must be 0 first; the wheel is never
# turned on Now Playing): tells a first-use cost from one every opening has. Prints the slow frames.
set -euo pipefail
source "$(dirname "$0")/y1.sh"
trap y1_rondo_again EXIT
y1_push rondo inject
adb shell "getprop init.svc.surfaceflinger" | grep -q stopped || y1_stop_android
adb shell "kill \$(cat $DIR/rondo.pid) 2>/dev/null; sleep 0.5; cd $DIR; (RONDO_USB_PROMPT=0 RONDO_SLOW=1 ./run.sh > rondo.log 2>&1 </dev/null &); sleep 2.5"
I() { adb shell "$DIR/inject $*"; }
for run in 1 2 3; do
  echo "== run $run"
  adb shell "kill \$(cat $DIR/rondo.pid) 2>/dev/null; sleep 0.5; cd $DIR; (RONDO_USB_PROMPT=0 RONDO_SLOW=1 ./run.sh > rondo.log 2>&1 </dev/null &); sleep 2.5"
  I wheel 1 300; sleep 0.5; I key center; sleep 1          # Library
  I wheel 3 300; sleep 0.6; I key center; sleep 1.5        # Songs
  I wheel 1 300; sleep 0.6; I key center; sleep 2          # the first song: Now Playing (1st open)
  I key play; sleep 0.5; adb shell "echo open2 >> $DIR/rondo.log"
  I key menu; sleep 1.2                                     # back to Songs
  I wheel 1 300; sleep 0.6; I key center; sleep 2          # the second song: Now Playing (2nd open)
  I key play; sleep 0.5
  adb shell "kill -INT \$(cat $DIR/rondo.pid); sleep 1; grep -E 'slow: frame|open2|missed' $DIR/rondo.log | grep -v 'frame at [12][0-9][0-9] ms'"
done
