#!/usr/bin/env bash
# Settings' screens, from the main menu with Now Playing selected, with either capture tool:
#   tools/parity/settings-steps.sh capture     (1.0.x)  |  tools/parity/settings-steps.sh shot   (native)
# They only look: no setting is toggled or picked (Menu leaves every screen).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 5 300; sleep 0.8           # Settings
SETTLE=2.5 $T settings-top key center >/dev/null
SETTLE=1.5 $T settings-end wheel 14 150 >/dev/null
I wheel 14 150 ccw; sleep 1
I wheel 3 300; sleep 0.8           # Theme
SETTLE=1.5 $T theme key center >/dev/null
SETTLE=1.5 $T theme-accent key center >/dev/null   # Accent color
I key menu; sleep 1
I wheel 2 300; sleep 0.6           # Text size
SETTLE=1.5 $T theme-size key center >/dev/null
I key menu; sleep 1
I wheel 1 300; sleep 0.6           # Main menu
SETTLE=1.5 $T theme-menu key center >/dev/null
I key menu; sleep 1
I key menu; sleep 1                # Settings (Theme selected)
I wheel 5 300; sleep 0.8           # Wheel acceleration
SETTLE=1.5 $T settings-accel key center >/dev/null
I key menu; sleep 1
I wheel 5 300; sleep 0.8           # About
SETTLE=1.5 $T about key center >/dev/null
I key menu; sleep 1
I key menu; sleep 1                # the main menu
I wheel 5 300 ccw; sleep 0.8       # Now Playing selected again
