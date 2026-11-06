# Native code on the Y1

What we learned running native programs (C++, NDK r23c, Android API 17) on the Y1 with Android's Java
layer stopped (`adb shell stop zygote`; `start zygote` brings it back). The test programs are in
`native/tests/`, built with `tools/native/build.sh`.

## Display

- **The panel refreshes at 42.14 Hz** (a vsync every 23.73 ms). Android reports 36.42 Hz
  (`dumpsys display`, `dumpsys SurfaceFlinger`: `refresh=27457442`), which is not what the panel does.
- **Hardware vsync:** `ioctl(open("/dev/mtkfb_vsync"), 0x5601)` returns at the next vsync. This is how
  MediaTek's hardware composer (`hwcomposer.mt6572.so`, `VSyncThread::threadLoop`) waits. Two waiters
  on it at once disturb each other.
- **While the display driver is in early suspend,** the wait returns at once (`[MTKFB_VSYNC]:mtkfb has
  suspend, return directly` in the kernel log). SurfaceFlinger then paces at 100 ms, or at a 30 ms
  software vsync when nothing is composed. Writing `on` to `/sys/power/state` wakes the driver, which
  Android's power manager normally does when the screen turns on.
- **Without Android's window manager,** the display has no layer stack (`layerStack=4294967295`,
  `numLayers=0` in `dumpsys SurfaceFlinger`), so nothing appears. A native program sets it:
  `SurfaceComposerClient::setDisplayLayerStack(display, 0)` and `setDisplayProjection`.
- SurfaceFlinger restarts the boot animation when system_server dies; `stop bootanim` ends it.
- **Latency:**
  - Through SurfaceFlinger a queued frame is on screen about 68 ms (2.9 refreshes) later.
  - Drawn straight to the framebuffer (libui's `android_createDisplaySurface()`, SurfaceFlinger
    stopped), it shows at the next vsync.
  - In both, a simple GLES2 scene ran at every refresh without a miss.
- GPU: Mali-400 MP, OpenGL ES 2.0, EGL 1.4 (`Linux-r3p2-01rel0`). Extensions include
  `OES_EGL_image_external`, ETC1, NPOT textures, `EXT_discard_framebuffer` and
  `EXT_multisampled_render_to_texture`.

## Sound

- OpenSL ES works from a native process with only mediaserver running: no underruns, real-time
  playback.
- MediaTek's mixer uses 1024-frame periods at 44.1 kHz (23.2 ms). A normal track gets a 4096-frame
  buffer (92.9 ms).

## Building against the firmware's libraries

- **Exports:** the Y1's `libgui.so` exports Android 4.2.2's `SurfaceComposerClient`, `SurfaceControl`
  and `DisplayEventReceiver` with AOSP's signatures. MediaTek added `setFlagsEx`.
- **`char16_t` and `char32_t`:** AOSP 4.2.2's headers define them as `uint16_t` and `uint32_t`
  typedefs, and the firmware's exported names use those types. Code built as C++11 or newer maps the
  names while including those headers (`native/platform/aosp_begin.h`).
- **Build configuration:** the libraries were built with `AndroidConfig.h` from
  `build/core/combo/include/arch/linux-arm`, which defines `HAVE_PTHREADS` and friends. Include it
  first (`tools/native/sysroot.sh` fetches it).
- **`adb pull /system/lib`** stops at a dangling symlink, `modules/wlan.ko`. Pull `hw/` and `egl/`
  separately.
