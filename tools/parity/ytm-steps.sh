#!/usr/bin/env bash
# YouTube Music's screens, from the main menu with Now Playing selected, with either capture tool:
#   tools/parity/ytm-steps.sh capture     (1.0.x)  |  tools/parity/ytm-steps.sh shot   (native)
# The server keeps a browse answer 2 minutes: shoot native with RONDO_API_RECORD, then capture 1.0.x
# within that, so both show the same feed. Nothing is played (no song row is pressed).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 3 300; sleep 0.8                      # YouTube Music
SETTLE=8 $T ytm-home key center >/dev/null
SETTLE=1.5 $T ytm-home-row2 wheel 2 300 >/dev/null
SETTLE=1.5 $T ytm-pills wheel 3 300 ccw >/dev/null   # past the first row: the pills, Home in focus
I wheel 1 300; sleep 0.6                      # Library
SETTLE=7 $T ytm-library key center >/dev/null
SETTLE=1.5 $T ytm-library-row2 wheel 2 300 >/dev/null
I wheel 3 300 ccw; sleep 0.6                  # the pills again, Library in focus
I wheel 1 300; sleep 0.6                      # search
SETTLE=2 $T ytm-search key center >/dev/null
I key menu; sleep 1.2                         # the keyboard's top row: out
I key menu; sleep 1.2                         # the main menu
I wheel 3 300 ccw; sleep 0.8                  # Now Playing selected again
