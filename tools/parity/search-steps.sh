#!/usr/bin/env bash
# The Search screen's states, from the main menu with Now Playing selected, with either capture tool:
#   tools/parity/search-steps.sh capture     (1.0.x)  |  tools/parity/search-steps.sh shot   (native)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 1 300; sleep 0.6           # Library
I key center; sleep 1.5
I wheel 1 300; sleep 0.6           # Search (never Shuffle songs)
SETTLE=1.8 $T search-empty key center >/dev/null
I wheel 1 300; sleep 1; I key center; sleep 1.2     # b
I wheel 1 300 ccw; sleep 1.2
SETTLE=2.5 $T search-ba key center >/dev/null           # a
for k in 1 2 3 4; do I key play; sleep 0.8; done       # down to the last row
for k in 1 2 3 4; do I key next; sleep 0.8; done       # right to Show
sleep 1.5; $T search-ba-show >/dev/null
SETTLE=2.5 $T search-ba-results key center >/dev/null
I key menu; sleep 0.8                                   # back to typing
I key menu; sleep 0.5; I key menu; sleep 0.5; I key menu; sleep 0.5; I key menu; sleep 0.8  # up to the top row, out
I key menu; sleep 1                                     # closes the keyboard: Library
I key menu; sleep 1                                     # Library -> main menu
