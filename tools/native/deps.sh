#!/usr/bin/env bash
# Downloads and builds the native third-party libraries for the Y1 (armeabi-v7a with NEON, API 17,
# static, NDK r23c) into native/third_party/y1/{include,lib}. Release flags are set explicitly: with CMake 3.21+
# the NDK r23 toolchain file gives Release no -O flag at all. Sources come from each project's official
# release and are checked against tools/native/deps.lock; a library missing from the lock file has its
# SHA-256 recorded on first download (review the change before committing it).
#
#   tools/native/deps.sh [name...]      default: all, in dependency order
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
NDK=${NDK:-$SDK/ndk/23.2.8568313}
CMAKE=${CMAKE:-$SDK/cmake/3.22.1/bin/cmake}
NINJA=$SDK/cmake/3.22.1/bin/ninja
TP=$ROOT/native/third_party
SRC=$TP/src
OUT=$TP/y1
LOCK=$ROOT/tools/native/deps.lock
mkdir -p "$SRC" "$OUT"
touch "$LOCK"

# name version url
LIBS=(
  "libpng 1.6.47 https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz"
  "libjpeg-turbo 3.1.0 https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz"
  "freetype 2.13.3 https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"
  "harfbuzz 10.2.0 https://github.com/harfbuzz/harfbuzz/releases/download/10.2.0/harfbuzz-10.2.0.tar.xz"
)

ANDROID_ARGS=(-G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA"
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI=armeabi-v7a -DANDROID_ARM_NEON=ON -DANDROID_PLATFORM=android-17
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
  -DCMAKE_INSTALL_PREFIX="$OUT" -DCMAKE_PREFIX_PATH="$OUT" -DCMAKE_FIND_ROOT_PATH="$OUT")

fetch() {  # name version url -> unpacked source directory on stdout
  local name=$1 version=$2 url=$3 file sum want dir
  file=$SRC/$name-$version.${url##*.}
  [[ $url == *.tar.gz ]] && file=$SRC/$name-$version.tar.gz
  [[ $url == *.tar.xz ]] && file=$SRC/$name-$version.tar.xz
  [[ -f $file ]] || curl -fsSL --retry 3 -o "$file" "$url"
  sum=$(shasum -a 256 "$file" | cut -d' ' -f1)
  want=$(awk -v n="$name" -v v="$version" '$1 == n && $2 == v { print $3 }' "$LOCK")
  if [[ -z $want ]]; then
    echo "$name $version $sum" >> "$LOCK"
    echo "deps: recorded $name $version $sum" >&2
  elif [[ $want != "$sum" ]]; then
    echo "deps: $name $version checksum mismatch ($sum, lock says $want)" >&2
    exit 1
  fi
  dir=$SRC/$name-$version
  if [[ ! -d $dir ]]; then
    mkdir -p "$dir.part"
    tar -xf "$file" -C "$dir.part" --strip-components 1
    mv "$dir.part" "$dir"
  fi
  echo "$dir"
}

build() {  # name source-dir cmake-args...
  local name=$1 dir=$2
  shift 2
  "$CMAKE" -S "$dir" -B "$TP/build/$name" "${ANDROID_ARGS[@]}" "$@" >/dev/null
  "$CMAKE" --build "$TP/build/$name" --target install >/dev/null
  echo "deps: $name built"
}

want=("$@")
for spec in "${LIBS[@]}"; do
  read -r name version url <<< "$spec"
  if [[ ${#want[@]} -gt 0 && ! " ${want[*]} " == *" $name "* ]]; then continue; fi
  dir=$(fetch "$name" "$version" "$url")
  case $name in
    libpng) build libpng "$dir" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
              -DPNG_ARM_NEON=on ;;
    libjpeg-turbo) build libjpeg-turbo "$dir" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=ON \
              -DWITH_TURBOJPEG=ON -DWITH_JAVA=OFF ;;
    freetype) build freetype "$dir" -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_BROTLI=ON \
              -DFT_DISABLE_HARFBUZZ=ON -DFT_REQUIRE_PNG=OFF -DFT_DISABLE_PNG=ON -DFT_REQUIRE_ZLIB=ON ;;
    harfbuzz) build harfbuzz "$dir" -DHB_HAVE_FREETYPE=ON -DHB_BUILD_SUBSET=OFF -DHB_BUILD_UTILS=OFF \
              -DFREETYPE_INCLUDE_DIRS="$OUT/include/freetype2" -DFREETYPE_LIBRARY="$OUT/lib/libfreetype.a" ;;
  esac
done
ls "$OUT/lib"
