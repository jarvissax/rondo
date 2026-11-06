#!/usr/bin/env bash
# Song changes in Now Playing: each one cross-fades the cover and the blurred full-screen backdrop
# (180 ms) while the next pictures decode and upload. Prints uitest's frame report.
#
#   tools/perf/n1-nowplaying.sh [changes] [gap ms]       default 60, 400; UITEST_ARGS adds uitest options
set -euo pipefail
source "$(dirname "$0")/y1.sh"
N=${1:-60}
GAP=${2:-400}
y1_push uitest inject
adb shell "getprop init.svc.surfaceflinger" | grep -q stopped || y1_stop_android
adb shell "cd $DIR; rm -f frames.csv; (./uitest --csv $DIR/frames.csv ${UITEST_ARGS:-} > ui.log 2>&1 </dev/null &); sleep 2"
adb shell "cd $DIR; ./inject wheel 2 60; sleep 0.5; ./inject key center; sleep 1.5; i=0; while [ \$i -lt $N ]; do ./inject key next 40; usleep ${GAP}000; i=\$((i+1)); done; sleep 1"
adb shell "kill -INT \$(cat $DIR/uitest.pid); sleep 1; cat $DIR/ui.log"
stamp=$(date +%Y%m%d-%H%M%S)
mkdir -p "$ROOT/private/perf"
adb pull "$DIR/frames.csv" "$ROOT/private/perf/n1-np-$stamp.csv" >/dev/null && echo "frames: private/perf/n1-np-$stamp.csv"
