#!/usr/bin/env bash
# Opens the Library's pages, YouTube, YouTube Music and Settings' pages one after another, each followed
# by Back, and prints what RONDO_SLOW found slow, then the frame report. The presses run from a script
# on the Y1 itself: an adb shell per press disturbs the timing (Nov 9: a stray 24 ms wait now and then
# that never showed without it). Leaves out FM Radio (it plays) and the switches. The music volume must
# be 0 first.
set -euo pipefail
source "$(dirname "$0")/y1.sh"
trap y1_rondo_again EXIT
y1_push rondo inject
adb shell "getprop init.svc.surfaceflinger" | grep -q stopped || y1_stop_android

# where each section is in this Y1's main menu (Settings › Main menu can reorder it)
order=$(adb shell cat /data/data/com.jarvissax.rondo/files/config.json 2>/dev/null | tr -d '\r' |
  sed -n 's/.*"menu":\[\([^]]*\)\].*/\1/p' | grep -o '"id":"[A-Za-z]*"' | cut -d'"' -f4 | tr '\n' ' ')
[ -n "$order" ] || order="music videos device nowPlaying radio settings"
at() { local i=0; for id in $order; do [ "$id" = "$1" ] && { echo $i; return; }; i=$((i + 1)); done; echo -1; }
lib=$(at device) yt=$(at videos) ytm=$(at music) set=$(at settings)
echo "main menu: $order"

script=$(mktemp)
cat > "$script" <<EOF
D=$DIR
I() { \$D/inject "\$@"; }
open() { I key center; sleep 1.3; I key menu; sleep 0.9; }   # the selected row, then back
down() { [ \$1 -gt 0 ] && I wheel \$1 300; sleep 0.5; }
top() { I wheel 9 120 ccw; sleep 0.5; }
kill \$(cat \$D/rondo.pid) 2>/dev/null; sleep 0.5; cd \$D
(${RONDO_HZ:+RONDO_HZ=$RONDO_HZ }RONDO_USB_PROMPT=0 RONDO_SLOW=1 ./run.sh > rondo.log 2>&1 </dev/null &); sleep 2.5
if [ $lib -ge 0 ]; then
  top; down $lib; I key center; sleep 1.2                      # Library
  top; down 1; open; down 1; open; down 2; open                # Search, Playlists, Artists
  down 1; open; down 1; open; down 1; open                     # Albums, Recently added, Videos
  I key menu; sleep 0.9
fi
if [ $yt -ge 0 ]; then top; down $yt; open; fi                 # YouTube
if [ $ytm -ge 0 ]; then top; down $ytm; open; fi               # YouTube Music
if [ $set -ge 0 ]; then
  top; down $set; I key center; sleep 1.2                      # Settings
  for r in 1 2 3 4 5 6 7 8; do open; down 1; done              # Dashboard .. Screen off after
  down 1                                                       # past Open lyrics automatically (a switch)
  open; down 1; open; down 1                                   # Wheel acceleration, Wheel vibration
  down 1                                                       # past Click sound (a switch)
  open; down 1; open; down 1; open                             # Server, Software update, About
fi
kill -INT \$(cat \$D/rondo.pid); sleep 1
grep -E 'slow|missed' \$D/rondo.log
EOF
adb push "$script" "$DIR/openings.sh" >/dev/null
rm -f "$script"
adb shell sh "$DIR/openings.sh"
