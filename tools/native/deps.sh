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
  "sqlite 3.50.4 https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip"
  "yyjson 0.12.0 https://github.com/ibireme/yyjson/archive/refs/tags/0.12.0.tar.gz"
  "openssl 3.5.4 https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz"
  "nghttp2 1.67.1 https://github.com/nghttp2/nghttp2/releases/download/v1.67.1/nghttp2-1.67.1.tar.xz"
  "curl 8.16.0 https://curl.se/download/curl-8.16.0.tar.xz"
  "ffmpeg 7.1.2 https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz"
)
BIN=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin

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
  [[ $url == *.zip ]] && file=$SRC/$name-$version.zip
  [[ $url == *.tar.xz ]] && file=$SRC/$name-$version.tar.xz
  # the network here refuses connections now and then: retry those too
  local try=0
  until [[ -f $file ]] || { curl -fsSL --retry 3 --retry-connrefused -o "$file.part" "$url" && mv "$file.part" "$file"; }; do
    (( ++try < 30 )) || { echo "deps: cannot download $url" >&2; exit 1; }
    sleep 3
  done
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
    rm -rf "$dir.part"
    mkdir -p "$dir.part"
    if [[ $file == *.zip ]]; then  # bsdtar fails on this zip's entries; unzip, then lift the one folder
      unzip -q "$file" -d "$dir.part" && mv "$dir.part"/*/* "$dir.part"/
    else
      tar -xf "$file" -C "$dir.part" --strip-components 1
    fi
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
    sqlite)  # the amalgamation: one C file
      "$BIN/armv7a-linux-androideabi17-clang" -O2 -DNDEBUG -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
        -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_ENABLE_FTS5 -c "$dir/sqlite3.c" -o "$TP/sqlite3.o"
      "$BIN/llvm-ar" rcs "$OUT/lib/libsqlite3.a" "$TP/sqlite3.o"
      cp "$dir/sqlite3.h" "$OUT/include/"
      echo "deps: sqlite built" ;;
    yyjson) build yyjson "$dir" -DYYJSON_BUILD_TESTS=OFF ;;
    openssl)  # its own Configure; android-arm at API 17, static
      (cd "$dir" && export ANDROID_NDK_ROOT="$NDK" PATH="$BIN:$PATH" &&
        ./Configure android-arm -D__ANDROID_API__=17 no-shared no-tests no-docs no-apps no-engine no-module \
          --prefix="$OUT" --libdir=lib -O2 >/dev/null && make -j8 build_libs >/dev/null 2>&1 && make install_dev >/dev/null)
      echo "deps: openssl built" ;;
    nghttp2) build nghttp2 "$dir" -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF -DENABLE_DOC=OFF ;;
    curl) build curl "$dir" -DBUILD_CURL_EXE=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
            -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR="$OUT" -DOPENSSL_USE_STATIC_LIBS=ON -DUSE_NGHTTP2=ON \
            -DNGHTTP2_INCLUDE_DIR="$OUT/include" -DNGHTTP2_LIBRARY="$OUT/lib/libnghttp2.a" -DCURL_USE_LIBPSL=OFF \
            -DCURL_USE_LIBSSH2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON \
            -DENABLE_UNIX_SOCKETS=ON -DBUILD_TESTING=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none ;;
    ffmpeg)  # LGPL, shared libraries (relinkable), audio decoders and the demuxers the card and server use
      (cd "$dir" && ./configure --prefix="$OUT" --target-os=android --arch=arm --cpu=cortex-a7 --enable-cross-compile \
        --cc="$BIN/armv7a-linux-androideabi17-clang" --cxx="$BIN/armv7a-linux-androideabi17-clang++" \
        --ar="$BIN/llvm-ar" --ranlib="$BIN/llvm-ranlib" --nm="$BIN/llvm-nm" --strip="$BIN/llvm-strip" \
        --enable-shared --disable-static --enable-pic --enable-neon --disable-programs --disable-doc \
        --disable-avdevice --disable-swscale --disable-postproc --disable-avfilter --disable-network \
        --disable-everything --enable-swresample --enable-protocol=file \
        --enable-demuxer=flac,mp3,mov,ogg,wav,aac,matroska \
        --enable-decoder=flac,mp3,mp3float,aac,aac_latm,vorbis,opus,alac,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8 \
        --enable-parser=flac,mpegaudio,aac,vorbis,opus,h264 --enable-bsf=h264_mp4toannexb \
        --extra-cflags="-O2" >/dev/null && make -j8 >/dev/null 2>&1 && make install >/dev/null)
      echo "deps: ffmpeg built" ;;
    harfbuzz) build harfbuzz "$dir" -DHB_HAVE_FREETYPE=ON -DHB_BUILD_SUBSET=OFF -DHB_BUILD_UTILS=OFF \
              -DFREETYPE_INCLUDE_DIRS="$OUT/include/freetype2" -DFREETYPE_LIBRARY="$OUT/lib/libfreetype.a" ;;
  esac
done
ls "$OUT/lib"
