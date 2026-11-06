#!/usr/bin/env bash
# Screenshots of Rondo 1.0.x on the Y1 (Android up), the references native screens are compared with
# (private/native-plan.md, Parity method). They show the user's library and account, so they go to
# private/parity/1.0/.
#
#   tools/parity/capture.sh <name> [inject args...]     e.g. capture.sh songs-top key center
#   With inject args, runs them first (tools/perf's injector) and waits for the screen to settle.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export PATH=$PATH:${ANDROID_SDK:-$HOME/Library/Android/sdk}/platform-tools
name=$1
shift
if [[ $# -gt 0 ]]; then
  adb shell "/data/local/tmp/rondo/inject $*"
  sleep "${SETTLE:-1.2}"
fi
mkdir -p "$ROOT/private/parity/1.0"
adb shell screencap -p /data/local/tmp/rondo/ref.png
adb pull /data/local/tmp/rondo/ref.png "$ROOT/private/parity/1.0/$name.png" >/dev/null
echo "private/parity/1.0/$name.png"
