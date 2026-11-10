# Native code on the Y1

What we learned running native programs (C++, NDK r23c, Android API 17) on the Y1 with Android's Java
layer stopped (`adb shell stop zygote`; `start zygote` brings it back). The test programs are in
`native/tests/`, built with `tools/native/build.sh`.

## Display

- **The panel refreshes at 42.14 Hz as it ships** (a vsync every 23.73 ms). Android reports 36.42 Hz
  (`dumpsys display`, `dumpsys SurfaceFlinger`: `refresh=27457442`), which is not what the panel does.
  Rondo 2.0 runs it at **60.00 Hz** (below).
- **The panel:** an ST7701 driven over two MIPI DSI lanes in sync-pulse video mode (the kernel's and
  the bootloader's `st7701_hvga_dsi_vdo_boe`, `lcm=0-st7701_hvga_dsi_vdo_boe` on the command line).
  Its parameters: 360 lines with 8 of sync, 100 of back porch and 100 of front porch; 480 pixels with
  10 + 80 + 80; RGB888; `pll_div1` 2, `pll_div2` 0, `fbk_div` 15 (208 Mb/s a lane, a 26 MHz byte
  clock). The kernel has a second panel driver with 60-line porches (`fbk_div` 13).
- **Registers** (kernel virtual addresses; the physical ones are 0x1400c000 and 0x10011000): the DSI
  at `0xf400c000` (`+0x20..0x2c` VSA, VBP, VFP, VACT lines; `+0x50..0x58` HSA, HBP, HFP byte counts;
  `+0x110..0x11c` the D-PHY timings TIMCON0-3, in byte clocks), the MIPI transmitter at `0xf0011000`
  (`+0x50` PLL_CON0: enable, TXDIV0 in bits 4:3, TXDIV1 in 6:5; `+0x54` PLL_CON1; `+0x58` PLL_CON2:
  the multiplier, integer in bits 30:24 and a 24-bit fraction). Read and write them with
  `echo regr:<addr> > /sys/kernel/debug/dispsys` (the answer is in the kernel log) and `regw:<addr>,<value>`
  after `mount -t debugfs none /sys/kernel/debug`. **mtkfb's own `regr`/`regw` take the same virtual
  addresses but read zeros; a physical address there crashes the kernel** (Nov 9: the Y1 rebooted).
  `echo reg:dsi > /sys/kernel/debug/mtkfb` dumps the DSI and the transmitter to the kernel log.
- **60 Hz:** a shorter front porch alone (VFP 11: 49.97 Hz) shifted the picture: the panel's own
  settings expect 100-line porches. Running the link faster works: TXDIV0 1 (÷2), the multiplier
  43.197 (`PLL_CON2` 0x2b326100) and the D-PHY timings MediaTek's `DSI_PHY_TIMCONFIG` works out for
  it (0x0d050302, 0x140a0308, 0x0d0e0000, 0x00041402): 16.667 ms ± 0.02, about 300 Mb/s a lane. The
  multiplier only takes effect when the PLL is switched on again (PLL_CON0 bit 0). The DSI clock
  moves from 104 MHz (inside the FM band) to 150 MHz; Wi-Fi lost no pings at either rate.
- **The kernel programs the PLL from the panel parameters at every wake** (and the DSI's own
  registers come back from its backup), so a runtime change doesn't last past a deep sleep. On 1.0.x's
  kernel, waking with 60 Hz timings left in place gives the stock PLL plus the fast timings, and the
  link stalls (the driver falls back to a 33 ms software vsync). The 2.0 firmware's kernel has the
  60 Hz values in its panel driver (`tools/firmware/BootImage.java`, eight instruction words).
- **At a cold boot of the 2.0 firmware** the registers already hold those 60 Hz values, but the panel
  refreshes every 13.79 ms (72.5 Hz) until the PLL is switched off and on again: the kernel writes
  the multiplier when it takes the panel over from the bootloader without re-enabling the PLL. After
  the first wake it is 60.00 Hz with the same values. Rondo measures at startup and switches the PLL
  again when it finds 13.79 ms (`native/platform/y1/display.cpp`); every boot since gives 60.00 Hz.
- **Interrupt latency matters at 60 Hz:** the display's blanking is 6.1 ms instead of 8.7. A kernel
  log line on the first core (the battery driver logs every read of `BatteryNotify`) held up the
  display interrupt long enough to miss a refresh; read such files from a thread on the second core.
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
- **A restarted mediaserver is at full volume.** Without Android's AudioService nothing tells the audio
  policy its ranges, so every stream has one step (0 to 1) and step 1 is full volume. Rondo's player
  sets the ranges (`initStreamVolume`, music 0-13) and the saved levels when it starts, but mediaserver
  can restart under it: vold stops every process that has a file open on the card when the card is
  unmounted or shared as a USB drive, and mediaserver had card videos open from stuck frame grabs
  (Nov 10). AudioTrack reconnects to the new mediaserver by itself and played the next song at full
  volume. `AudioSystem::setErrorCallback` reports the death (`DEAD_OBJECT`); Rondo's output plays
  silence from then until the new mediaserver has the ranges and the levels again
  (`player/engine.cpp`, `audioLost`). `dumpsys media.audio_policy` shows each stream's range and
  index per output.
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
- **What Android 4.2 set** (`framework.odex`'s WifiNative, Nov 9): `SCAN_INTERVAL 15` (the
  supplicant's own default is 5 s), IPv6 multicast let through (`DRIVER RXFILTER-STOP`, `-ADD 3`,
  `-START`) and IPv4 multicast filtered (`-STOP`, `-REMOVE 2`, `-START`), `SET ps 1` (0 while DHCP
  runs), and `DRIVER SETSUSPENDMODE 1` with the screen off, which MediaTek's driver refuses: its own
  early-suspend handler does that part. All the others answer OK.
- **`SET ps 1` only after DHCP:** set before joining a network, the first DHCP after the join got its
  offer and then no answer to its requests for 30 s (every boot of the 2.0 firmware, Nov 9). The
  next DHCP 10 s later was answered at once. With power save switched on after DHCP, as Android's
  `handlePostDhcpSetup` did, the first DHCP takes 3.5 s.
- **`net.hostname` must be set before DHCP.** MediaTek's `libnetutils.so` builds dhcpcd's start
  command as `%s_%s:-f %s -h %s %s` with a hostname, and without one as `%s_%s:-f %s %s` with its
  arguments in the wrong order: `dhcpcd_/system/etc/dhcpcd/dhcpcd.conf:-f wlan0 wlan0`, which init has
  no service for (`init: no such service` in the kernel log), so `dhcp_do_request` times out
  ("Timed out waiting for dhcpcd to start"). Android's ConnectivityService set it at every boot
  (`android-` and the secure setting `android_id`). Without Android, Rondo sets it the same way.
  (`/system/etc/dhcpcd/dhcpcd.conf` doesn't exist; dhcpcd runs on its defaults.)
- **No MAC address in the Wi-Fi NVRAM as shipped:** bytes 4-9 of `/data/nvram/APCFG/APRDEB/WIFI` are
  zeros, so the driver made up an address under MediaTek's `00:08:22` at every load (six boots of 2.0
  on Nov 9 gave five different ones), and MediaTek listed a fake network, `NVRAM WARNING: Err = 0x10`,
  in the scan results. Since Nov 9 Rondo fills the empty address in once, before the driver loads,
  with a locally administered one (`02:` and five bytes from a hash of the eMMC's CID, so the same
  Y1 gets the same address after any reflash). The driver takes it at its next load, the fake
  network is gone, and the router gives the same lease at every boot. The record as it was is kept in
  `/data/rondo/nvram-WIFI.orig`. A lease saved for another MAC is still deleted before DHCP (it
  earns a NAK).
- **MediaTek's NVRAM files** (`/data/nvram/APCFG/APRDEB/*`, owner `nvram`, mode 0660) end in `0xAA` and
  a check byte over the record before it: from 0, the even-index bytes added and the odd-index bytes
  XORed, in turn (`WIFI` 512 + 2 bytes, `BT_Addr` 30 + 2, `WIFI_CUSTOM` 4 + 2). `WIFI`: two version
  words, the MAC address at 4-9.
- **Time zone:** bionic's `localtime` reads `TZ`, else `persist.sys.timezone`, at each call, so setting
  the property changes the time zone at once in every process, and it outlasts a reboot. The stock
  firmware ships `Asia/Shanghai`.
- **Time zone data:** bionic 4.2 reads only `/system/usr/share/zoneinfo/zoneinfo.idx` and `.dat`
  (no `/data/misc/zoneinfo`). The base firmware's are tzdata 2012h, whose rules are long out of date:
  Moscow is +4 instead of +3, Istanbul +2 instead of +3, and Mexico City still switches to
  summer time. The format: `zoneinfo.dat` is the zones' TZif files one after another;
  `zoneinfo.idx` is a 52-byte entry for each zone, sorted by name (40 bytes of name, then big-endian
  int32s: start in the .dat, length, standard offset in ms); `zoneinfo.version` is "2012h\n". bionic
  reads the TZif's version-1 data (32-bit `time_t`), so zic must write "fat" files (`-b fat`; slim
  ones leave it empty). `tools/firmware/zoneinfo.mjs` packs zic's output; `firmware/zoneinfo` holds
  2026f (597 zones), which `firmware/build.sh` puts in. IANA has since dropped invented
  abbreviations, so `date` shows a number such as `+03` where 2012h had a name.
- **The RTC:** `ioctl(/dev/alarm, ANDROID_ALARM_SET_RTC)` (Android's AlarmManagerService's way) sets
  MediaTek's clock chip, which keeps counting while the Y1 is off; the kernel reads it at boot
  (`/sys/class/rtc/rtc0/time` shows it, in UTC). A time set by hand survives a restart and a power-off.

## Power

- **Shutting down and restarting:** this firmware's `init.rc` has no trigger for `sys.powerctl`
  (Android 4.2's Java layer called libcutils' `android_reboot` itself), so setting it does nothing.
  Rondo unmounts the card and the internal storage through vold, then calls
  `android_reboot(0xDEAD0002 /* power off */ or 0xDEAD0001 /* restart */, 0, NULL)`, which syncs,
  remounts everything read-only and calls `reboot(2)` (`platform/y1/power.cpp`). Switched off on a
  USB cable, the Y1 enters MediaTek's power-off charging: a press shows the charge picture, holding
  the center button about 3 s starts it.
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
- **The CPU without Android's framework** (Nov 9): after a cold boot `cpu_hotplug_disable` is 1 (the
  second core stays off) and `/proc/cpufreq/cpufreq_state` says DVFS is off with `usb_raise_freq`
  pinning 1.0 GHz. With `cpu_hotplug_disable` 0, `cpufreq_state` 1 and `usb_raise_freq` 0 the clock
  runs 598 MHz idle and 1.0 GHz busy and the governor adds the second core under load. Taking the
  second core out or back in stalls the whole system 10–25 ms (a missed refresh in a slide), so Rondo
  keeps `cpu_num_base` at 2 while the screen is lit. The GPU has one operating point (416 MHz).
- **MediaTek's safety signals:** the thermal policy (init's `thermal_manager`) sends SIGIO with
  `si_code` 1 to the pid written to `/proc/driver/mtk_cl_sd_pid` when the chip must cool down (write
  1 to `/proc/driver/mtk_cl_sd_rst` at boot first); the battery driver's
  `/sys/devices/platform/mt-battery/BatteryNotify` holds 1 (charger over voltage), 2 (battery too
  hot), 4 (over current), 8 (battery over voltage) or 16 (charging too long). MediaTek's daemons read
  them for Android's warning dialogs; without Android, Rondo does.

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
- **Storage:** vold mounts nothing by itself; Android's MountService asked it to. On the 2.0
  firmware vold starts listening, and then lists its volumes (`vdc volume list`: the card
  `/storage/sdcard0`, vold's `sdcard2`, and the internal storage `/storage/sdcard1`), a few seconds
  into the boot. A boot script that asks once can find no answer yet, or the volumes still
  initializing (both happened on Nov 9: the card stayed unmounted and the Library was empty).
  `firmware/rondo/start` asks until each is mounted or 20 s have passed, in the background. Rondo also
  mounts any volume vold lists as idle every 2 s (a card put in later), except while it hands the
  card to a computer. A Y1 started on a computer's cable must still mount its card: the USB drive
  prompt only offers sharing it.
- **net.hostname** (ConnectivityService's job) is needed before DHCP; see Wi-Fi.
- **vold tells its clients:** every client of `/dev/socket/vold` receives its broadcasts, such as
  `605 Volume sdcard2 /storage/sdcard0 state changed from 0 (No-Media) to 1 (Idle-Unmounted)`. Rondo
  mounts on those instead of asking `volume list` every few seconds (asking every 2 s woke vold and
  the CPU all the time).
- **mediaserver's frame retriever can hang:** on the 2.0 firmware a native
  `MediaMetadataRetriever.getFrameAtTime` call for a card video sometimes never returns (Nov 9: two
  calls waiting in binder, the videos' files open in rondo-media). Rondo grabs a video's frame on a
  thread of its own and gives up after 8 s, so the other artwork keeps coming. Why mediaserver hangs
  isn't known yet.

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
- **Full-size decodes happen before they are needed.** A large cover decoded on demand can still cost
  one refresh. Covers are decoded once into a thumbnail cache at the sizes the screens use, by
  prefetches that run only after 1.5 s without movement. With the cache full, a full-speed spin
  through 821 songs misses no refresh.
- **Datagram queues are short:** AF_UNIX `SOCK_DGRAM` holds 10 messages (`max_dgram_qlen`). Send in
  batches as room appears.
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
