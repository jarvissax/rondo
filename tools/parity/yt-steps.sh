#!/usr/bin/env bash
# YouTube's screens (Home, Subscriptions, History, search), from the main menu with Now Playing
# selected, as ytm-steps.sh: shoot native with RONDO_API_RECORD, then capture 1.0.x within 2 minutes.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 2 300; sleep 0.8                      # YouTube
SETTLE=8 $T yt-home key center >/dev/null
SETTLE=1.5 $T yt-home-row3 wheel 3 300 >/dev/null
SETTLE=1.5 $T yt-pills wheel 4 300 ccw >/dev/null    # the pills, Home in focus
I wheel 1 300; sleep 0.6                      # Subscriptions
SETTLE=7 $T yt-subs key center >/dev/null
I wheel 1 300 ccw; sleep 0.6                  # the pills, Subscriptions in focus
I wheel 1 300; sleep 0.6                      # History
SETTLE=7 $T yt-history key center >/dev/null
I wheel 1 300 ccw; sleep 0.6
I wheel 1 300; sleep 0.6                      # search
SETTLE=2 $T yt-search key center >/dev/null
I key menu; sleep 1.2
I key menu; sleep 1.2                         # the main menu
I wheel 2 300 ccw; sleep 0.8                  # Now Playing selected again
