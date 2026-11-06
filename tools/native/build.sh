#!/usr/bin/env bash
# Builds native/ for the Y1 with NDK r23c (the last NDK that supports Android 4.2) into native/build/y1.
#   tools/native/build.sh [target...]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
NDK=${NDK:-$SDK/ndk/23.2.8568313}
CMAKE=${CMAKE:-$SDK/cmake/3.22.1/bin/cmake}
[[ -d $ROOT/native/sysroot/system-lib ]] || "$ROOT/tools/native/sysroot.sh"
"$CMAKE" -S "$ROOT/native" -B "$ROOT/native/build/y1" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$SDK/cmake/3.22.1/bin/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=android-17 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=${BUILD_TYPE:-Release} \
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" >/dev/null
"$CMAKE" --build "$ROOT/native/build/y1" ${1:+--target "$@"}
