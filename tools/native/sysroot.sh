  # the pull stops at that error, so the folders after it come separately
  for d in hw egl; do "$ADB" pull /system/lib/$d "$OUT/system-lib/$d" >/dev/null 2>&1 || true; done
#!/usr/bin/env bash
# Fills native/sysroot/ for the native build:
#   system-lib/  the Y1's own /system/lib, pulled over adb from a Y1 running Rondo's firmware, so the
#                native code links against the exact libraries it will run with (SurfaceFlinger's client
#                library, binder, EGL, OpenSL ES, the media libraries)
#   aosp/        the Android 4.2.2 (android-4.2.2_r1) headers of the private libraries we use
# Nothing in native/sysroot/ is committed: the libraries are the firmware's binaries (Google's and
# MediaTek's), and the headers are fetched again from AOSP whenever this runs.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/native/sysroot
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
TAG=android-4.2.2_r1

mkdir -p "$OUT/aosp"
if [[ ! -d $OUT/system-lib || ${1:-} == --refresh ]]; then
  rm -rf "$OUT/system-lib"
  # a dangling symlink (modules/wlan.ko) makes adb pull report an error at the end; what matters is checked below
  "$ADB" pull /system/lib "$OUT/system-lib" >/dev/null 2>&1 || true
fi
for f in libgui.so libui.so libbinder.so libutils.so libcutils.so liblog.so libEGL.so libGLESv2.so libOpenSLES.so libmedia.so libstagefright.so libhardware.so; do
  [[ -f $OUT/system-lib/$f ]] || { echo "sysroot: $f missing from the pulled /system/lib" >&2; exit 1; }
done
echo "system-lib: $(ls "$OUT/system-lib" | wc -l | tr -d ' ') files"

# <repo path>:<folder in the repo>:<name here> (config: the AndroidConfig.h every 4.2.2 library was built with)
for spec in frameworks/native:include:native system/core:include:core \
            hardware/libhardware:include:hardware frameworks/av:include:av \
            hardware/libhardware_legacy:include:hardware_legacy \
            build:core/combo/include/arch/linux-arm:config; do
  IFS=: read -r repo dir name <<< "$spec"
  [[ -d $OUT/aosp/$name ]] && continue
  mkdir -p "$OUT/aosp/$name.part"
  curl -fsSL "https://android.googlesource.com/platform/$repo/+archive/refs/tags/$TAG/$dir.tar.gz" |
    tar -xz -C "$OUT/aosp/$name.part"
  mv "$OUT/aosp/$name.part" "$OUT/aosp/$name"
done
echo "aosp: $(ls "$OUT/aosp" | tr '\n' ' ')"
