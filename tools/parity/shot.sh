#!/usr/bin/env bash
# A screenshot of the running native rondo into private/parity/2.0/<name>.png, after optional injector
# arguments (as capture.sh does for 1.0.x), so the same steps give the same states on both sides.
#
#   tools/parity/shot.sh <name> [inject args...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
name=$1
shift
D=/data/local/tmp/rondo
if [[ $# -gt 0 ]]; then
  adb shell "$D/inject $*"
  sleep "${SETTLE:-1.2}"
fi
mkdir -p "$ROOT/private/parity/2.0"
# done once rondo logs "screenshot saved" again (writing it takes longer while a video plays)
adb shell "rm -f $D/shot.png; n=\$(grep -c 'screenshot saved' $D/rondo.log); kill -USR1 \$(cat $D/rondo.pid); i=0; while [ \$(grep -c 'screenshot saved' $D/rondo.log) -le \$n ] && [ \$i -lt 80 ]; do sleep 0.1; i=\$((i+1)); done"
adb pull "$D/shot.png" "$ROOT/private/parity/2.0/$name.png" >/dev/null
echo "private/parity/2.0/$name.png"
