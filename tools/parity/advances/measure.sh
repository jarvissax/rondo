#!/usr/bin/env bash
# Measures Advances.java's table on the Y1 (Android's Dalvik through app_process; zygote may be up or
# down) and writes native/assets/advances.bin. Sizes: 1.0.x's text sizes at the three text scales.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
export PATH=$PATH:$SDK/platform-tools
export JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}
export PATH=$JAVA_HOME/bin:$PATH
OUT=$(mktemp -d)
"$JAVA_HOME/bin/javac" -source 8 -target 8 -nowarn -cp "$SDK/platforms/android-34/android.jar" -d "$OUT" "$HERE/Advances.java" 2>/dev/null
"$SDK/build-tools/34.0.0/d8" --min-api 17 --output "$OUT" "$OUT"/Advances.class
(cd "$OUT" && zip -q advances.jar classes.dex)
adb push "$OUT/advances.jar" /data/local/tmp/rondo/advances.jar >/dev/null
sizes=""
for sp in 9 10 11 12 12.5 13 13.5 14 15 16 17 17.5 18 20 21 22 24 26 28 30 32 36 40 44; do
  for k in 1.25 1.4 1.55; do sizes+=" $(awk -v a="$sp" -v b="$k" 'BEGIN { printf "%.4f", a * b }')"; done
done
adb shell "CLASSPATH=/data/local/tmp/rondo/advances.jar app_process /data/local/tmp Advances /data/local/tmp/rondo/fonts/Roboto-Regular.ttf /data/local/tmp/rondo/advances.bin $sizes"
mkdir -p "$ROOT/native/assets"
adb pull /data/local/tmp/rondo/advances.bin "$ROOT/native/assets/advances.bin" >/dev/null
ls -l "$ROOT/native/assets/advances.bin"
rm -rf "$OUT"
