// MediaTek's thermal and battery warnings, which the stock firmware's MTKThermalManager and BatteryWarning
// apps showed (decompiled Nov 9: private/recon/mtk-apps) and Rondo 2.0, with no Android to run them,
// shows itself. The detection stays MediaTek's: the kernel's thermal policy (init's thermal_manager) and
// the battery driver.
//
// - Overheating: the kernel's shutdown cooler sends SIGIO (si_code 1) to the pid in
//   /proc/driver/mtk_cl_sd_pid when the chip must cool down. MediaTek's thermald took it and opened
//   "Thermal Warning! Phone needs to cool down soon!": Ok shut down at once, and an alarm set before the
//   dialog shut down 30 s later either way (Cancel did nothing). The firmware's Rondo registers its own
//   pid there, warns in the Y1's words and shuts down at 30 s, with the countdown on screen.
// - The charger and the battery: the driver's /sys/devices/platform/mt-battery/BatteryNotify, read every
//   10 s as MediaTek's batterywarning daemon did (1 charger over voltage, 2 battery over temperature,
//   4 over-current protection, 8 battery over voltage, 16 charge time too long; other values nothing).
//   The warning comes back on every reading while the fault lasts (Snooze: until the next one), Dismiss
//   keeps that one quiet until a restart, and the charger's two close when it's unplugged.
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>

#include "../platform/haptics.h"
#include "../platform/power.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

volatile sig_atomic_t g_thermalSignal = 0;  // 1 the kernel's, 2 a development test
bool g_acceptUserSignal = false;  // development: `kill -IO <pid>` stands in for the kernel
bool g_test = false;              // that test: everything but the shutdown itself

void onThermal(int, siginfo_t* info, void*) {
  if (!info) return;
  if (info->si_code == 1) g_thermalSignal = 1;
  else if (g_acceptUserSignal && info->si_code == SI_USER) g_thermalSignal = 2;
}

// A warning: its message in full, wrapped (up to four lines), above its choices
class WarningScreen : public ShellListScreen {
 public:
  WarningScreen(std::string title, std::string message) : ShellListScreen(std::move(title)), message_(std::move(message)) {}
  bool keepScreenOn() const override { return true; }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.shellBg);
    float x = float(e.dp(12)), y = float(e.dp(10));
    e.c.layer(2);
    for (const Label& l : wrap(e, message_, 14, false, w - 2 * x, 4)) {
      drawLabel(e, l, x, y, e.p.text);
      y += l.h;
    }
    y += float(e.dp(10));
    e.c.layer(0);
    e.c.fill({0, y - 1, w, 1}, e.p.rowLine);
    list_.layout(e, w, h - y);
    list_.draw(e, 0, y);
  }

 protected:
  std::string message_;
};

int64_t g_shutdownAt = 0;  // the thermal warning's 30 s
class ThermalScreen;
ThermalScreen* g_thermalScreen = nullptr;

// ShutDownAlertDialogActivity in the Y1's words: the warning, Cancel (the shutdown comes anyway, as
// MediaTek's alarm did) and Shut down now
class ThermalScreen : public WarningScreen {
 public:
  ThermalScreen() : WarningScreen("Too hot", "The Y1 is too hot and has to cool down. It shuts down by itself in a moment.") {
    g_thermalScreen = this;
    render();
  }
  ~ThermalScreen() override {
    if (g_thermalScreen == this) g_thermalScreen = nullptr;
  }
  bool tick(int64_t now) override {
    int left = int((g_shutdownAt - now + kSec - 1) / kSec);
    if (left == shown_) return false;
    render();
    return true;
  }
  int64_t wakeAt() const override { return now_ns() + 250 * kMs; }

 private:
  void render() {
    shown_ = std::max(0, int((g_shutdownAt - now_ns() + kSec - 1) / kSec));
    Row cancel;
    cancel.title = "Cancel";
    cancel.subtitle = "Shuts down in " + std::to_string(shown_) + (shown_ == 1 ? " second" : " seconds") + " anyway";
    cancel.onSelect = [] { g_app->pop(); };
    Row now;
    now.title = "Shut down now";
    now.tile = &tiles::power;
    now.onSelect = [] { g_shutdownAt = now_ns(); };
    list_.setRows({cancel, now}, true);  // Cancel first, as every question here: a stray press keeps things
  }
  int shown_ = -1;
};

struct BatteryFault {
  int value;
  const char* title;
  const char* message;
  bool charger;  // closes by itself when the charger is unplugged
};
// MediaTek's five, in the Y1's words: its battery is built in, so "remove the battery" becomes what an
// owner can do (unplug, let it cool, switch it off)
constexpr BatteryFault kFaults[] = {
    {1, "Charger voltage too high", "The charger gives the Y1 too much voltage. Unplug it and use another charger.", true},
    {2, "Battery too hot", "The Y1's battery is too hot. Unplug the charger and let the Y1 cool down.", false},
    {4, "Battery current too high", "The Y1's battery draws too much current. Unplug the charger and switch the Y1 off for a while.", false},
    {8, "Battery voltage too high", "The Y1's battery voltage is too high. Unplug the charger.", false},
    {16, "Charging takes too long", "The Y1 has been charging for too long. Unplug the charger, then plug it in again later.", true},
};
bool g_dismissed[5] = {false, false, false, false, false};
class BatteryWarningScreen;
BatteryWarningScreen* g_batteryScreen = nullptr;

// BatteryWarningActivity: the fault's title, its message, Snooze and Dismiss
class BatteryWarningScreen : public WarningScreen {
 public:
  explicit BatteryWarningScreen(int type) : WarningScreen(kFaults[type].title, kFaults[type].message), type_(type) {
    g_batteryScreen = this;
    Row snooze;
    snooze.title = "Snooze";
    snooze.subtitle = "Again in 10 s while it lasts";
    snooze.onSelect = [] { g_app->pop(); };
    Row dismiss;
    dismiss.title = "Dismiss";
    dismiss.subtitle = "Not again until Rondo restarts";
    dismiss.onSelect = [type] {
      g_dismissed[type] = true;
      g_app->pop();
    };
    list_.setRows({snooze, dismiss});
  }
  ~BatteryWarningScreen() override {
    if (g_batteryScreen == this) g_batteryScreen = nullptr;
  }
  int type() const { return type_; }

 private:
  int type_;
};

int readNotify() {
  // development: RONDO_BATTERY_NOTIFY=<value> stands in for the driver's file
  static const char* fake = g_development ? getenv("RONDO_BATTERY_NOTIFY") : nullptr;
  if (fake) return atoi(fake);
  int fd = open("/sys/devices/platform/mt-battery/BatteryNotify", O_RDONLY);
  if (fd < 0) return 0;
  char b[16] = {0};
  ssize_t n = read(fd, b, sizeof(b) - 1);
  close(fd);
  return n > 0 ? atoi(b) : 0;
}

// The driver logs every reading of the file, and the log holds up the core it runs on for long enough
// that the display's interrupt (the first core's) came a refresh late: a missed refresh every 10 s at
// 60 Hz (Nov 9). So it is read on its own thread on the second core, which is in while the screen is
// on (power.h); with the screen off (one core) nothing is drawn.
std::atomic<int> g_notify{0};
void* notifyLoop(void*) {
  cpu_set_t second;
  CPU_ZERO(&second);
  CPU_SET(1, &second);
  for (;;) {
    sched_setaffinity(0, sizeof(second), &second);  // again each time: the core may have been out
    g_notify = readNotify();
    sleep(10);
  }
  return nullptr;
}

bool topIs(Screen* s) { return s && g_app->top() == s; }

}  // namespace

void safetyStart(bool firmware) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = onThermal;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGIO, &sa, nullptr);
  pthread_t t;
  pthread_create(&t, nullptr, notifyLoop, nullptr);
  pthread_detach(t);
  g_acceptUserSignal = !firmware;
  if (!firmware) return;  // on 1.0.x's firmware its own thermald keeps the registration
  // what MediaTek's ServiceStarter did at boot, then the registration thermald made (its pid, decimal)
  if (int fd = open("/proc/driver/mtk_cl_sd_rst", O_WRONLY); fd >= 0) (void)!write(fd, "1", 1), close(fd);
  if (int fd = open("/proc/driver/mtk_cl_sd_pid", O_WRONLY); fd >= 0) {
    char b[16];
    int n = snprintf(b, sizeof(b), "%d", int(getpid()));
    (void)!write(fd, b, size_t(n));
    close(fd);
  }
}

bool safetyTick(int64_t now) {
  bool changed = false;
  if (g_thermalSignal) {
    g_test = g_thermalSignal == 2;
    g_thermalSignal = 0;
    fprintf(stderr, "rondo: the kernel says the chip must cool down: shutting down in 30 s\n");
    if (!g_shutdownAt) g_shutdownAt = now + 30 * kSec;
    wakeScreen();
    haptics::vibrate(400);
    if (!topIs(g_thermalScreen)) g_app->push(std::unique_ptr<Screen>(new ThermalScreen()));
    changed = true;
  }
  if (g_shutdownAt && now >= g_shutdownAt) {
    g_shutdownAt = 0;
    if (g_test) {
      fprintf(stderr, "rondo: the thermal test would shut down now\n");
      g_app->toast("A real warning would shut down now");
      if (topIs(g_thermalScreen)) g_app->pop();
    } else {
      shutDownNow("the chip must cool down");
    }
    changed = true;
  }
  static int64_t lastRead = 0;
  if (now - lastRead >= 10 * kSec) {
    lastRead = now;
    int v = g_notify;
    int type = -1;
    for (int i = 0; i < 5; i++)
      if (kFaults[i].value == v) type = i;
    // the charger's warnings go when it does
    if (g_batteryScreen && kFaults[g_batteryScreen->type()].charger && !power::battery().plugged && topIs(g_batteryScreen)) {
      g_app->pop();
      changed = true;
    }
    if (type >= 0 && !g_dismissed[type] && !g_batteryScreen) {
      fprintf(stderr, "rondo: battery warning: %s\n", kFaults[type].title);
      wakeScreen();
      haptics::vibrate(250);
      g_app->push(std::unique_ptr<Screen>(new BatteryWarningScreen(type)));
      changed = true;
    }
  }
  return changed;
}

}  // namespace rondo::shell
