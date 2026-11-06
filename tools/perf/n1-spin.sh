#!/usr/bin/env bash
# N1's exit scenario: a real-speed wheel spin through every song (one pulse every 40 ms, like a fast
# spin of the real wheel), then the dark image list and five song changes in Now Playing. Prints uitest's frame report and
# saves the per-frame CSV to private/perf/.
#
#   tools/perf/n1-spin.sh [pulses] [gap ms]       default 820, 40; UITEST_ARGS adds uitest options
set -euo pipefail
source "$(dirname "$0")/y1.sh"
PULSES=${1:-820}
GAP=${2:-40}
OUT=$ROOT/private/perf
mkdir -p "$OUT"
y1_push uitest inject
adb shell "getprop init.svc.surfaceflinger" | grep -q stopped || y1_stop_android
# a fixed menu order (Library, YouTube Music, ...) and the default look, whatever the device's settings say
adb shell "echo '{\"theme\":{\"menu\":[{\"id\":\"device\"},{\"id\":\"music\"},{\"id\":\"nowPlaying\"}]}}' > $DIR/perf-config.json"
adb shell "cd $DIR; rm -f frames.csv; (./uitest --config $DIR/perf-config.json --csv $DIR/frames.csv ${UITEST_ARGS:-} > ui.log 2>&1 </dev/null &); sleep 2"
adb shell "cd $DIR; ./inject key center; sleep 0.8; ./inject key center; sleep 1; ./inject wheel $PULSES $GAP cw; sleep 1.5"
adb shell "cd $DIR; ./inject key menu; sleep 0.6; ./inject key menu; sleep 1; ./inject wheel 1 40 cw; sleep 0.5; ./inject key center; sleep 1; ./inject wheel 119 $GAP cw; sleep 1.5"
# now playing: five song changes (cover and backdrop cross-fades), then back
adb shell "cd $DIR; ./inject key center; sleep 1; for i in 1 2 3 4 5; do ./inject key next; sleep 0.4; done; ./inject key menu; sleep 1"
adb shell "kill -INT \$(cat $DIR/uitest.pid); sleep 1; cat $DIR/ui.log"
stamp=$(date +%Y%m%d-%H%M%S)
adb pull "$DIR/frames.csv" "$OUT/n1-spin-$stamp.csv" >/dev/null && echo "frames: private/perf/n1-spin-$stamp.csv"
