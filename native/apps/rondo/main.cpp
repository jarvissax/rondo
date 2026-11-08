// rondo: Rondo 2.0's native shell, a 1:1 port of 1.0.x's screens (private/native-plan.md, "parity
// first"). Runs on the Y1 with the Java layer and SurfaceFlinger stopped.
//
// Ported so far: the shell (ShellActivity: the screen stack, the status bar), the main menu with its
// preview pane (MainMenuScreen.kt), and the card library (DeviceScreens.kt: Library, Songs, Artists,
// Albums, Recently added, Videos). Everything else is a row that does nothing yet.
//
// What 2.0 adds is motion: screens slide like the iPod's (1.0.x replaces them), the list and its
// highlight glide to where 1.0.x puts them, the preview pane cross-fades.
//
//   rondo [--config path] [--library db] [--fonts dir] [--regular ttf] [--thumbs dir] [--csv path]
//         [--restore-android 1] [--shot path]
//
// SIGUSR1 saves a screenshot (--shot, default /data/local/tmp/rondo/shot.png). Holding Menu 2 s quits
// (a development affordance); with --restore-android 1, 1.0.x then comes back.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/library.h"
#include "../../platform/platform.h"
#include "../../platform/usb_drive.h"
#include "../../platform/power.h"
#include "../../platform/haptics.h"
#include "../../shell/dashboard.h"
#include "../../shell/main_queue.h"
#include "../../platform/video_frame.h"
#include "../../platform/video_surface.h"
#include "../../services/core.h"
#include "../../shell/dispatch.h"
#include "../../shell/kit.h"
#include "../../shell/app.h"
#include "../../shell/screens.h"
#include "../../ui/frame_log.h"
#include "../../ui/keyboard.h"

using namespace rondo;
using namespace rondo::ui;
using namespace rondo::shell;

namespace rondo::shell {
Config g_config;
Library g_library;
PlayerHost g_player;
CoreHost g_core;
Playlists g_playlists(g_library);
Prefs g_prefs;
std::string g_configPath, g_prefsPath;

void saveConfigNow() {
  if (!saveConfig(g_configPath.c_str(), g_config)) fprintf(stderr, "rondo: settings not saved\n");
}
void savePrefsNow() {
  if (!savePrefs(g_prefsPath, g_prefs)) fprintf(stderr, "rondo: device settings not saved\n");
}
void themeChanged() {
  sanitize(g_config.theme);
  saveConfigNow();
  if (g_app && g_app->onThemeChanged) g_app->onThemeChanged();
}
void applyBrightness(int value) {
  int fd = open("/sys/class/leds/lcd-backlight/brightness", O_WRONLY);
  if (fd < 0) return;
  char b[8];
  int n = snprintf(b, sizeof(b), "%d", std::max(10, std::min(255, value)));
  if (write(fd, b, size_t(n)) != n) fprintf(stderr, "rondo: brightness not set\n");
  close(fd);
}
App* g_app = nullptr;
bool g_traceSlow = false;
}  // namespace rondo::shell

namespace rondo {
CoreHost& coreHost() { return shell::g_core; }
}  // namespace rondo

namespace {

volatile sig_atomic_t g_stop = 0, g_shot = 0;

float readBattery() {
  int v = 100;
  if (FILE* f = fopen("/sys/class/power_supply/battery/capacity", "r")) {
    if (fscanf(f, "%d", &v) != 1) v = 100;
    fclose(f);
  }
  return std::clamp(v / 100.0f, 0.0f, 1.0f);
}

// online: a default route exists (ConnectivityManager's answer, without Java)
bool readOnline() {
  FILE* f = fopen("/proc/net/route", "r");
  if (!f) return false;
  char line[256];
  bool online = false;
  while (fgets(line, sizeof(line), f)) {
    char iface[32];
    unsigned dest = 1;
    if (sscanf(line, "%31s %x", iface, &dest) == 2 && dest == 0) online = true;
  }
  fclose(f);
  return online;
}

void onSignal(int sig) {
  if (sig == SIGUSR1) g_shot = 1;
  else g_stop = 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* configPath = "/data/data/com.jarvissax.rondo/files/config.json";
  const char* libraryDb = "/data/local/tmp/rondo/library.db";
  const char* prefsPath = "/data/local/tmp/rondo/prefs.json";
  const char* indexJson = "/data/data/com.jarvissax.rondo/files/local-index.json";
  const char* fonts = "/system/fonts";
  const char* regular = "/data/local/tmp/rondo/fonts/Roboto-Regular.ttf";
  const char* thumbs = "/data/local/tmp/rondo/thumbs";
  const char* shotPath = "/data/local/tmp/rondo/shot.png";
  const char* advances = "/data/local/tmp/rondo/advances.bin";
  const char* csv = nullptr;
  bool restoreAndroid = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--config")) configPath = argv[i + 1];
    else if (!strcmp(argv[i], "--library")) libraryDb = argv[i + 1];
    else if (!strcmp(argv[i], "--prefs")) prefsPath = argv[i + 1];
    else if (!strcmp(argv[i], "--fonts")) fonts = argv[i + 1];
    else if (!strcmp(argv[i], "--regular")) regular = argv[i + 1];
    else if (!strcmp(argv[i], "--thumbs")) thumbs = argv[i + 1];
    else if (!strcmp(argv[i], "--shot")) shotPath = argv[i + 1];
    else if (!strcmp(argv[i], "--csv")) csv = argv[i + 1];
    else if (!strcmp(argv[i], "--restore-android")) restoreAndroid = atoi(argv[i + 1]) != 0;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGUSR1, onSignal);
  signal(SIGHUP, SIG_IGN);
  setpriority(PRIO_PROCESS, 0, -10);
  if (FILE* pf = fopen("/data/local/tmp/rondo/rondo.pid", "w")) {
    fprintf(pf, "%d\n", getpid());
    fclose(pf);
  }
  mainQueueInit();  // this is the UI thread
  // the dashboard's page beside the fonts' folder (<dir>/fonts, <dir>/dashboard)
  {
    std::string r = regular;
    std::string dir = r.substr(0, r.rfind('/'));
    g_dashboardAssets = dir.substr(0, dir.rfind('/'));
  }
  // first, while this is the only thread: it forks the decoding process
  ui::setFrameGrabber(grabVideoFrame);  // card videos' artwork: a frame (in rondo-media)
  ImageLoader loader;
  if (!loader.start(2, thumbs)) return 1;
  if (!g_player.start()) return 1;  // rondo-player, also before any thread
  if (!g_core.start()) return 1;    // rondo-core, the system services, the same
  // Development: RONDO_WIFI_JOIN="<name><tab><password>" joins a WPA network at startup (scripts, without
  // typing on the wheel)
  if (const char* j = getenv("RONDO_WIFI_JOIN")) {
    std::string s = j;
    size_t tab = s.find('\t');
    if (tab != std::string::npos) g_core.wifiJoin(s.substr(0, tab), WifiSecurity::Psk, s.substr(tab + 1));
  }
  int64_t t0 = now_ns();
  g_traceSlow = getenv("RONDO_SLOW") != nullptr;
  g_config = loadConfig(configPath);
  g_configPath = configPath, g_prefsPath = prefsPath;
  g_prefs = loadPrefs(prefsPath);
  // the screen at its brightness whatever the last owner left (Android's 0, or a dimmed screen)
  applyBrightness(g_prefs.brightness);
  // the server's answers kept for two minutes, on disk too: beside the prefs
  const std::string apiCache = g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "api-cache";
  g_core.apiConfigure(g_config.server.url, g_config.server.token, apiCache);
  // Bluetooth's device names and last headphones next to the prefs; on if it was on (Android's setting)
  g_core.btConfigure(g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "bluetooth.json");
  if (g_prefs.bluetoothOn) g_core.btPower(true);
  g_core.wifiEnable(g_prefs.wifiOn);  // as it was left (rondo-core takes over a running supplicant)
  loader.configure(g_config.server.url, g_config.server.token);
  g_player.configure(g_config.server.url, g_config.server.token);
  // Settings › Server connected to another server or took a token: everything that talks to it
  g_onServerChanged = [&] {
    g_core.apiConfigure(g_config.server.url, g_config.server.token, apiCache);
    loader.configure(g_config.server.url, g_config.server.token);
    g_player.configure(g_config.server.url, g_config.server.token);
  };
  Palette pal = paletteFor(g_config.theme);
  if (g_library.open(libraryDb) && g_library.tracks().empty()) g_library.importIndexJson(indexJson);
  g_library.scanAsync();  // the card read again on the library's thread; what is indexed shows at once
  g_library.artists();  // builds the groups now, not when Library opens
  int64_t tLib = now_ns();

  auto display = openDisplay();
  if (!display) return 1;
  auto input = openInput();
  if (!input) return 1;
  Canvas canvas;
  if (!canvas.init(display->width(), display->height())) return 1;
  Text text;
  // the color emoji next to the bundled Roboto
  std::string emoji = std::string(regular).substr(0, std::string(regular).rfind('/') + 1) + "NotoColorEmoji.ttf";
  if (!text.init(fonts, access(regular, R_OK) == 0 ? regular : nullptr, access(emoji.c_str(), R_OK) == 0 ? emoji.c_str() : nullptr)) return 1;
  if (!text.loadAdvances(advances)) fprintf(stderr, "rondo: no advance table at %s: text widths will differ from 1.0.x\n", advances);
  text.loadKerning("/data/local/tmp/rondo/kerning.bin");
  Icons icons;
  icons.init(text);
  Covers covers(loader);
  covers.reserve(int(floorf(26 * pal.scale + 0.5f)), int(floorf(26 * pal.scale + 0.5f)), 96);
  // Now Playing's artwork, its backdrop and the lyrics' thumbnail; the main menu's pane
  for (float u : {140.f, 22.f, 98.f, 24.f}) covers.reserve(int(floorf(u * pal.scale + 0.5f)), int(floorf(u * pal.scale + 0.5f)), 3);
  covers.reserve(int(floorf(34 * pal.scale + 0.5f)), int(floorf(34 * pal.scale + 0.5f)), 24);  // MUSIC rows
  covers.reserve(6, 6, 3);
  {  // the list thumbnails, decoded once into the cache while nothing moves
    int t = int(floorf(26 * pal.scale + 0.5f));
    for (const Track& tr : g_library.tracks()) covers.prefetch(tr.path, t, t, 0);
  }
  App app;
  g_app = &app;
  app.push(makeMainMenu());
  // A computer on the cable offers the USB drive screen, at startup too (1.0.x's sticky broadcast).
  // RONDO_USB_PROMPT=0 (the test scripts) leaves it to the cable being plugged in later.
  bool usbComputer = usb::computerConnected();
  if (usbComputer && !(getenv("RONDO_USB_PROMPT") && !strcmp(getenv("RONDO_USB_PROMPT"), "0"))) app.push(makeUsbDrive());
  int64_t lastUsb = 0;
  Dispatcher dispatch;
  dispatch.acceleration = g_config.behavior.wheelAcceleration;
  app.onAccelerationChanged = [&] { dispatch.acceleration = g_config.behavior.wheelAcceleration; };
  dispatch.emit = [&](Action a, int steps) { app.action(a, steps); };
  // 1.0.x's onPulse (WheelHaptics.kt, ShellActivity): a buzz of the setting's length plus a boost that
  // fades as a spin goes on (a slow turn feels like clicks, a fast spin like a purr), and the click
  dispatch.onPulse = [](int spin) {
    const std::string& level = g_config.behavior.wheelVibration;
    int base = level == "light" ? 15 : level == "normal" ? 25 : level == "strong" ? 40 : 0;
    if (base) rondo::haptics::vibrate(base + (spin < 3 ? 20 : spin < 7 ? 8 : spin < 15 ? 2 : 0));
    if (g_prefs.clickSound) g_player.click();
  };
  g_onSleep = [&] { dispatch.cancel(); };
  Status status{false, readOnline(), readBattery()};
  printf("rondo: %zu files in the library (%.0f ms), ready at %.0f ms\n", g_library.tracks().size(), (tLib - t0) / 1e6,
         (now_ns() - t0) / 1e6);

  FrameLog log(display->period());
  const int64_t period = display->period();
  bool moving = true, wasMoving = false;
  int prevGlyphs = 0, prevRows = 0, prevUploads = 0;  // for RONDO_SLOW
  int64_t prevSpan = 0;
  int64_t lastPresent = 0, lastStatus = now_ns(), menuDown = 0;
  // Glyphs and icons are rasterized while idle: adding to the atlas while a frame still uses it makes the
  // next frame wait a whole refresh for the GPU (N5 log), so a screen's first frame must find them ready.
  // What is warmed depends on the text scale: built again when the theme changes it.
  std::vector<int> warmFonts, warmDigits;
  struct WarmIcon {
    Icon icon;
    int w, h;
  };
  std::vector<WarmIcon> warmIconList;
  auto buildWarm = [&] {
    warmFonts.clear();
    for (float sp : {15.f, 13.5f, 11.f, 12.f, 14.f, 10.f, 9.f})
      for (bool bold : {false, true}) warmFonts.push_back(text.font(bold ? Weight::Bold : Weight::Regular, sp * pal.scale));
    for (int px : {int(floorf(15 * pal.scale + 0.5f)), int(floorf(12 * pal.scale + 0.5f))})  // the keyboard's labels
      for (bool bold : {false, true}) warmFonts.push_back(text.font(bold ? Weight::Bold : Weight::Regular, float(px)));
    // Now Playing: the title, the artist; the lyrics' sung line
    for (float sp : {17.f, 17.5f}) warmFonts.push_back(text.font(Weight::Bold, sp * pal.scale));
    warmFonts.push_back(text.font(Weight::Regular, 13 * pal.scale));
    warmFonts.push_back(text.font(Weight::Bold, 13 * pal.scale));  // the selected MUSIC row
    warmDigits = {text.font(Weight::Bold, 26 * pal.scale)};        // the scrub bubble's time
    auto d = [&](float v) { return int(floorf(v * pal.scale + 0.5f)); };
    auto& list = warmIconList;  // every icon at the sizes the shell draws it
    list.clear();
    for (Icon k : {Icon::Note, Icon::Play, Icon::Folder, Icon::Bars, Icon::Gear, Icon::Wifi, Icon::Bluetooth, Icon::Cloud,
                   Icon::Info, Icon::Person, Icon::Disc, Icon::Screen, Icon::Sun, Icon::Timer, Icon::Radio, Icon::Volume,
                   Icon::Shuffle, Icon::Power, Icon::Refresh, Icon::Theme, Icon::Playlist, Icon::Plus, Icon::Search,
                   Icon::Trash, Icon::Shorts, Icon::Check})
      list.push_back({k, d(16), d(16)});
    for (Icon k : {Icon::Bars, Icon::Folder, Icon::Radio, Icon::Gear}) list.push_back({k, d(70), d(70)});
    list.push_back({Icon::Chevron, d(8), d(12)});
    list.push_back({Icon::Wifi, d(14), d(14)});
    list.push_back({Icon::BatteryFrame, d(24), d(14)});
    list.push_back({Icon::Play, d(12), d(12)});
    list.push_back({Icon::Search, d(11), d(11)});
    // Now Playing
    list.push_back({Icon::Note, d(44), d(44)});
    for (Icon k : {Icon::Shuffle, Icon::Repeat}) list.push_back({k, d(15), d(13)});
    for (Icon k : {Icon::Play, Icon::Pause}) list.push_back({k, d(9), d(9)});
    list.push_back({Icon::Wifi, d(11), d(11)});
    list.push_back({Icon::BatteryFrame, d(18), d(11)});
    list.push_back({Icon::Volume, d(22), d(22)});
    // the YouTube Music lists: the chevron, the mini player's state, the header's logo
    list.push_back({Icon::Chevron, d(7), d(11)});
    for (Icon k : {Icon::Play, Icon::Pause}) list.push_back({k, d(12), d(12)});
    for (Icon k : {Icon::MusicLogoDisc, Icon::MusicLogoMarks}) list.push_back({k, d(16), d(16)});
    for (Icon k : {Icon::MusicLogoDisc, Icon::MusicLogoMarks}) list.push_back({k, d(19), d(19)}), list.push_back({k, d(70), d(70)});
    for (Icon k : {Icon::VideoLogoButton, Icon::VideoLogoPlay}) list.push_back({k, d(19), d(14)}), list.push_back({k, d(80), d(56)});
  };
  buildWarm();
  static const char kWarmChars[] =
      " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
      "ÀÁÂÄÇÈÉÊÍÑÓÖÚÜ"
      "àáâãäåçèéêëíîïñóôöøúûü"
      "ĞğİıŞşƏə·–—‘’“”•…";
  bool warmed = false;
  int warmStage = 0;
  size_t warmIcon = 0;
  auto warmIcons = [&] {  // a few per idle slice
    int64_t until = now_ns() + 3 * kMs;
    const auto& list = warmIconList;
    while (warmIcon < list.size() && now_ns() < until) icons.mask(list[warmIcon].icon, list[warmIcon].w, list[warmIcon].h), warmIcon++;
    return warmIcon >= list.size();
  };
  // Settings › Theme: the palette, and the text scale's sizes warmed again (1.0.x rebuilds its screens)
  app.onThemeChanged = [&] {
    pal = paletteFor(g_config.theme);
    buildWarm();
    warmIcon = 0;
    warmStage = 0;
    warmed = false;
    text.resetWarm();
    auto u = [&](float v) { return int(floorf(v * pal.scale + 0.5f)); };
    covers.reserve(u(26), u(26), 96);
    covers.reserve(u(34), u(34), 24);
    for (float v : {140.f, 22.f, 98.f, 24.f}) covers.reserve(u(v), u(v), 3);
  };
  int64_t lastPeriodic = 0, lastLog = 0;
  static const bool kWifiLog = getenv("RONDO_WIFI_LOG"), kPlayerLog = getenv("RONDO_PLAYER_LOG");
  // The timers, the cable and the status: on every quiet wake, and every 250 ms while frames follow each
  // other (a video's pictures, a long animation), which never leave the loop quiet.
  auto periodic = [&]() {
    bool changed = false;
    lastPeriodic = now_ns();
    if (powerTick(now_ns())) changed = true;
    dispatch.tick(now_ns());
    bluetoothTick();
    fmTick();
    streamsNetwork(status.online);  // read with the status, every 500 ms
    soundTick();
    if (g_library.applyScan()) changed = true;  // a background scan's files and tags
    if (app.tick(now_ns())) changed = true;
    if (now_ns() - lastUsb > kSec) {  // the cable, once a second
      lastUsb = now_ns();
      bool c = usb::computerConnected();
      if (c != usbComputer) usbComputer = c, usbCableChanged(c), changed = true;
      if (usbIdle()) changed = true;
    }
    if ((g_player.state().playing != 0) != status.playing) changed = true;
    if (now_ns() - lastStatus >= 500 * kMs) {  // 1.0.x refreshes on its 500 ms tick
      lastStatus = now_ns();
      Status s{false, readOnline(), readBattery()};
      if (s.online != status.online || fabsf(s.battery - status.battery) > 0.001f) status = s, changed = true;
    }
    if (now_ns() - lastLog > 2 * kSec) {  // development: what the services say, every 2 s
      lastLog = now_ns();
      if (kWifiLog) {
        CoreState cs = g_core.state();
        char ip[16] = "";
        if (cs.ip) snprintf(ip, sizeof(ip), "%u.%u.%u.%u", cs.ip & 0xff, cs.ip >> 8 & 0xff, cs.ip >> 16 & 0xff, cs.ip >> 24);
        printf("wifi: enabled %d state %d ssid \"%s\" ip %s rssi %d speed %d, %d networks, %d saved, failed \"%s\"\n", cs.wifiEnabled,
               cs.wifiState, cs.ssid, ip, cs.rssi, cs.linkSpeed, cs.networkCount, cs.savedCount, cs.failedSsid);
        std::string names;
        for (int i = 0; i < cs.networkCount; i++) names += std::string(i ? ", " : "") + cs.networks[i].ssid + " " + std::to_string(cs.networks[i].rssi);
        printf("wifi: in range: %s\n", names.c_str());
        names.clear();
        for (int i = 0; i < cs.savedCount; i++) names += std::string(i ? ", " : "") + std::to_string(cs.saved[i].id) + " " + cs.saved[i].ssid;
        printf("wifi: saved: %s\n", names.c_str());
      }
      if (kPlayerLog) {
        PlayerState ps = g_player.state();
        printf("player: %s %d/%d %lld/%lld ms vol %d %s\n", ps.playing ? "playing" : "paused", ps.pos, ps.count,
               (long long)ps.positionMs, (long long)ps.durationMs, ps.volume, ps.error);
      }
    }
    return changed;
  };
  while (!g_stop) {
    if (!moving && !g_shot) {
      VideoSurface* video = VideoSurface::made();  // a video's pictures wake the loop as input does
      pollfd p[6] = {{input->waitFd(), POLLIN, 0}, {loader.waitFd(), POLLIN, 0}, {g_core.fd(), POLLIN, 0},
                     {video ? video->fd() : -1, POLLIN, 0}, {power::wakeFd(), POLLIN, 0}, {mainQueueFd(), POLLIN, 0}};
      if (!warmed) {
        // the icons, the fonts' characters, the bubble's digits, a slice at a time
        if (warmStage == 0 && warmIcons()) warmStage = 1, text.resetWarm();
        if (warmStage == 1 && text.warm(warmFonts, kWarmChars, 3 * kMs)) warmStage = 2, text.resetWarm();
        if (warmStage == 2 && text.warm(warmDigits, "0123456789:", 3 * kMs)) {
          warmStage = 3;
          printf("rondo: warmed, the atlas holds %d of 2048 rows\n", text.atlasRows());
        } else if (warmStage == 3) {
          // Now Playing drawn once, unseen (never presented): the first time it was drawn, the next
          // frame waited a whole refresh for the GPU, a cost paid once per run (N3 log).
          warmStage = 4;
          std::unique_ptr<Screen> np = makeNowPlayingPreview();
          canvas.begin(pal.shellBg);
          Env env{canvas, text, icons, covers, pal, now_ns()};
          np->draw(env, float(display->width()), float(display->height()));
          text.flush();
          canvas.end();
          glFinish();
        }
        warmed = warmStage == 4;
        text.flush();  // to the GPU now, not in a moving frame
        pollfd q[1] = {{input->waitFd(), POLLIN, 0}};
        if (poll(q, 1, 0) == 0) continue;
      }
      bool prefetching = covers.sendPrefetches() | covers.topUp();
      int64_t deadline = dispatch.nextDeadline();
      int timeout = prefetching ? 50 : screenDrawing() ? 500 : 1000;  // the status bar's 500 ms tick while lit
      if (deadline) timeout = std::max(0, std::min(timeout, int((deadline - now_ns()) / kMs) + 1));
      if (g_player.active()) timeout = std::min(timeout, 250);  // the position, the play state
      if (g_library.scanning()) timeout = std::min(timeout, 50);  // the scan's results as they come
      if (int64_t wake = app.wakeAt()) timeout = std::max(0, std::min(timeout, int((wake - now_ns()) / kMs) + 1));
      if (int64_t wake = powerWakeAt()) timeout = std::max(0, std::min(timeout, int((wake - now_ns()) / kMs) + 1));
      if (int64_t wake = mainQueueWakeAt()) timeout = std::max(0, std::min(timeout, int((wake - now_ns()) / kMs) + 1));
      int r = poll(p, 6, timeout);
      if (runMainQueue()) moving = true;  // the dashboard's requests, on this thread
      if (r > 0 && (p[4].revents & POLLIN)) {  // drawing may start again after a suspend
        uint64_t n;
        (void)!read(p[4].fd, &n, sizeof(n));
      }
      if (r > 0 && (p[2].revents & POLLIN) && !g_core.pump() && !(p[0].revents | p[1].revents | p[3].revents | p[4].revents) && !moving) continue;  // half a reply
      if (r < 0 && errno == EINTR) continue;
      if (r == 0) {
        if (periodic()) moving = true;
        if (!moving) continue;
      }
    }
    g_core.pump();  // the server's replies, on this thread, before the frame
    runMainQueue();
    if (now_ns() - lastPeriodic > 250 * kMs) periodic();
    if (!screenDrawing()) {  // the screen is off: input only (Power wakes it), nothing drawn, no refresh waited for
      input->acknowledge();
      InputEvent ev;
      while (input->poll(ev))
        if (powerFilter(ev)) dispatch.event(ev);
      dispatch.tick(now_ns());
      moving = powerWantsFrame();
      wasMoving = false;
      continue;
    }
    int64_t vs = display->waitVsync();
    int64_t present = vs + period;
    app.setNow(present);
    int64_t a = now_ns();  // the frame's work starts with the input: a screen opening counts
    input->acknowledge();
    InputEvent ev;
    int64_t oldest = -1;
    while (input->poll(ev)) {
      if (oldest < 0) oldest = vs - ev.time;
      if (!powerFilter(ev)) continue;  // used up: waking, swallowed, the Power key
      dispatch.event(ev);
      if (ev.key == Key::Menu) {
        if (ev.down) menuDown = ev.time;
        else if (menuDown && ev.time - menuDown >= 2 * kSec) g_stop = 1;
      }
    }
    dispatch.tick(now_ns());
    // Opening a screen can take 10 ms. A frame finished that late reaches the panel a refresh late and holds
    // up the next one (N5 log), so such a refresh draws nothing: the slide starts cleanly on the next.
    if (now_ns() - a > 6 * kMs) {
      app.delay(period);
      lastPresent = present;
      wasMoving = moving = true;
      continue;
    }
    if (VideoSurface* v = VideoSurface::made()) v->latch();  // the newest picture, for this frame
    float dt = wasMoving && lastPresent ? std::min(0.1f, float(present - lastPresent) / 1e9f) : float(period) / 1e9f;
    int64_t a1 = now_ns();
    bool anim = app.step(dt);
    int64_t a2 = now_ns();
    canvas.begin(pal.shellBg);
    int64_t a3 = now_ns();
    Env env{canvas, text, icons, covers, pal, present};
    status.playing = g_player.state().playing != 0;
    app.draw(env, float(display->width()), float(display->height()), status);
    int64_t b = now_ns();
    if (g_traceSlow && b - a > 8 * kMs)
      printf("slow: frame at %.0f ms: input %.1f, step %.1f, clear %.1f, draw %.1f ms (the frame before: %d glyphs, "
             "%d atlas rows, %d covers uploaded, presented %.1f ms after its vsync)\n",
             (vs - t0) / 1e6, (a1 - a) / 1e6, (a2 - a1) / 1e6, (a3 - a2) / 1e6, (b - a3) / 1e6, prevGlyphs, prevRows, prevUploads, prevSpan / 1e6);
    text.flush();
    bool more = covers.pump(present, 2);
    prevGlyphs = text.glyphsRasterizedLastFrame(), prevRows = text.flushedRows(), prevUploads = covers.uploadedLast();
    canvas.end();
    int64_t c = now_ns();
    if (g_shot) {
      g_shot = 0;
      printf("rondo: screenshot %s\n", saveScreenshot(shotPath, display->width(), display->height()) ? "saved" : "failed");
    }
    display->present();
    framePresented();  // after a wake, the backlight once what is there now is on the panel
    int64_t d = now_ns();
    prevSpan = d - vs;
    FrameLog::Frame f{};
    f.vsync = vs, f.buildNs = int32_t(b - a), f.submitNs = int32_t(c - b), f.swapNs = int32_t(d - c);
    f.inputNs = int32_t(oldest), f.draws = int16_t(canvas.drawCalls()), f.quads = canvas.quads();
    f.glyphs = int16_t(text.glyphsRasterizedLastFrame()), f.continued = wasMoving, f.uploads = int16_t(covers.uploadedLast());
    log.add(f);
    text.frameDone();
    lastPresent = present;
    moving = anim || more || dispatch.nextDeadline() != 0;
    if (moving) loader.active(present);
    wasMoving = moving;
  }
  log.report(stdout, csv);
  if (restoreAndroid) {
    display.reset();
    system("start surfaceflinger; sleep 1; start zygote");
  }
  fflush(stdout);
  _exit(0);
}
