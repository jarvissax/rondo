// Wi-Fi from the wheel (WifiScreens.kt in 1.0.x), ported 1:1 over rondo-core's Wi-Fi: the networks in
// range, a saved network's details and Forget, and the password on the TV keyboard.
#include <arpa/inet.h>
#include <string.h>

#include <algorithm>

#include "../ui/keyboard.h"
#include "app.h"
#include "music_list.h"
#include "screens.h"

namespace rondo::shell {

std::string wifiName() {
  CoreState s = g_core.state();
  if (s.wifiEnabled != 2) return "Off";
  return s.wifiState > 0 && s.ssid[0] ? std::string(s.ssid) : "Not connected";
}

namespace {

const WifiSaved* savedNamed(const CoreState& s, const std::string& ssid) {
  for (int i = 0; i < s.savedCount; i++)
    if (ssid == s.saved[i].ssid) return &s.saved[i];
  return nullptr;
}

std::string ipText(uint32_t ip) {
  in_addr a;
  a.s_addr = ip;
  return inet_ntoa(a);
}

// a list that follows rondo-core's Wi-Fi state: drawn again whenever it changes
class WifiList : public ShellListScreen {
 public:
  using ShellListScreen::ShellListScreen;
  bool tick(int64_t now) override {
    CoreState s = g_core.state();
    if (s.serial == serial_) return false;
    serial_ = s.serial;
    render(s);
    return true;
  }

 protected:
  virtual void render(const CoreState& s) = 0;
  uint32_t serial_ = 0;
};

// WifiNetworkScreen: a saved network: what it is, and Forget.
class WifiNetworkScreen : public WifiList {
 public:
  explicit WifiNetworkScreen(std::string ssid) : WifiList(ssid), ssid_(std::move(ssid)) { render(g_core.state()); }

 protected:
  void render(const CoreState& s) override {
    bool connected = s.wifiState == 4 && ssid_ == s.ssid;
    const WifiSaved* saved = savedNamed(s, ssid_);
    std::vector<Row> rows;
    auto info = [&](const char* title, std::string trailing) {
      Row r;
      r.title = title;
      r.trailing = std::move(trailing);
      r.enabled = false;
      rows.push_back(std::move(r));
    };
    if (connected) {
      info("Status", "Connected");
      info("Signal", wifiSignal(s.rssi));
      info("Speed", std::to_string(s.linkSpeed) + " Mb/s");
      info("IP address", ipText(s.ip));
    } else if (saved) {
      Row c;
      c.title = "Connect";
      int id = saved->id;
      c.onSelect = [id] {
        g_core.wifiConnect(id);
        g_app->pop();
      };
      rows.push_back(c);
    }
    if (saved) {
      Row f;
      f.title = "Forget this network";
      f.subtitle = "The password will be needed again";
      int id = saved->id;
      f.onSelect = [id] {
        g_core.wifiForget(id);
        g_app->pop();
      };
      rows.push_back(f);
    }
    list_.setRows(std::move(rows), true);
  }

 private:
  std::string ssid_;
};

// WifiPasswordScreen: the password on the TV keyboard, shown as typed (nobody looks over the shoulder
// of a 2.4" screen, and typos are hard to spot otherwise).
class WifiPasswordScreen : public Screen {
 public:
  WifiPasswordScreen(std::string ssid, WifiSecurity security) : ssid_(std::move(ssid)), security_(security), kb_("Join") {}
  std::string title() const override { return "Password for " + ssid_; }
  bool statusBar() const override { return false; }
  bool onAction(Action a, int steps) override {
    switch (a) {  // TvKeyboard.handle()
      case Action::Down: kb_.nextKey(); return true;
      case Action::Up: kb_.prevKey(); return true;
      case Action::Next:
      case Action::LongNext: kb_.right(); return true;
      case Action::Prev:
      case Action::LongPrev: kb_.left(); return true;
      case Action::PlayPause: kb_.down(); return true;
      case Action::Back: return kb_.up();
      case Action::Select: {
        Keyboard::Result r = kb_.press();
        if (r == Keyboard::Result::Action) {
          size_t minimum = security_ == WifiSecurity::Psk ? 8 : 1;
          if (length(kb_.typed()) < minimum) {
            note_ = "At least " + std::to_string(minimum) + " characters";
            return true;
          }
          g_core.wifiJoin(ssid_, security_, kb_.typed());
          g_app->pop();
          return true;
        }
        if (r == Keyboard::Result::Typed && length(kb_.typed()) > 63) {
          std::string t = kb_.typed();
          while (!t.empty() && (uint8_t(t.back()) & 0xc0) == 0x80) t.pop_back();
          if (!t.empty()) t.pop_back();
          kb_.setTyped(t);
        }
        note_.clear();
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return kb_.step(dt); }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.dark);
    darkHeader(e, w, title(), HeaderLogo::Wifi, p.white, 28);
    // the field: 28 units tall, 8 in from the sides, 2 below the header and 5 above the keyboard
    float bx = float(e.dp(8)), by = float(e.dp(28) + e.dp(2)), bw = w - 2 * bx, bh = float(e.dp(28));
    e.c.layer(2);
    e.c.shape({bx, by, bw, bh}, e.dpf(14), p.raised);
    e.c.layer(4);
    Label field = label(e, note_.empty() ? kb_.typed() + "|" : note_, 14, false, bw - 2 * e.dp(10));
    drawLabel(e, field, bx + e.dp(10), by + float((int(bh) - field.h) / 2), p.white);
    float area = by + bh + e.dp(5);
    Label hint = label(e, "ABC: capitals \xc2\xb7 #+=: symbols \xc2\xb7 Menu up \xc2\xb7 Play down \xc2\xb7 Prev/Next left/right", 9, false, w);
    float kh = h - area - (e.dp(3) + hint.h + e.dp(4));
    e.c.layer(1);
    kb_.draw(e.c, e.text, p, {float(e.dp(5)), area, w - 2 * e.dp(5), kh}, e.dp(15), e.dp(12));
    e.c.layer(4);
    drawLabel(e, hint, centerText(0, w, hint.w), area + kh + e.dp(3), p.darkMuted);
    e.c.layer(0);
  }

 private:
  static size_t length(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += (uint8_t(c) & 0xc0) != 0x80;
    return n;
  }
  std::string ssid_, note_;
  WifiSecurity security_;
  Keyboard kb_;
};

// WifiScreen: Wi-Fi on or off, then the networks in range (one row per name, strongest first, the
// current one on top); a fresh scan every 15 s while it is open.
class WifiScreen : public WifiList {
 public:
  WifiScreen() : WifiList("Wi-Fi") { render(g_core.state()); }
  void onShow() override {
    g_core.wifiScan();
    lastScan_ = now_ns();
    render(g_core.state());
  }
  bool tick(int64_t now) override {
    if (now - lastScan_ > 15 * kSec) {
      g_core.wifiScan();
      lastScan_ = now;
    }
    return WifiList::tick(now);
  }

 protected:
  void render(const CoreState& s) override {
    bool enabled = s.wifiEnabled == 2;
    std::vector<Row> rows;
    Row onOff;
    onOff.title = "Wi-Fi";
    onOff.trailing = enabled ? "On" : s.wifiEnabled == 1 ? "Turning on\xe2\x80\xa6" : "Off";
    onOff.tile = &tiles::wifi;
    onOff.onSelect = [enabled] {
      g_core.wifiEnable(!enabled);
      g_prefs.wifiOn = !enabled;  // remembered, as Android's wifi_on was
      savePrefsNow();
    };
    rows.push_back(onOff);
    if (enabled) {
      std::string current = s.wifiState > 0 ? s.ssid : "";
      std::vector<WifiNetwork> nets(s.networks, s.networks + s.networkCount);
      std::stable_sort(nets.begin(), nets.end(), [&](const WifiNetwork& a, const WifiNetwork& b) {
        bool ca = current == a.ssid, cb = current == b.ssid;
        if (ca != cb) return ca;
        return a.rssi > b.rssi;
      });
      Row h;
      h.header = true;
      h.title = nets.empty() ? "Searching\xe2\x80\xa6" : "Choose a network";
      rows.push_back(h);
      for (const WifiNetwork& n : nets) {
        std::string ssid = n.ssid;
        auto security = WifiSecurity(n.security);
        const WifiSaved* saved = savedNamed(s, ssid);
        bool isCurrent = ssid == current;
        bool failed = ssid == s.failedSsid;
        std::string status = isCurrent             ? (s.wifiState == 4   ? "Connected"
                                                      : s.wifiState == 3 ? "Getting address\xe2\x80\xa6"
                                                      : s.wifiState == 2 ? "Checking password\xe2\x80\xa6"
                                                                         : "Connecting\xe2\x80\xa6")
                             : failed              ? "Wrong password"
                             : saved               ? "Saved"
                             : security == WifiSecurity::Open ? "Open"
                             : security == WifiSecurity::Eap  ? "Enterprise (not supported)"
                                                              : "Secured";
        Row r;
        r.title = ssid;
        r.subtitle = status;
        r.trailing = wifiSignal(n.rssi);
        r.chevron = saved != nullptr;
        r.enabled = security != WifiSecurity::Eap;
        int id = saved ? saved->id : -1;
        r.onSelect = [ssid, security, id, isCurrent, failed] {
          if (isCurrent) g_app->push(std::unique_ptr<Screen>(new WifiNetworkScreen(ssid)));
          // a saved network whose password just failed asks for the password again
          else if (id >= 0 && !failed) g_core.wifiConnect(id);
          else if (security == WifiSecurity::Open) g_core.wifiJoin(ssid, security, "");
          else g_app->push(std::unique_ptr<Screen>(new WifiPasswordScreen(ssid, security)));
        };
        if (saved) r.onLongSelect = [ssid] { g_app->push(std::unique_ptr<Screen>(new WifiNetworkScreen(ssid))); };
        rows.push_back(std::move(r));
      }
    }
    list_.setRows(std::move(rows), true);
  }

 private:
  int64_t lastScan_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeWifi() { return std::unique_ptr<Screen>(new WifiScreen()); }

}  // namespace rondo::shell
