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
  if (s.wifiEnabled != 2) return tr("common.off");
  return s.wifiState > 0 && s.ssid[0] ? std::string(s.ssid) : tr("wifi.notConnected");
}

namespace {

// rondo-core's wifiSignal() in the language spoken (rondo-core speaks only English)
std::string signalName(int rssi) {
  switch (wifiLevel(rssi)) {
    case 3: return tr("wifi.signal.strong");
    case 2: return tr("wifi.signal.good");
    case 1: return tr("wifi.signal.fair");
    default: return tr("wifi.signal.weak");
  }
}

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
    auto info = [&](std::string title, std::string trailing) {
      Row r;
      r.title = std::move(title);
      r.trailing = std::move(trailing);
      r.enabled = false;
      rows.push_back(std::move(r));
    };
    if (connected) {
      info(tr("wifi.network.status"), tr("wifi.connected"));
      info(tr("wifi.network.signal"), signalName(s.rssi));
      info(tr("wifi.network.speed"), std::to_string(s.linkSpeed) + " Mb/s");
      info(tr("wifi.network.ipAddress"), ipText(s.ip));
    } else if (saved) {
      Row c;
      c.title = tr("wifi.network.connect");
      int id = saved->id;
      c.onSelect = [id] {
        g_core.wifiConnect(id);
        g_app->pop();
      };
      rows.push_back(c);
    }
    if (saved) {
      Row f;
      f.title = tr("wifi.network.forget");
      f.subtitle = tr("wifi.network.forgetNote");
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
  // Rondo's own keyboard, without emoji: a Wi-Fi password is letters, digits and symbols only
  WifiPasswordScreen(std::string ssid, WifiSecurity security)
      : ssid_(std::move(ssid)), security_(security), kb_(tr("wifi.password.join"), Keyboard::Style::Shell, false) {}
  std::string title() const override { return tr("wifi.password.title", {{"network", ssid_}}); }
  bool onAction(Action a, int steps) override {
    Keyboard::Result r;  // TvKeyboard.handle()
    if (!keyboardAction(kb_, a, steps, &r)) return false;
    if (r == Keyboard::Result::Action) {
      size_t minimum = security_ == WifiSecurity::Psk ? 8 : 1;
      if (Keyboard::characters(kb_.typed()) < minimum) {
        note_ = tr("wifi.password.atLeast", {{"count", minimum}});
        return true;
      }
      g_core.wifiJoin(ssid_, security_, kb_.typed());
      g_app->pop();
      return true;
    }
    if (r == Keyboard::Result::Typed) kb_.keepAtMost(63);  // WPA's longest passphrase
    if (r == Keyboard::Result::Typed || r == Keyboard::Result::Delete) note_.clear();
    return true;
  }
  bool step(float dt) override { return kb_.step(dt); }
  // as the shell's other text entries (TextEntryScreen): the field, a line under it (what's wrong, in the
  // accent color), the keyboard
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    // the field: 26 units tall, 8 in from the sides and 5 from the top, rounded 8 with a 1 px edge
    float bx = float(e.dp(8)), by = float(e.dp(5)), bw = w - 2 * bx, bh = float(e.dp(26));
    e.c.shape({bx, by, bw, bh}, e.dpf(8), p.rowLine);
    e.c.shape({bx + 1, by + 1, bw - 2, bh - 2}, e.dpf(8) - 1, p.header);
    e.c.layer(3);
    Label field = label(e, kb_.typed() + "|", 14, false, bw - 2 * e.dp(10));
    drawLabel(e, field, bx + e.dp(10), by + float((int(bh) - field.h) / 2), p.text);
    float sy = by + bh + e.dp(3);
    std::string line = !note_.empty() ? note_ : security_ == WifiSecurity::Psk ? tr("wifi.password.shown") : "";
    Label msg = label(e, line.empty() ? " " : line, 10, false, w - 2 * e.dp(8));
    drawLabel(e, msg, centerText(float(e.dp(8)), w - 2 * e.dp(8), msg.w), sy, note_.empty() ? p.muted : p.accent);
    drawKeyboard(e, kb_, sy + msg.h + e.dp(3), w, h, tr("wifi.password.keyboardHint"), p.muted, p.text);
    e.c.layer(0);
  }

 private:
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
    onOff.trailing = enabled ? tr("common.on") : s.wifiEnabled == 1 ? tr("wifi.turningOn") : tr("common.off");
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
      h.title = nets.empty() ? tr("wifi.searching") : tr("wifi.chooseNetwork");
      rows.push_back(h);
      for (const WifiNetwork& n : nets) {
        std::string ssid = n.ssid;
        auto security = WifiSecurity(n.security);
        const WifiSaved* saved = savedNamed(s, ssid);
        bool isCurrent = ssid == current;
        bool failed = ssid == s.failedSsid;
        std::string status = isCurrent             ? (s.wifiState == 4   ? tr("wifi.connected")
                                                      : s.wifiState == 3 ? tr("wifi.status.gettingAddress")
                                                      : s.wifiState == 2 ? tr("wifi.status.checkingPassword")
                                                                         : tr("wifi.status.connecting"))
                             : failed              ? tr("wifi.status.wrongPassword")
                             : saved               ? tr("wifi.status.saved")
                             : security == WifiSecurity::Open ? tr("wifi.status.open")
                             : security == WifiSecurity::Eap  ? tr("wifi.status.enterprise")
                                                              : tr("wifi.status.secured");
        Row r;
        r.title = ssid;
        r.subtitle = status;
        r.trailing = signalName(n.rssi);
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
