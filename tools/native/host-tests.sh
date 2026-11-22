#!/usr/bin/env bash
# Builds and runs native/tests/host/ on the Mac (clang, the Mac's libcurl; tag_write_test also uses
# ffmpeg when it's on the PATH), then the language check. Each test's own build line is at its top.
#   tools/native/host-tests.sh
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
Y=native/third_party/src/yyjson-0.12.0/src
clang -c "$Y/yyjson.c" -o "$OUT/yy.o" || exit 1
failed=0
run() {  # name, then the sources and flags after clang++ -std=c++17
  local name=$1; shift
  if clang++ -std=c++17 -w "$@" -o "$OUT/$name" && "$OUT/$name" >"$OUT/$name.log" 2>&1; then
    echo "ok    $name"
  else
    echo "FAIL  $name"; tail -20 "$OUT/$name.log" 2>/dev/null; failed=$((failed + 1))
  fi
}
run dsp_test -O2 -I native/player native/tests/host/dsp_test.cpp native/player/dsp.cpp
run loudness_test -O2 -I native/player native/tests/host/loudness_test.cpp native/player/loudness.cpp
run lang_test -I native -I $Y native/core/lang.cpp "$OUT/yy.o" native/tests/host/lang_test.cpp
run organize_test -I native -I $Y native/core/organize.cpp native/core/fold.cpp native/core/lang.cpp "$OUT/yy.o" native/tests/host/organize_test.cpp
run subtitle_test -I native -I $Y native/core/subtitle_file.cpp native/core/lang.cpp "$OUT/yy.o" native/tests/host/subtitle_test.cpp
run tag_write_test -Dpread64=pread -Dlseek64=lseek -Doff64_t=off_t -I native -I $Y native/core/tag_write.cpp native/core/tags.cpp \
  native/core/lyrics.cpp native/core/fold.cpp native/core/lang.cpp "$OUT/yy.o" native/tests/host/tag_write_test.cpp
run api_method_test -Dpthread_cond_timedwait_monotonic_np=pthread_cond_timedwait -I native/core -I $Y native/core/api.cpp \
  native/core/net.cpp native/tests/host/api_method_test.cpp "$OUT/yy.o" -lcurl
run lookup_test -I native -I $Y native/core/lookup.cpp native/core/fold.cpp "$OUT/yy.o" native/tests/host/lookup_test.cpp -lcurl
if node tools/lang/check.mjs >"$OUT/lang.log" 2>&1; then echo "ok    lang/ (tools/lang/check.mjs)"; else echo "FAIL  lang/"; tail -20 "$OUT/lang.log"; failed=$((failed + 1)); fi
echo "$failed failed"
exit $failed
