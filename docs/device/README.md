# Notes on the Innioasis Y1

What we learned about the Y1 while building Rondo, on a Y1 running the community's
"Android 4.2 (Dev)" firmware. Each fact comes from a file in this folder or from the command named
next to it. If you're writing your own software for the Y1, start here.

Rondo 1.0 was an Android app, so much of this page is about Android 4.2's framework on the Y1, and
the files named here (`.kt`) are 1.0's, in `app/`. Rondo 2.0 runs without Android's Java layer:
what it does instead, and what that took, is in [native code on the Y1](native.md).

**The wheel in one sentence:** the click wheel is plain key events, not an axis or a touch ring.
Every detent sends an instant press and release on `/dev/input/event2` ("mtk-tpd"): clockwise
`KEY_DOWN` (108 → `DPAD_RIGHT`) and counter-clockwise `KEY_UP` (103 → `DPAD_LEFT`), about two
pulses per felt detent. On the same device, Menu sends `KEY_BACK` (158 → `BACK`), Play/Pause
`KEY_PLAYPAUSE` (164 → `MEDIA_PLAY_PAUSE`), Next `KEY_RIGHT` (106 → `MEDIA_NEXT`) and Prev
`KEY_LEFT` (105 → `MEDIA_PREVIOUS`). Center sends `KEY_REPLY` (232 → `ENTER`) on `/dev/input/event0`
("mtk-kpd"). Every device uses `Generic.kl`, a copy of the stock `Stock.kl`.

## Input

| Physical control | Input device | Linux key (scancode) | Android keycode (`Generic.kl`) |
| --- | --- | --- | --- |
| Wheel clockwise | event2 `mtk-tpd` | `KEY_DOWN` (108) | `DPAD_RIGHT` (22) |
| Wheel counter-clockwise | event2 `mtk-tpd` | `KEY_UP` (103) | `DPAD_LEFT` (21) |
| Center | event0 `mtk-kpd` | `KEY_REPLY` (232) | `ENTER` (66), flagged `WAKE_DROPPED` |
| Menu (top) | event2 `mtk-tpd` | `KEY_BACK` (158) | `BACK` (4) |
| Play/Pause (bottom) | event2 `mtk-tpd` | `KEY_PLAYPAUSE` (164) | `MEDIA_PLAY_PAUSE` (85) |
| Next (right) | event2 `mtk-tpd` | `KEY_RIGHT` (106) | `MEDIA_NEXT` (87) |
| Prev (left) | event2 `mtk-tpd` | `KEY_LEFT` (105) | `MEDIA_PREVIOUS` (88) |
| Power | event0 `mtk-kpd` | `KEY_POWER` (116) | `POWER`, flagged `WAKE` |

- **Wheel timing:** a detent's down and up share one timestamp, so the wheel has no hold or
  auto-repeat. 3 counted clockwise clicks gave 6 pulses (`getevent-l-3-clockwise-3-clicks.txt`),
  and 5 gave 10. A fast spin sends pulses as little as about 40 ms apart (`getevent-l-2.txt`).
  Buttons are real presses, 40–170 ms between down and up.
- **One step per pulse, like Rockbox:** counting one step per felt click (ignoring the second
  pulse) felt slow. Rockbox's Y1 port ([multiwirth/rockbox-y1](https://github.com/multiwirth/rockbox-y1),
  `firmware/target/hosted/android/button-android.c`) posts a button for every key event, so two
  steps per click, and buzzes the vibrator on each: `duration_mapping[intensity]` (5 ms per level,
  so level 5 = 25 ms) plus 20 / 8 / 2 / 0 ms as a spin goes on (pulses under HZ/6.5 ≈ 154 ms
  apart). Rondo does the same (1.0: `input/WheelInput.kt`, `input/WheelHaptics.kt`; 2.0 reads
  `/dev/input` itself, `native/platform/y1/input.cpp`). Measured on 1.0 with injected pulses: a
  pulse reaches the app in about 25 ms when idle and 50–85 ms while a video plays; a volume step
  takes 4–9 ms.
- **Vibrator:** `/sys/class/timed_output/vibrator`. The MTK driver logs each request in `dmesg`
  ("vibrator first in value = N") and runs for at least 60 ms ("cust timer: 60"). `adb shell` is
  root on this firmware, so `sendevent`, or writing `input_event` structs to `/dev/input/event2`,
  injects real wheel pulses for tests.
- **The wheel's click sound comes from the system UI.** With no app sound, a wheel pulse still
  plays a short tick from `com.android.systemui` (Innioasis' addition, in every app) while
  `Settings.System.SOUND_EFFECTS_ENABLED` is 1; setting it to 0 silences it at once. Rondo 1.0's
  **Click sound** setting writes this switch. The Rondo firmware has no SystemUI, so Rondo plays
  the tick itself there (2.0: `native/player/click.cpp`; see "The Rondo firmware" below).
- **MediaTek's sound profiles put it back at every boot** (Android's framework, so 1.0 only). The
  `audioprofile` service
  (`com.mediatek.audioprofile.AudioProfileManager`, profiles General, Silent, Meeting and Outdoor;
  `mtk_audioprofile_active` is `mtk_audioprofile_general`) keeps its own "sound effects" value per
  profile, defaulting to on for General, and copies it into `SOUND_EFFECTS_ENABLED` when the Y1
  starts. Writing the setting alone lasts until the next restart; Rondo also calls
  `setSoundEffectEnabled(getActiveProfileKey(), on)` on the manager
  (`context.getSystemService("audioprofile")`, by reflection).
- **What Android thinks the wheel is:** `dumpsys input` lists `mtk-tpd` with classes `0x15`,
  keyboard plus multi-touch screen, and a 480×360 touch axis that never fires. `mtk-tpd-kpd`
  (event3) declares the same keys but stays silent. `ACCDET` (event1) is headset-jack detection
  and uses `ACCDET.kl`.
- **Key layout files:** `Generic.kl` is loaded for `mtk-tpd`, `mtk-kpd`, `mtk-tpd-kpd` and the
  virtual keyboard (`dumpsys-input.txt`). A `mtk-kpd.kl` exists, yet Android doesn't load it for
  `mtk-kpd` (vendor `0x2454`, product `0x6572`); why isn't known. So changing `Generic.kl` changes
  every button.
- **Wake keys:** only Power (`WAKE`) and Center (`WAKE_DROPPED`) carry wake flags. The wheel and
  the other buttons don't wake the screen.
- **Android's own lists ignore the wheel.** `DPAD_LEFT/RIGHT` don't move the focus in Android's
  vertical Settings lists or in the "Complete action using" dialog, and the Wi-Fi password dialog
  needs the touch keyboard. Rondo has its own Wi-Fi and Bluetooth screens: in 1.0, joining uses
  the hidden `WifiManager.connect(config|netId, listener)`, and pairing and A2DP use the hidden
  `createBond`, `BluetoothA2dp.connect/disconnect` and `removeBond`, by reflection. 2.0 talks to
  wpa_supplicant and MediaTek's Bluetooth daemon itself ([native.md](native.md#wi-fi)).

### How other launchers read it

JJ Launcher ([ismileblue/y1_launcher](https://github.com/ismileblue/y1_launcher)) and its fork
Inniclassic ([FabianZettl/inniclassic](https://github.com/FabianZettl/inniclassic)), both Java,
use only standard `KeyEvent`s; neither reads `/dev/input` or uses motion events.

- Wheel: keycode `21` = up / previous, `22` = down / next. In Now Playing the wheel changes the
  volume, with 22 = louder.
- Center: `DPAD_CENTER` or `ENTER`. Back: `BACK`, and `19` (`DPAD_UP`) as the "top button".
- Debounce: JJ drops wheel steps under 40 ms apart, Inniclassic under 15 ms.
- Long press: from auto-repeat after 500 ms (JJ), or an 800 ms timer (Inniclassic).
- Play/Pause arrives twice: once through the activity and once through a registered
  `MEDIA_BUTTON` receiver. The stock app also registers `PlayControllerReceiver` for
  `MEDIA_BUTTON`.

## Screen

480 × 360 px at density 160, so the `mdpi` bucket with 1 dp = 1 px, but the panel is about
250 ppi, so mdpi sizes come out tiny. Rondo scales everything sized with `dp()` (1.4× by
default). Android reports 36.43 fps, but the panel's hardware vsync comes every 23.73 ms, so it really refreshes at 42.14 Hz; Rondo 2.0 runs it at 60 Hz ([native code on the Y1](native.md#display)). Sources: `dumpsys-display.txt`, `lcd-density.txt`.
`wm` doesn't exist on 4.2 (`wm-size.txt`, `wm-density.txt`).

## Software

- **Firmware:** "Android 4.2 (Dev)" from the [y1-community](https://github.com/y1-community),
  installed with Innioasis Updater. It writes every partition, including userdata.
  - Android 4.2.2 / API 17, display id `Y1_001`, kernel 3.4.5 (`proc-version.txt`), MediaTek
    `ALPS.JB3.MP`, signed with test keys.
  - `ro.secure=0` and `ro.debuggable=1`: adbd runs as root, and `/system/xbin/su` is setuid
    root. USB mode is `mass_storage,adb`.
  - It ships Rockbox as a system app (`/system/app/org.rockbox.apk`) with a "dual boot" script
    (`/system/etc/init.d/99Y1ButtonScript`): holding Back + Play for 10 s enables Rockbox,
    disables the stock UI, copies `Rockbox.kl` over `Generic.kl` and restarts.
- **Stock UI:** package `com.innioasis.y1` (FR_2.7.2), in `/data/app`. Its home activity is
  `com.innioasis.y1/.activity.SplashActivity`: MAIN + HOME but no LAUNCHER, so
  `getLaunchIntentForPackage` returns null and it has to be opened by component. Menu does
  nothing at its top level, so the way out is a restart. It runs as the system user.
- **Home screen (1.0):** Rondo's `ShellActivity` declares HOME + DEFAULT and is made the default
  with the "Complete action using" dialog (Rondo, then Always). Uninstalling Rondo brings the
  dialog back on the next HOME.
- **Settings and Bluetooth:** `com.android.settings` has `WifiSettingsActivity`,
  `BluetoothSettingsActivity`, `DateTimeSettingsActivity`, `DisplaySettingsActivity` and
  `AudioProfileSettingsActivity`. The Bluetooth stack is `com.mediatek.bluetooth`. There's no
  WebView browser and no Google services. LatinIME is the only keyboard.
- **Updater stops adb:** Innioasis Updater kills any running `adb` when it starts. Don't use adb
  while it flashes.
- **Updater reuses its unpacked copy by file name.** It unpacks a package into
  `~/Library/Application Support/Innioasis Updater/downloads/.<zip name>_extracted` and reuses that
  folder the next time a zip of the same name is installed (its `.extract_complete` marker holds
  just `ok`), so a rebuilt `rom.zip` flashes the old images. Give each build its own file name,
  or delete that folder first. It also deletes the zip it was given once it has unpacked it
  (updater-ce-neo's `flash_service.py`), so keep a copy of a build you want to publish.

### The Rondo firmware

`firmware/build.sh` builds it from the y1-community "Android 4.2 (Dev)" Type A ROM
([Building Rondo](../building.md#the-firmware)). Facts that came out of running it on the Y1:

- **No Java layer (2.0).** The boot image's `init.rc` has zygote disabled and a `rondo` service
  that runs `/system/rondo/start` (`tools/firmware/BootImage.java`); `/system/app` is empty.
  SurfaceFlinger still starts, to set the panel up as it always has, and the start script stops it
  before Rondo draws to the framebuffer itself. There's no boot animation
  (`debug.sf.nobootanimation=1`): the boot logo stays until Rondo's first frame.
- **60 Hz (2.0).** The boot image's kernel has the panel driver's 60 Hz values in place of the
  stock 42 Hz ones ([native.md](native.md#display)).
- **No root backdoors (3.0).** Innioasis's firmware (3.0.7's build manifest and `init.rc`), and
  so the base ROM, runs two programs as root that nothing on the Y1 needs, both found by static
  analysis on Nov 12:
  - `aggdll` (init's `aggsrv`, setuid root) is the agent of "zxly" (`com.zxly.assist`), an app
    pusher. It's a hidden `su` for apps signed with zxly's key and a silent installer of APKs from
    the card. It also writes into `/system`: a copy of itself (`net_server`), a hidden copy of `pm`
    (`mp`), a program of its own in `am`'s place (Android's script kept as `am2`), and `wifi_init`,
    which it starts. `wifi_init` checks in with a server, downloads a file with `wifi_check` (the
    `axel` download tool) and runs it as root. `/system/fonts/AndroidBoot.ttf` is its state file.
    That server doesn't answer now (Nov 12). Wi-Fi doesn't need any of it.
  - `libsp.so` (init's `spwall`) is "SPWall", a service that runs shell commands as root for other
    programs.

  `firmware/build.sh` takes all of them out and puts Android's `am` script back, and
  `tools/firmware/BootImage.java` drops both services from `init.rc`. Since 3.0.1 SPWall's tools go
  too, though nothing starts them: `libwp.so` (a packet filter that asks SPWall), `libld.so` (a
  code injector) and `libip.so` (a renamed iptables; Android's own `iptables` stays), with
  `/system/channel/qbchannel.ini`, the stock firmware's distribution tag. Rondo 3.0 needs this firmware
  (firmware number 3), so a Y1 on 2.0.x gets it only by installing 3.0 with Innioasis Updater CE.
  Rondo also removes the same files itself at each start if it finds them (a bundle put on an older
  firmware by hand), with init's two services stopped (`native/services/base_rom.cpp`), and logs one
  line when it does. `BGW` (`/system/xbin`, user
  system) stays: it's MediaTek's Wi-Fi and modem coexistence helper, with no network code.
- **Current time zones.** The base ROM's tzdata is 2012h; the firmware has IANA's 2026 data
  (`firmware/zoneinfo`).
- **The firmware's number.** `firmware/build.sh` adds `ro.rondo.firmware=<n>` to
  `/system/build.prop`: `kFirmwareNeeded` in `native/core/version.h`, 3 for the 3.0 firmware (no
  root backdoors), 2 for 2.0's (the first without Android), 1 for 1.0.x's (the first, 1.0.0, has no
  property and counts as 1). Updates over Wi-Fi, the dashboard and the update page install only when the
  firmware is at least as new as the release needs.
- **Updates without a flash (2.0).** A signed bundle unpacks into `/data/rondo/<version>`, named
  by `/data/rondo/current`; the start script runs it instead of `/system/rondo`, and after three
  quick ends in a row goes back to the firmware's copy ([Building Rondo](../building.md#updates)).
- **adb stays on**, as root, for logs and the update page.
- **The Y1 doesn't start by itself after a flash.** Press and hold center (the power button) once
  the Updater says it's done.

#### Rondo 1.0's firmware

The 1.0.x firmware keeps Android's Java layer, with Rondo as its only app with a screen:

- **No SystemUI.** On this ROM SystemUI opens its own touch-only "USB connected" screen when a
  computer is plugged in, and crashes if that screen is disabled, so it is removed. Its other
  jobs move to Rondo, only when SystemUI is missing: the wheel's click
  (`AudioManager.playSoundEffect(FX_KEY_CLICK)`, still governed by `SOUND_EFFECTS_ENABLED`) and the
  low battery warning (`ACTION_BATTERY_LOW`).
- **The boot animation waits for a wallpaper.** Android 4.2 keeps it on screen
  (`service.bootanim.exit=0`) until a wallpaper draws, and the default `ImageWallpaper` lives in
  SystemUI. Rondo sets its own black `WallpaperService` with the hidden
  `IWallpaperManager.setWallpaperComponent`, which needs `SET_WALLPAPER_COMPONENT` (built-in apps
  only). Android remembers it, so only the first boot after a flash sets it.
- **USB drive:** `StorageManager.enableUsbMassStorage` and `disableUsbMassStorage` (hidden, need
  `MOUNT_UNMOUNT_FILESYSTEMS`, built-in apps only) work, but turning it on returns before the card
  has changed hands, and `isUsbMassStorageEnabled` asks vold about the wrong volume (vold calls the
  card `sdcard2`) and always says no. `Environment.getExternalStorageState()` reads `shared` while
  a computer has the card. The card shows up on a Mac as a 62.5 GB FAT32 volume named `NO NAME`.
  (2.0 asks vold itself, over its socket: `native/platform/y1/usb_drive.cpp`.)
- **Updates without a flash.** Rondo has `INSTALL_PACKAGES` as a built-in app, and `pm install -r`
  run by Rondo installs a newer APK over the firmware's copy: Android keeps `/system/app/Rondo.apk`,
  runs the update from `/data/app`, and grants it the built-in permissions the firmware's copy had
  (not new ones). Android stops Rondo for the swap and starts it again as the home screen; it's
  back on screen about 10 s after `pm` starts. Only an APK signed with the firmware copy's key is
  accepted. adb's `install -r` does the same. Every Rondo APK names the firmware number it needs
  (`rondo.firmware` in its manifest).
- **Booting** after a flash is slower the first time (Android optimizes every app); later boots
  take about 30 s.

## Hardware, storage, network, time

- **CPU and RAM:** MT6572, 2 × Cortex-A7 with NEON / VFPv4 (`proc-cpuinfo.txt`); 494,376 kB of
  RAM (`proc-meminfo.txt`).
- **Storage:**
  - The microSD card is `/storage/sdcard0` (vfat). `/storage/sdcard1` is a 433 MB FAT partition
    on the internal flash.
  - Both are mounted `uid=1023,gid=1023` (media_rw) with `fmask=0007,dmask=0007`, so other apps
    can't even list them. Only `WRITE_EXTERNAL_STORAGE` maps to `media_rw` in this firmware's
    `platform.xml`; `READ_EXTERNAL_STORAGE` isn't enough.
  - There's no MediaProvider scan to rely on, so Rondo walks the card itself.
    `MediaMetadataRetriever` reads FLAC tags and embedded covers on 4.2, but not lyrics
    (`local/EmbeddedLyrics.kt` reads those). 2.0 reads every tag itself (`native/core/tags.cpp`).
  - `/system` is 629 MB (read-only), `/data` 1.9 GB, `/cache` 503 MB.
- **Wi-Fi:** 2.4 GHz only. The scan list shows a fake network called "NVRAM WARNING: Err = 0x10",
  MediaTek's sign that the Wi-Fi calibration and MAC aren't provisioned. The MAC address changes on
  every Wi-Fi start (the 2.0 firmware gives it a fixed one: [native.md](native.md#wi-fi)). There's
  no `wpa_cli`; on Android, Wi-Fi is switched with `svc wifi disable|enable`.
- **Flaky networks:** on one phone hotspot, about 70% of new TCP connections to the internet were
  refused instantly (`EHOSTUNREACH`), while ICMP and established connections were fine. Rondo
  retries refused connects every 250 ms, up to 20 times (1.0: `net/ConnectRetry.kt`; 2.0:
  `native/core/api.cpp`), which took video start from 12.7 s to 2.2–2.8 s there.
- **Time:** on the stock firmware automatic time and timezone are on, but the timezone is
  `Asia/Shanghai` and there's no modem to correct it. The clock survives a restart with Wi-Fi off.
  Rondo 2.0 starts in UTC and sets the time and the time zone itself (the time zone notes are
  under [Wi-Fi in native.md](native.md#wi-fi)).
- **Battery:** there's no fuel gauge (`/sys/devices/platform/battery_meter/FG_*` all read 0).
  `/sys/class/power_supply/battery/BatteryAverageCurrent` (mA) is the only current reading, and on
  USB it shows the charging current, so measure discharge unplugged (adb over Wi-Fi).

## Audio

- One AudioFlinger output thread at 44.1 kHz.
- **Volume is per output.** Music on the speaker goes 0–13; wired headphones keep their own
  level. `input keyevent KEYCODE_VOLUME_DOWN` changes only the active output.
- **The video decoder's audio partner** is `OMX.MTK.AUDIO.DECODER.AAC`.

### FM radio

- **Access:** the connectivity chip is MediaTek's built-in one (MT6627-class FM). `/dev/fm` is
  `crw-rw---- system media`. Requesting `android.permission.ACCESS_MTK_MMHW` puts an ordinary
  app in the `media` (1013) and `camera` groups (granted on install).
- **The JNI library:** `/system/lib/libfmjni.so` registers natives on
  `com/mediatek/FMRadio/FMRadioNative` (33 methods: opendev, closedev, powerup(F)Z,
  powerdown(I)Z, tune(F)Z, seek(FZ)F, autoscan()[S, stopscan, readRssi, rdsset, readrds, getPI,
  getPTY()B, getPS()[B, getLRText, activeAF, getAFList, activeTA, deactiveTA, setmute(Z)I,
  getchipid, isRDSsupport, isFMPoweredUp, setFMViaBTController, switchAntenna(I)I, stereoMono,
  readCapArray, readRdsBler, setStereoMono, getHardwareVersion, scannew(III)[S,
  seeknew(IIIIII)I, tunenew(IIII)Z) and on `com/mediatek/FMTransmitter/FMTransmitterNative`
  (24). Both classes must exist with exactly these signatures, or the library won't load.
- **The chip:** 0x6627, band 76.0–108.0 MHz, 100 kHz steps, RDS supported. De-emphasis is 50 µs
  (`/system/etc/fmr/mt6627_fm_cust.cfg`). A full `autoscan` takes about 20 s.
- **Sound:** a `MediaPlayer` on `MEDIATEK://MEDIAPLAYER_PLAYERTYPE_FM` with stream 10
  (`STREAM_FM`, its own volume per output). It sets `AudioSetFmDigitalEnable=1` and plays chip →
  DAC over I2S, so AudioFlinger shows no track. `set ioctl FM_IOCTL_I2S_SETTING Fail
  !ret_FM(-1002)` appears for the stock player too and is harmless. The speaker is
  `AudioSystem.setForceUse(5, 1)`, and `(5, 0)` switches back. Wired headphones are the antenna.
- **The FM sound switch is shared by the whole system.** Every `FMAudioPlayer` sends
  `AudioSetFmDigitalEnable=0` when it's reset or destroyed. The stock app starts its own
  `FmRadioService` from `Y1Application.onCreate` at every boot (and on media-button or SD card
  events), and that service keeps an idle FM player. When the stock app's process ends
  (force-stop, low memory), mediaserver destroys that player and every other app's FM goes
  silent while the chip stays tuned. `getParameters("GetFmEnable")` reports the switch
  (`GetFmEnable=true|false`). Rondo checks it once a second while its radio plays and recreates
  its own player when it's off, releasing the old one first, since that also switches the sound
  off. Measured: back within 0.5 s.

## Video

- **Hardware decoders:** `OMX.MTK.VIDEO.DECODER.AVC` (with the `wants-NAL-fragments` quirk) and
  `OMX.MTK.AUDIO.DECODER.AAC`. YouTube's formats 134/135/136 and 18 are all H.264 Main profile.
- **The video decoder returns `presentationTimeUs = 0` for every frame**, whatever goes in.
  With stock ExoPlayer that meant about 9 fps rendered, frames racing ahead of the audio, and an
  empty buffer after about 18 s. `player/TimestampFix.kt` gives each output frame the smallest
  pending input timestamp, since frames come out in display order. Result: 25.0 fps, no drops,
  1.00× at 360p, 480p and 720p, and mediaserver's CPU use fell from 52% to 18%.
- **Resolution changes restart the decoder.** API 17 has no adaptive playback, so each switch
  costs 100–250 ms of re-init and about a second of dropped frames. Re-running ExoPlayer's track
  selection also flushed the buffer (1.8–3.7 s of buffering). `player/QualitySelection.kt` keeps
  one adaptive selection and changes the preferred height in place: no buffering, and the switch
  lands in about 4 s. Auto stops at 360p, the panel's size.
- **The log buffer is tiny.** ExoPlayer and MediaCodec push other lines out within seconds; stream
  it during tests: `adb logcat -v time -s Y1:V > file` (1.0; 2.0 writes its own log to
  `/data/rondo/rondo.log`).

The TimestampFix and QualitySelection files are 1.0's, around ExoPlayer; 2.0 decodes with the
native `MediaCodec` and handles the same timestamps itself ([native.md](native.md#video)).

## Drawing on Android 4.2 (Rondo 1.0)

- **Shapes under a scale come out blurry** with the GPU renderer: a path or circle is drawn at its
  own size and then enlarged. Large icons are drawn with a software layer (`ui/IconView.kt`).
- **`TransitionDrawable` as a view background stalls** on its first frame (stuck near 14% alpha).
  Swap backgrounds directly.
- **Fonts:** the system Roboto has no Azerbaijani ə/Ə, so the app bundles a newer one. With an
  asset font, `setTypeface(tf, BOLD)` reports bold without drawing it, and the GPU text renderer
  ignores `setFakeBoldText`, so bold is drawn as fill plus a same-color stroke (6% of the size).
- **Animation cost:** redrawing about 8 lines of text per frame cost 12–140 ms of GL time per
  frame. The lyrics view draws its lines once into an `ALPHA_8` bitmap and only moves and fades
  bitmaps: 2.2 ms per frame on average. A shader on an `ALPHA_8` `drawBitmap` is applied in
  window coordinates, not the view's. `TextView.setText` with unchanged text still lays the view
  out, so only set real changes.
- **Settings an app can change:** `WRITE_SETTINGS` is granted at install on API 17, so screen
  timeout, brightness and the click sound are settable from Rondo.

## Boot screens

The bootloader and kernel show images 0 and 38 of the MediaTek `logo` partition (in
`/proc/dumchar_info`: `mmcblk0` offset `0x2a40000`, 3 MB; a "LOGO" header and 39 zlib-compressed
480×360 RGB565 images, of which 1–37 are the charging animation). Android's `bootanimation` reads
`/data/local/bootanimation.zip` before `/system/media/bootanimation.zip`, so on a firmware with a
boot animation (the stock one, or Rondo 1.0's) it can be replaced without touching the system
partition (see `tools/bootscreen`). The 2.0 firmware has none: image 0 stays until Rondo's first
frame.

## Files

| File | What it holds |
| --- | --- |
| `getevent-l.txt` | 45 s of `getevent -lt`: wheel both ways, then Center, Menu and Play. |
| `getevent-l-2.txt` | 30 s of `getevent -lt`: every control, including Next, Prev and fast spins. |
| `getevent-l-3-clockwise-3-clicks.txt` | Exactly 3 clockwise clicks: clockwise = `KEY_DOWN`, about 2 pulses per detent. |
| `getevent-p.txt` | The four input devices, with the keys and axes each declares. |
| `dumpsys-input.txt` | Android's view of each input device and the key layout it uses. |
| `keylayout-Generic.kl` | The active key layout (identical to the stock `Stock.kl`). |
| `keylayout-mtk-kpd.kl` | The unused keypad layout (Center would be `DPAD_CENTER` here). |
| `keylayout-ls.txt` | The key layout files on the device. |
| `dumpsys-display.txt`, `lcd-density.txt` | 480×360, density 160, 36.43 fps as Android reports it (the panel runs at 42.14 Hz). |
| `native.md` | Rondo 2.0 without Android's Java layer: its processes and files, the display at 60 Hz, vsync, sound, video, Wi-Fi, power, Bluetooth, FM, building against the firmware's libraries. |
| `wm-size.txt`, `wm-density.txt` | `wm` is missing on 4.2. |
| `proc-cpuinfo.txt`, `proc-meminfo.txt`, `proc-version.txt` | CPU, RAM, kernel. |
