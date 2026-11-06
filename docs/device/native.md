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
- **Optimization flags:** with CMake 3.21 or newer, NDK r23's toolchain file gives the Release build
  type no `-O` flag at all, so everything builds at `-O0`. `tools/native/build.sh` and
  `tools/native/deps.sh` set `-O2` explicitly. Measured: the UI engine's CPU time per frame went from
  1.8 ms to 0.9 ms, and startup from 250 ms to 53 ms.

## Video

- **Decoding:** the native `NuMediaExtractor` and `MediaCodec` in `libstagefright` (what Java's
  MediaExtractor and MediaCodec wrap) decode H.264 in MediaTek's hardware decoder from a native
  process. Call `DataSource::RegisterDefaultSniffers()` before opening a file.
- **Output:** 640×360 30 fps plays at full rate. The decoder outputs MediaTek's own color format
  (`0x7f000200`).
- **Timestamps:** every output frame comes back with timestamp 0. Take presentation times from the
  queued samples, smallest first, and tell new frames apart by the latched buffer.
- **Drawing:** a `SurfaceTexture` makes the frames GPU textures with no copy; the Mali-400 samples
  MediaTek's format through `samplerExternalOES`.
- **Without SurfaceFlinger:** a `BufferQueue` built with SurfaceFlinger's `GraphicBufferAlloc` (exported
  by `libsurfaceflinger.so`) allocates the buffers in-process. Decoding and drawing straight to the
  framebuffer then needs no SurfaceFlinger service at all.

## Wi-Fi

- **Driver:** libhardware_legacy's `wifi_load_driver` and `wifi_unload_driver` power the combo chip's
  Wi-Fi through MediaTek's **hald** (`/dev/socket/hald`, commands `hal load wifi`, `hal unload wifi`,
  `hal load p2p`, `hal unload p2p`; no sequence numbers).
- **wpa_supplicant:** the firmware's build crashes (SIGSEGV) when started as the wlan0-only
  `wpa_supplicant` service. Start it as `p2p_supplicant` (wlan0 and p2p0), after `hal load p2p`. Its
  control socket is reached with `wifi_connect_to_supplicant("wlan0")`.
- **netd** on 4.2 wants a sequence number before each command (`1 resolver setdefaultif wlan0`). DNS
  comes from `resolver setifdns` and `resolver setdefaultif`. The address and default route are set
  with libnetutils' `ifc_configure` after `dhcp_do_request`.
- **Timing:** a cold start to a working connection took 7.4 s, 4.7 s of it DHCP.

## Power

- **Battery and charger:** `/sys/class/power_supply/battery` has `capacity`, `status`, `batt_vol`
  (mV), `batt_temp` (tenths of °C), `BatteryAverageCurrent`, `ChargerVoltage` and `health`; `ac` and
  `usb` have `online`.
- **Wakelocks:** `/sys/power/wake_lock` and `/sys/power/wake_unlock` (Android's).
- **Backlight:** `/sys/class/leds/lcd-backlight/brightness`, 0–255.
- **Early suspend:** `mem` to `/sys/power/state` starts it; `on` ends it (late resume).
- **The stop-drawing handshake:** while early suspend runs, the kernel waits up to 1 s for the
  program drawing to the screen to stop. The program blocks on `/sys/power/wait_for_fb_sleep`, stops
  drawing when it returns, then reads `/sys/power/wait_for_fb_wake`. That read acknowledges, and
  blocks until late resume allows drawing again.
- **Waking takes 0.6 s,** because the display driver's late resume (`mtkfb_late_resume`) takes about
  540 ms. Keys on `/dev/input/event0` (Power, Center) still arrive during early suspend.

## Bluetooth

- **The stack:** MediaTek's own (not BlueZ or Bluedroid). The `mtkbt` daemon takes "ILM" messages
  (0x81c-byte buffers, ID at +4, payload from +0x20) as datagrams on `/dev/socket/bt.int.adp`, and
  sends replies and events to the abstract datagram socket `bt.ext.adp`, which a native program binds
  once Android's Bluetooth service is gone.
- **The messages verified from a native program:**
  - power on 0x64, answered by 0x91;
  - own address 0x6f, answered by 0x9d;
  - discovery 0x67, with results as 0x93 and the end as 0x95; cancel 0x68, answered by 0x96;
  - bond 0x7e, ending in 0xab with result 0;
  - PIN request 0xa3, answered with 0x75; SSP confirmation request 0xa4, answered with 0x76.
- **Where they come from:** the layouts are read from `libandroid_runtime.so`, which holds MediaTek's
  Bluetooth service and event loop in C.

## FM radio

- **Tuner:** `libfmjni.so` exports MediaTek's `FMR_*` functions, callable directly (power-up and tune
  take MHz × 10, seek MHz × 100).
- **Audio is digital** on this Y1 (`fm_use_analog_input = 0` in `libaudiosetting.so`). Enable it with
  the audio parameter `AudioSetFmDigitalEnable=1`, record from audio source 99 at 44.1 kHz stereo, and
  play that PCM.

## System services a native program must provide

- **"permission":** with system_server gone, AudioFlinger waits forever on `setParameters` while it
  checks `MODIFY_AUDIO_SETTINGS` through the "permission" service. A native program can publish one
  (a BBinder answering `CHECK_PERMISSION_TRANSACTION` with granted). See
  `native/platform/permission_service.h`.
- **A2DP from a native program:**
  - Bind the abstract `bt.ext.adp.a2dp` and pass it to `btmtk_a2dp_setSockAddress`; open an
    auto-bound socket connected to `bt.int.adp`.
  - Call `btmtk_a2dp_send_activate_req`, then `btmtk_a2dp_send_stream_open_req(0, &addr, 1)`.
    `btmtk_a2dp_handle_message` returns 3 when connected and 4 when playing (all in
    `libmtkbtextadpa2dp.so`).
  - Then `AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, available, address)`,
    and `setForceUse(FOR_MEDIA, NONE)`: mediaserver keeps the last "no A2DP" otherwise.
  - Paired devices are in `/data/@btmtk/devdb`.

## Keeping frames on time (measured with the N1 UI engine)

- **The render process's address space must stay quiet.** Page faults and large allocations (anything
  `malloc` serves with `mmap`, 64 KB and up) on any thread of the process that draws take its
  memory-map lock. The Mali driver needs that lock at the start of every frame, inside the first GL
  call. With two threads decoding covers in the same process, `glClear` waited 50 ms to 2 s.
  - Allocating, touching and freeing 2 MB blocks on two threads is enough to cause it, with no I/O and
    no decoding.
  - The same churn in another process costs nothing. So does a CPU hog or an SD-card reader in
    another process.
  - So decoding runs in a helper process with its own address space (`ImageLoader` forks it at
    startup), and pixels come back through shared memory mapped and touched once.
- **Background work runs at `SCHED_IDLE`.** At nice 10 the helper still delayed the display about once
  a minute (a frame's first GL call waited a whole refresh for its buffer). `SCHED_IDLE` lets the
  kernel's display and GPU threads always go first.
- **Glyphs are rasterized before they are needed.** FreeType's light autohinting costs about 0.25 ms
  per glyph. A screen's first frame with 70 new glyphs missed its refresh, so the engine rasterizes the
  common glyphs of every text style while idle, uploads them while idle, and shapes a sample per font
  so HarfBuzz builds its plans early.
- **Only one core is usually online.** The `hotplug` cpufreq governor
  (`/sys/devices/system/cpu/cpufreq/hotplug/`) adds the second core after 300 ms above 90 % load
  (`cpu_up_avg_times` 10 × 30 ms samples) and removes it after 1.5 s. `cpu_num_base` sets the minimum
  number of cores online.
