#!/usr/bin/env bash
# Settings' system screens (Wi-Fi, a saved network, Bluetooth, Shut down) and the FM radio, from the
# main menu with Now Playing selected, with either capture tool (as settings-steps.sh). Nothing is
# switched, joined or paired; the radio needs wired headphones in (its antenna) and plays at volume 0.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 5 300; sleep 0.8                      # Settings
I key center; sleep 1.5
I wheel 1 300; sleep 0.6                      # Wi-Fi
SETTLE=4 $T wifi key center >/dev/null
I wheel 2 300; sleep 0.6                      # the network joined (under the toggle and its header)
SETTLE=1.5 $T wifi-network key center >/dev/null
I key menu; sleep 1.2
I key menu; sleep 1.2                         # Settings, Wi-Fi selected
I wheel 1 300; sleep 0.6                      # Bluetooth
SETTLE=4 $T bluetooth key center >/dev/null
I key menu; sleep 1.2
SETTLE=1.5 $T settings-shutdown wheel 14 150 >/dev/null   # the end of Settings: Shut down
SETTLE=1.5 $T power key center >/dev/null
I key menu; sleep 1.2
I wheel 16 150 ccw; sleep 1
I key menu; sleep 1.2                         # the main menu, Settings selected
I wheel 1 300 ccw; sleep 0.6                  # FM Radio
SETTLE=5 $T fm key center >/dev/null
SETTLE=1.5 $T fm-options key center 900 >/dev/null
SETTLE=1.5 $T fm-stations key center >/dev/null
I key menu; sleep 1.2
I wheel 5 300; sleep 0.6                      # Turn radio off (the options' last row)
I key center; sleep 1.5                       # off, back to the radio
I key menu; sleep 1.2                         # the main menu
I wheel 4 300 ccw; sleep 0.8                  # Now Playing selected again
