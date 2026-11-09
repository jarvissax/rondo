// The screen's power and the Power screen: what Android's PowerManagerService, PhoneWindowManager and
// BatteryService did for 1.0.x, and PowerScreen.kt.
//
// - The screen dims, then goes off, after Settings' "Screen off after" without input (Android's dim:
//   the last fifth, at most 7 s, at brightness 10); the video player and Shorts keep it on.
// - Black or asleep, any press (center, which is also the Y1's power button, any other button, the
//   wheel) only wakes the screen: that press and its release do nothing else (no lyrics opening, no song
//   skipped). The wheel, center and Menu are also swallowed for 600 ms after
//   waking. Power (KEY_POWER, if it comes) turns the screen off; held, it opens the Power screen.
// - Off, the backlight goes first (a key brings it back at once); 10 s later early suspend. A wakelock
//   is held while anything plays.
// - The battery: shut down at 0 % unplugged and above 68 °C (BatteryService), "Battery low" at 15 %.
#include <set>

#include "../platform/power.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {

void App::drawToast(Env& e, float W, float H) {
  Label l = label(e, toast_, 14, false, W - e.dp(48));
  int padX = e.dp(16), padY = e.dp(10);
  int bw = l.w + 2 * padX, bh = l.h + 2 * padY;
  float x = float((int(W) - bw) / 2), y = H - e.dp(64) - bh;
  e.c.layer(7);
  e.c.shape({x, y, float(bw), float(bh)}, e.dpf(4), 0xe6262626);
  drawLabel(e, l, x + padX, y + padY, 0xffffffff);
  e.c.layer(0);
}

std::function<void()> g_onSleep;

namespace {

enum Stage { kOn, kDim, kOff, kSuspended };
Stage g_stage = kOn;
int64_t g_activity = 0, g_offAt = 0, g_wokeAt = 0, g_powerDown = 0, g_lastBattery = 0;
bool g_powerLong = false, g_backlightPending = false;
std::set<Key> g_swallowing;
int g_emptyReadings = 0, g_hotReadings = 0;
bool g_lowWarned = false;
int g_lastLevel = -1;

constexpr int64_t kDimBrightness = 10;  // config_screenBrightnessDim

bool navigation(Key k) { return k == Key::WheelCw || k == Key::WheelCcw || k == Key::Center || k == Key::Menu; }

void goToSleep(int64_t now) {
  g_stage = kOff;
  g_offAt = now;
  power::setBacklight(0);
  if (g_onSleep) g_onSleep();
  g_core.screen(false);  // Wi-Fi saves power (Android's suspend optimizations)
}

void wake(int64_t now) {
  if (g_stage == kSuspended) power::suspend(false);  // the panel comes back with late resume (0.54 s)
  if (g_stage >= kOff) g_core.screen(true);
  g_stage = kOn;
  g_wokeAt = g_activity = now;
  g_backlightPending = true;  // once a frame of what is there now has been drawn
}

class PowerScreen;
PowerScreen* g_powerScreen = nullptr;  // the one open (no RTTI to ask the stack)

// PowerScreen: shut down or restart. Cancel is first, so a stray center press does nothing.
class PowerScreen : public ShellListScreen {
 public:
  PowerScreen() : ShellListScreen("Power") {
    g_powerScreen = this;
    render();
  }
  ~PowerScreen() override {
    if (g_powerScreen == this) g_powerScreen = nullptr;
  }
  bool tick(int64_t now) override {
    if (goAt_ && now >= goAt_) {
      goAt_ = 0;
      saveConfigNow();
      savePrefsNow();
      if (restart_) power::reboot();
      else power::shutdown();
    }
    return false;
  }
  int64_t wakeAt() const override { return goAt_; }
  // the battery's or the heat's shutdown: the message, then off
  void go(const std::string& message, bool restart) {
    PlayerState s = g_player.state();
    if (s.playing) g_player.togglePlay();
    if (s.fmOn) g_player.fmStop();
    list_.setMessage(message);
    restart_ = restart;
    goAt_ = g_app->now() + 400 * kMs;  // the message is drawn first
  }

 private:
  void render() {
    std::vector<Row> rows;
    Row cancel;
    cancel.title = "Cancel";
    cancel.onSelect = [] { g_app->pop(); };
    rows.push_back(cancel);
    Row off;
    off.title = "Shut down";
    off.tile = &tiles::power;
    off.onSelect = [this] { go("Shutting down\xe2\x80\xa6", false); };
    rows.push_back(off);
    Row again;
    again.title = "Restart";
    again.tile = &tiles::restart;
    again.onSelect = [this] { go("Restarting\xe2\x80\xa6", true); };
    rows.push_back(again);
    list_.setRows(std::move(rows));
  }
  int64_t goAt_ = 0;
  bool restart_ = false;
};

PowerScreen* powerScreenOnTop() { return g_powerScreen && g_app->top() == g_powerScreen ? g_powerScreen : nullptr; }

void shutDownFor(const char* why) {
  fprintf(stderr, "rondo: shutting down (%s)\n", why);
  if (g_stage >= kOff) wake(now_ns());
  PowerScreen* p = powerScreenOnTop();
  if (!p) {
    g_app->push(std::unique_ptr<Screen>(p = new PowerScreen()));
  }
  p->go("Shutting down\xe2\x80\xa6", false);
}

}  // namespace

std::unique_ptr<Screen> makePower() { return std::unique_ptr<Screen>(new PowerScreen()); }

void wakeScreen() {
  int64_t now = now_ns();
  if (g_stage >= kOff) wake(now);
  g_activity = now;
  if (g_stage == kDim) {
    g_stage = kOn;
    applyBrightness(g_prefs.brightness);
  }
}
void shutDownNow(const char* why) { shutDownFor(why); }

// the dashboard's Shut down and Restart: the Power screen with its message, then off
void powerAction(bool restart) {
  if (g_stage >= kOff) wake(now_ns());
  PowerScreen* p = powerScreenOnTop();
  if (!p) g_app->push(std::unique_ptr<Screen>(p = new PowerScreen()));
  p->go(restart ? "Restarting\xe2\x80\xa6" : "Shutting down\xe2\x80\xa6", restart);
}

// A wired headset's buttons (PlayerService.onMediaButton): they act on what plays whether the screen
// is on or not, and never wake it (they aren't the Y1's own buttons); the volume shows its panel
// only while the screen is lit
void headsetButton(Key k) {
  switch (k) {
    case Key::HeadsetPlay: g_player.togglePlay(); break;
    case Key::HeadsetNext: g_player.next(); break;
    case Key::HeadsetPrev: g_player.previous(); break;
    case Key::HeadsetVolumeUp:
    case Key::HeadsetVolumeDown: {
      int dir = k == Key::HeadsetVolumeUp ? 1 : -1;
      if (g_stage <= kDim) g_app->changeVolume(dir);
      else g_player.changeVolume(dir);
      break;
    }
    default: break;
  }
}

bool powerFilter(const InputEvent& ev) {
  int64_t now = ev.time ? ev.time : now_ns();
  if (ev.key >= Key::HeadsetPlay) {
    if (ev.down) headsetButton(ev.key);
    return false;
  }
  if (ev.key == Key::Power) {
    if (ev.down) {
      if (g_stage >= kOff) {
        wake(now);
        g_powerDown = 0;
      } else {
        g_powerDown = now;
        g_powerLong = false;
      }
    } else {
      if (g_powerDown && !g_powerLong && g_stage < kOff) goToSleep(now);
      g_powerDown = 0;
    }
    return false;
  }
  if (g_stage >= kOff) {
    // black or asleep: any press only wakes it; its release is swallowed too
    if (ev.down) {
      wake(now);
      if (ev.key != Key::WheelCw && ev.key != Key::WheelCcw) g_swallowing.insert(ev.key);
    }
    return false;
  }
  g_activity = now;  // user activity: the timeout starts again, a dimmed screen brightens
  if (g_stage == kDim) {
    g_stage = kOn;
    applyBrightness(g_prefs.brightness);
  }
  if (navigation(ev.key) && now - g_wokeAt < 600 * kMs) {
    if (ev.down && ev.key != Key::WheelCw && ev.key != Key::WheelCcw) g_swallowing.insert(ev.key);
    return false;
  }
  if (g_swallowing.count(ev.key)) {
    if (!ev.down) g_swallowing.erase(ev.key);
    return false;
  }
  return true;
}

bool powerTick(int64_t now) {
  bool changed = false;
  if (!g_activity) g_activity = now;
  // Power held: the Power screen (Android's global actions after half a second)
  if (g_powerDown && !g_powerLong && now - g_powerDown >= 500 * kMs && g_stage < kOff) {
    g_powerLong = true;
    if (!powerScreenOnTop()) g_app->push(makePower()), changed = true;
  }
  // the CPU stays up while anything plays
  PlayerState s = g_player.state();
  // wired headphones pulled out: music pauses (Android's "becoming noisy"; the radio stops by itself)
  static int headset = -1;
  int jack = power::headset();
  if (headset > 0 && jack == 0 && s.playing) g_player.pause();
  // the audio policy is told, as Android's WiredAccessoryManager did: sound goes to the jack
  if (jack != headset) g_player.wired(jack);
  headset = jack;
  power::hold(s.playing || s.fmOn || s.fmStarting);
  // the timeout (Settings › Screen off after); the video player and Shorts keep the screen on
  // (development: RONDO_SCREEN_ON=1 keeps it on, for the parity scripts)
  static const bool screenOn = g_development && getenv("RONDO_SCREEN_ON") != nullptr;
  if ((screenOn || g_app->topKeepsScreenOn()) && g_stage <= kDim) g_activity = now;
  int64_t timeout = int64_t(g_prefs.screenTimeoutMs) * kMs;
  if (timeout > 0) {
    int64_t dimFor = std::min<int64_t>(7000 * kMs, timeout / 5);
    if (g_stage == kOn && now - g_activity >= timeout - dimFor) {
      g_stage = kDim;
      power::setBacklight(std::min<int>(int(kDimBrightness), g_prefs.brightness));
    }
    if (g_stage == kDim && now - g_activity >= timeout) goToSleep(now);
  }
  if (g_stage == kOff && now - g_offAt >= 10000 * kMs) {
    g_stage = kSuspended;
    power::suspend(true);
  }
  // the battery, every 2 s
  if (now - g_lastBattery >= 2000 * kMs) {
    g_lastBattery = now;
    power::Battery b = power::battery();
    g_emptyReadings = !b.plugged && b.level <= 0 ? g_emptyReadings + 1 : 0;
    g_hotReadings = b.tempTenths > 680 ? g_hotReadings + 1 : 0;
    if (g_emptyReadings >= 2) shutDownFor("the battery is empty"), changed = true;
    else if (g_hotReadings >= 2) shutDownFor("the battery is too hot"), changed = true;
    // ACTION_BATTERY_LOW: crossing 15 % unplugged
    if (!b.plugged && b.level <= 15 && g_lastLevel > 15 && !g_lowWarned) {
      g_lowWarned = true;
      g_app->toast("Battery low. Plug in to charge.");
      changed = true;
    }
    if (b.plugged || b.level > 15) g_lowWarned = false;
    g_lastLevel = b.plugged ? 100 : b.level;
  }
  return changed;
}

int64_t powerWakeAt() {
  int64_t timeout = int64_t(g_prefs.screenTimeoutMs) * kMs;
  int64_t t = 0;
  auto soonest = [&](int64_t v) {
    if (v && (!t || v < t)) t = v;
  };
  if (g_powerDown && !g_powerLong) soonest(g_powerDown + 500 * kMs);
  if (timeout > 0 && g_stage == kOn) soonest(g_activity + timeout - std::min<int64_t>(7000 * kMs, timeout / 5));
  if (timeout > 0 && g_stage == kDim) soonest(g_activity + timeout);
  if (g_stage == kOff) soonest(g_offAt + 10000 * kMs);
  return t;
}

bool screenDrawing() { return g_stage <= kDim && power::drawingAllowed(); }

bool powerWantsFrame() { return g_backlightPending && power::drawingAllowed(); }

void framePresented() {
  if (!g_backlightPending || g_stage > kDim) return;
  g_backlightPending = false;
  applyBrightness(g_prefs.brightness);
  g_wokeAt = now_ns();  // the 600 ms count from when the screen is lit (1.0.x: ACTION_SCREEN_ON)
}

}  // namespace rondo::shell
