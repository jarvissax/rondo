#!/usr/bin/env bash
# Now Playing's states, from the main menu with Now Playing selected, with either capture tool:
#   tools/parity/np-steps.sh capture     (1.0.x)  |  tools/parity/np-steps.sh shot   (native)
# The wheel is volume on Now Playing, so these steps never turn it there.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 1 300; sleep 0.6           # Library
I key center; sleep 1.5
I wheel 3 300; sleep 0.8           # Songs
I key center; sleep 1.8
I wheel 1 300; sleep 0.8           # the first song
I key center; sleep 2.5            # plays it: Now Playing
SETTLE=1.5 $T np-paused key play >/dev/null
SETTLE=1.5 $T np-lyrics key center >/dev/null       # the lyrics page
I key center; sleep 0.8            # back to Now Playing
SETTLE=1.2 $T np-options key center 900 >/dev/null
I wheel 2 300; sleep 0.8           # the options' cursor: Shuffle
$T np-options-shuffle >/dev/null
I wheel 2 300 ccw; sleep 0.6
SETTLE=1.2 $T np-scrub key center >/dev/null     # Scrub through the song
I key menu; sleep 0.8              # cancels the scrub
I key menu; sleep 1.2              # back to Songs
I key menu; sleep 1                # Library
I key menu; sleep 1                # the main menu
SETTLE=1.5 $T mm-np-paused wheel 1 300 ccw >/dev/null    # Now Playing: the pane shows the song
