#!/usr/bin/env bash
# Builds direct mode's code for the Mac (native/direct, with the Mac's own libcurl) into
# native/build/host/dcli: one route answered and printed, or the local server with --serve <port>.
#   tools/native/direct-host.sh && native/build/host/dcli '/music/search?q=tarkan&type=song'
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/native/build/host
YY=$ROOT/native/third_party/src/yyjson-0.12.0/src
mkdir -p "$OUT"
[[ $OUT/yyjson.o -nt $YY/yyjson.c ]] || clang -O2 -c "$YY/yyjson.c" -o "$OUT/yyjson.o"
clang++ -std=c++17 -O1 -g -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -I"$YY" \
  "$ROOT"/native/direct/*.cpp "$ROOT/native/core/fold.cpp" "$ROOT/native/direct/host/dcli.cpp" "$OUT/yyjson.o" -lcurl -o "$OUT/dcli"
echo "$OUT/dcli"
