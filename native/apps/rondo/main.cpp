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
App* g_app = nullptr;
}  // namespace rondo::shell

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
  // first, while this is the only thread: it forks the decoding process
  ImageLoader loader;
  if (!loader.start(2, thumbs)) return 1;
  if (!g_player.start()) return 1;  // rondo-player, also before any thread
  int64_t t0 = now_ns();
  g_config = loadConfig(configPath);
  Palette pal = paletteFor(g_config.theme);
  if (g_library.open(libraryDb) && g_library.tracks().empty()) g_library.importIndexJson(indexJson);
  g_library.scan();
  g_library.readTags();
  g_library.save();
  g_library.artists();  // builds the groups now, not when Library opens
  int64_t tLib = now_ns();

  auto display = openDisplay();
  if (!display) return 1;
  auto input = openInput();
  if (!input) return 1;
  Canvas canvas;
  if (!canvas.init(display->width(), display->height())) return 1;
  Text text;
  if (!text.init(fonts, access(regular, R_OK) == 0 ? regular : nullptr)) return 1;
  if (!text.loadAdvances(advances)) fprintf(stderr, "rondo: no advance table at %s: text widths will differ from 1.0.x\n", advances);
  text.loadKerning("/data/local/tmp/rondo/kerning.bin");
  Icons icons;
  icons.init(text);
  Covers covers(loader);
  covers.reserve(int(floorf(26 * pal.scale + 0.5f)), int(floorf(26 * pal.scale + 0.5f)), 96);
  {  // the list thumbnails, decoded once into the cache while nothing moves
    int t = int(floorf(26 * pal.scale + 0.5f));
    for (const Track& tr : g_library.tracks()) covers.prefetch(tr.path, t, t, 0);
  }
  App app;
  g_app = &app;
  app.push(makeMainMenu());
  Dispatcher dispatch;
  dispatch.acceleration = g_config.behavior.wheelAcceleration;
  dispatch.emit = [&](Action a, int steps) { app.action(a, steps); };
  Status status{false, readOnline(), readBattery()};
  printf("rondo: %zu files in the library (%.0f ms), ready at %.0f ms\n", g_library.tracks().size(), (tLib - t0) / 1e6,
         (now_ns() - t0) / 1e6);

  FrameLog log(display->period());
  const int64_t period = display->period();
  bool moving = true, wasMoving = false;
  int64_t lastPresent = 0, lastStatus = now_ns(), menuDown = 0;
  // Glyphs and icons are rasterized while idle: adding to the atlas while a frame still uses it makes the
  // next frame wait a whole refresh for the GPU (N5 log), so a screen's first frame must find them ready.
  std::vector<int> warmFonts;
  for (float sp : {15.f, 13.5f, 11.f, 12.f, 14.f, 10.f, 9.f})
    for (bool bold : {false, true}) warmFonts.push_back(text.font(bold ? Weight::Bold : Weight::Regular, sp * pal.scale));
  for (int px : {int(floorf(15 * pal.scale + 0.5f)), int(floorf(12 * pal.scale + 0.5f))})  // the keyboard's labels
    for (bool bold : {false, true}) warmFonts.push_back(text.font(bold ? Weight::Bold : Weight::Regular, float(px)));
  static const char kWarmChars[] =
      " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
      "ÀÁÂÄÇÈÉÊÍÑÓÖÚÜ"
      "àáâãäåçèéêëíîïñóôöøúûü"
      "ĞğİıŞşƏə·–—‘’“”•…";
  bool warmed = false;
  size_t warmIcon = 0;
  auto warmIcons = [&] {  // every icon at the sizes the shell draws it, a few per idle slice
    auto d = [&](float v) { return int(floorf(v * pal.scale + 0.5f)); };
    struct I { Icon icon; int w, h; };
    static std::vector<I> list;
    if (list.empty()) {
      for (Icon k : {Icon::Note, Icon::Play, Icon::Folder, Icon::Bars, Icon::Gear, Icon::Wifi, Icon::Bluetooth, Icon::Cloud,
                     Icon::Info, Icon::Person, Icon::Disc, Icon::Screen, Icon::Sun, Icon::Timer, Icon::Radio, Icon::Volume,
                     Icon::Shuffle, Icon::Power, Icon::Refresh, Icon::Theme, Icon::Playlist, Icon::Plus, Icon::Search,
                     Icon::Trash, Icon::Shorts})
        list.push_back({k, d(16), d(16)});
      for (Icon k : {Icon::Bars, Icon::Folder, Icon::Radio, Icon::Gear}) list.push_back({k, d(70), d(70)});
      list.push_back({Icon::Chevron, d(8), d(12)});
      list.push_back({Icon::Wifi, d(14), d(14)});
      list.push_back({Icon::BatteryFrame, d(24), d(14)});
      list.push_back({Icon::Play, d(12), d(12)});
      list.push_back({Icon::Search, d(11), d(11)});
      for (Icon k : {Icon::MusicLogoDisc, Icon::MusicLogoMarks}) list.push_back({k, d(19), d(19)}), list.push_back({k, d(70), d(70)});
      for (Icon k : {Icon::VideoLogoButton, Icon::VideoLogoPlay}) list.push_back({k, d(19), d(14)}), list.push_back({k, d(80), d(56)});
    }
    int64_t until = now_ns() + 3 * kMs;
    while (warmIcon < list.size() && now_ns() < until) icons.mask(list[warmIcon].icon, list[warmIcon].w, list[warmIcon].h), warmIcon++;
    return warmIcon >= list.size();
  };
  while (!g_stop) {
    if (!moving && !g_shot) {
      pollfd p[2] = {{input->waitFd(), POLLIN, 0}, {loader.waitFd(), POLLIN, 0}};
      if (!warmed) {
        warmed = warmIcons() && text.warm(warmFonts, kWarmChars, 3 * kMs);
        text.flush();  // to the GPU now, not in a moving frame
        pollfd q[1] = {{input->waitFd(), POLLIN, 0}};
        if (poll(q, 1, 0) == 0) continue;
      }
      bool prefetching = covers.sendPrefetches();
      int64_t deadline = dispatch.nextDeadline();
      int timeout = prefetching ? 50 : 1000;
      if (deadline) timeout = std::max(0, std::min(timeout, int((deadline - now_ns()) / kMs) + 1));
      if (g_player.active()) timeout = std::min(timeout, 250);  // the position, the play state
      if (int64_t wake = app.wakeAt()) timeout = std::max(0, std::min(timeout, int((wake - now_ns()) / kMs) + 1));
      int r = poll(p, 2, timeout);
      if (r < 0 && errno == EINTR) continue;
      if (r == 0) {
        dispatch.tick(now_ns());
        if (app.tick(now_ns())) moving = true;
        if ((g_player.state().playing != 0) != status.playing) moving = true;
        if (now_ns() - lastStatus > 2 * kSec) {  // 1.0.x refreshes on its 500 ms tick
          lastStatus = now_ns();
          Status s{false, readOnline(), readBattery()};
          if (s.online != status.online || fabsf(s.battery - status.battery) > 0.001f) status = s, moving = true;
          if (getenv("RONDO_PLAYER_LOG")) {
            PlayerState ps = g_player.state();
            printf("player: %s %d/%d %lld/%lld ms vol %d %s\n", ps.playing ? "playing" : "paused", ps.pos, ps.count,
                   (long long)ps.positionMs, (long long)ps.durationMs, ps.volume, ps.error);
          }
        }
        if (!moving) continue;
      }
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
    float dt = wasMoving && lastPresent ? std::min(0.1f, float(present - lastPresent) / 1e9f) : float(period) / 1e9f;
    bool anim = app.step(dt);
    canvas.begin(pal.shellBg);
    Env env{canvas, text, icons, covers, pal, present};
    status.playing = g_player.state().playing != 0;
    app.draw(env, float(display->width()), float(display->height()), status);
    int64_t b = now_ns();
    text.flush();
    bool more = covers.pump(present, 2);
    canvas.end();
    int64_t c = now_ns();
    if (g_shot) {
      g_shot = 0;
      printf("rondo: screenshot %s\n", saveScreenshot(shotPath, display->width(), display->height()) ? "saved" : "failed");
    }
    display->present();
    int64_t d = now_ns();
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
