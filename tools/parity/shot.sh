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
adb shell "rm -f $D/shot.png; kill -USR1 \$(cat $D/rondo.pid); i=0; while [ ! -f $D/shot.png ] && [ \$i -lt 30 ]; do sleep 0.1; i=\$((i+1)); done; sleep 0.2"
adb pull "$D/shot.png" "$ROOT/private/parity/2.0/$name.png" >/dev/null
echo "private/parity/2.0/$name.png"
