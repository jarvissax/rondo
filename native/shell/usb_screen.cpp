// The USB drive screen (UsbDriveScreen.kt in 1.0.x), ported 1:1, with what ShellActivity's USB receiver
// did: a computer on the cable offers the screen; pulling the cable gives the card back and closes it.
#include <atomic>
#include <thread>

#include "../platform/usb_drive.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

// the switch runs on a thread of its own (vold takes seconds); 0 idle, 1 switching, 2 done
std::atomic<int> g_switch{0};

void startSwitch(bool on) {
  g_switch = 1;
  std::thread([on] {
    usb::setShared(on);
    g_switch = 2;
  }).detach();
}

// the card is back: the library reads it again (songs added on the computer appear)
void rescanCard() { g_library.scanAsync(); }

// what plays is a song on the card (streamed songs and videos keep playing: 1.0.x's isLocal)
bool cardPlaying() {
  PlayerState s = g_player.state();
  return s.path[0] && !isStream(s.path);
}

class UsbDriveScreen : public ShellListScreen {
 public:
  UsbDriveScreen() : ShellListScreen("USB drive") {
    on_ = usb::shared();
    render();
  }
  bool onAction(Action a, int steps) override {
    // nothing on the card can play while the computer has it (a streamed queue goes on)
    if ((on_ || switching_) && cardPlaying() &&
        (a == Action::PlayPause || a == Action::Next || a == Action::Prev || a == Action::LongNext || a == Action::LongPrev))
      return true;
    return ShellListScreen::onAction(a, steps);
  }
  bool tick(int64_t) override {
    if (!switching_ || g_switch != 2) return false;
    g_switch = 0;
    switching_ = false;
    on_ = usb::shared();
    if (!on_) rescanCard();
    render();
    return true;
  }
  int64_t wakeAt() const override { return switching_ ? now_ns() + 100 * kMs : 0; }

 private:
  void render() {
    std::vector<Row> rows;
    auto add = [&](const char* title, const char* subtitle, const Tile* tile, bool enabled, std::function<void()> on) {
      Row r;
      r.title = title;
      if (subtitle) r.subtitle = subtitle;
      r.tile = tile;
      r.enabled = enabled;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    if (switching_) {
      add(target_ ? "Turning on\xe2\x80\xa6" : "Turning off\xe2\x80\xa6", nullptr, &tiles::device, false, nullptr);
    } else if (on_) {
      add("USB drive is on", "Copy files on your computer", &tiles::device, false, nullptr);
      add("Turn off USB drive", "Eject it on the computer first", nullptr, true, [this] { switchTo(false); });
      Row h;
      h.header = true;
      h.title = "While it's on";
      rows.push_back(h);
      add("Music on the card is paused", "It comes back when the drive is off", nullptr, false, nullptr);
    } else {
      add("Use as USB drive", "Copy files from your computer", &tiles::device, true, [this] { switchTo(true); });
      add("Not now", "Keep listening while it charges", nullptr, true, [] { g_app->pop(); });
    }
    list_.setRows(std::move(rows));
  }
  void switchTo(bool on) {
    if (switching_ || g_switch != 0) return;
    // 1.0.x paused the music; here the song's file is closed too, as vold kills whoever keeps one open
    if (on && g_player.active() && cardPlaying()) g_player.release();
    switching_ = true;
    target_ = on;
    render();
    startSwitch(on);
  }
  bool on_ = false, switching_ = false, target_ = false;
};

}  // namespace

std::unique_ptr<Screen> makeUsbDrive() { return std::unique_ptr<Screen>(new UsbDriveScreen()); }

void usbCableChanged(bool computer) {
  if (computer) {
    if (g_app->topTitle() != "USB drive") g_app->push(makeUsbDrive());
    return;
  }
  // the cable came out: the card is the Y1's again
  if (g_switch == 0 && usb::shared()) startSwitch(false);
  if (g_app->topTitle() == "USB drive") g_app->pop();
}

bool usbIdle() {
  // a switch that ended with no USB drive screen to see it (the cable came out): the card, back, is
  // read again
  if (g_switch != 2 || g_app->topTitle() == "USB drive") return false;
  g_switch = 0;
  if (!usb::shared()) rescanCard();
  return true;
}

}  // namespace rondo::shell
