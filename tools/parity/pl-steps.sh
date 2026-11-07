#!/usr/bin/env bash
# The playlist screens, from the main menu with Now Playing selected, with either capture tool:
#   tools/parity/pl-steps.sh capture     (1.0.x)  |  tools/parity/pl-steps.sh shot   (native)
# They only look: nothing is created, changed or deleted (no Create, Save, Delete or Remove is pressed),
# and Menu leaves the keyboard screens from their top row.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
T=$HERE/$1.sh
I() { adb shell "/data/local/tmp/rondo/inject $*"; }
I wheel 1 300; sleep 0.6           # Library
I key center; sleep 1.5
I wheel 2 300; sleep 0.8           # Playlists
SETTLE=1.5 $T pl-list key center >/dev/null
I wheel 1 300; sleep 0.8           # the first playlist
SETTLE=1.5 $T pl-one key center >/dev/null
I wheel 2 300; sleep 0.8           # its first song
SETTLE=1.5 $T pl-song-menu key center 900 >/dev/null
I key menu; sleep 1                # back to the playlist
I wheel 1 300 ccw; sleep 0.6       # Add songs
SETTLE=1.8 $T pl-add key center >/dev/null
I key menu; sleep 1                # the keyboard's top row: closes it
I key menu; sleep 1                # back to Playlists (the playlist selected)
SETTLE=1.5 $T pl-menu key center 900 >/dev/null
I key menu; sleep 1
I wheel 1 300 ccw; sleep 0.6       # New playlist
SETTLE=1.8 $T pl-new key center >/dev/null
I key menu; sleep 1                # leaves without creating
I key menu; sleep 1                # Library
I key menu; sleep 1                # the main menu
I wheel 1 300 ccw; sleep 0.6       # Now Playing selected again
