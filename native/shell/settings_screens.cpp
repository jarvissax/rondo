// Settings (SettingsScreen.kt and ThemeScreens.kt in 1.0.x): since 2.0 in groups (Connections, YouTube,
// Sound, Display, Controls, System) under the Dashboard, the choice screens, Theme with its accent, text
// size and main menu order, and About; each system service leads to its screen.
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statfs.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <deque>

#include "app.h"
#include "screens.h"
#include "subtitles.h"

namespace rondo::shell {
namespace {

// kVersion: screens.h

std::string onOff(bool on) { return on ? "On" : "Off"; }

std::string accentName(const std::string& hex) {
  for (const AccentPreset& p : kAccentPresets)
    if (strcasecmp(p.color, hex.c_str()) == 0) return p.name;
  return "Custom";
}
std::string textSizeName(float s) { return s < 1.3f ? "Small" : s > 1.5f ? "Large" : "Medium"; }
std::string accelerationName(const std::string& l) { return l == "off" ? "Off" : l == "fast" ? "Fast" : "Normal"; }
std::string vibrationName(const std::string& l) {
  return l == "off" ? "Off" : l == "light" ? "Light" : l == "strong" ? "Strong" : "Normal";
}
std::string timeoutName(int ms) {
  char b[16];
  if (ms >= 60000) snprintf(b, sizeof(b), "%d min", ms / 60000);
  else snprintf(b, sizeof(b), "%d s", ms / 1000);
  return b;
}
int brightnessPercent() { return (g_prefs.brightness * 100 + 127) / 255; }

// a swatch in that color with the selection bar's lit top (ThemeScreens.kt swatch())
Tile swatch(const std::string& hex, bool glyph) {
  unsigned c = 0x808080;
  parseColor(hex, &c);
  Tile t{Icon::Check, lit(rgb(c)), rgb(c)};
  t.glyph = glyph;
  return t;
}

// ChoiceScreen: a list of choices with the current one marked; picking one goes back.
class ChoiceScreen : public ShellListScreen {
 public:
  struct Choice {
    std::string name, value;
  };
  ChoiceScreen(std::string title, std::vector<Choice> choices, std::function<std::string()> current,
               std::function<void(const std::string&)> pick, std::function<std::string(const std::string&)> note = nullptr)
      : ShellListScreen(std::move(title)), choices_(std::move(choices)), current_(std::move(current)), pick_(std::move(pick)),
        note_(std::move(note)) {
    render();
  }

 private:
  void render() {
    std::string now = current_();
    std::vector<Row> rows;
    int at = 0;
    for (size_t i = 0; i < choices_.size(); i++) {
      Row r;
      r.title = choices_[i].name;
      bool current = choices_[i].value == now;
      if (current) at = int(i);
      r.trailing = current ? "Current" : note_ ? note_(choices_[i].value) : "";
      std::string v = choices_[i].value;
      r.onSelect = [this, v] {
        pick_(v);
        g_app->pop();
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    list_.selectIndex(at);
  }
  std::vector<Choice> choices_;
  std::function<std::string()> current_;
  std::function<void(const std::string&)> pick_;
  std::function<std::string(const std::string&)> note_;
};

std::unique_ptr<Screen> choice(std::string title, std::vector<ChoiceScreen::Choice> choices, std::function<std::string()> current,
                               std::function<void(const std::string&)> pick,
                               std::function<std::string(const std::string&)> note = nullptr) {
  return std::unique_ptr<Screen>(new ChoiceScreen(std::move(title), std::move(choices), std::move(current), std::move(pick),
                                                  std::move(note)));
}

// AccentScreen: the dashboard's presets; a color picked there (any color) shows as Custom on top
class AccentScreen : public ShellListScreen {
 public:
  AccentScreen() : ShellListScreen("Accent color") { render(); }

 private:
  void render() {
    std::string now = g_config.theme.accent;
    bool custom = true;
    for (const AccentPreset& p : kAccentPresets)
      if (strcasecmp(p.color, now.c_str()) == 0) custom = false;
    swatches_.clear();
    std::vector<Row> rows;
    if (custom) {
      swatches_.push_back(swatch(now, true));
      Row r;
      r.title = "Custom";
      std::string up = now;
      for (char& ch : up) ch = char(toupper((unsigned char)ch));
      r.subtitle = up;
      r.trailing = "Current";
      r.tile = &swatches_.back();
      r.enabled = false;
      rows.push_back(std::move(r));
    }
    for (const AccentPreset& p : kAccentPresets) {
      bool current = strcasecmp(p.color, now.c_str()) == 0;
      swatches_.push_back(swatch(p.color, current));
      Row r;
      r.title = p.name;
      r.trailing = current ? "Current" : "";
      r.tile = &swatches_.back();
      std::string hex = p.color;
      r.onSelect = [this, hex] {
        g_config.theme.accent = hex;
        themeChanged();
        render();
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows), true);
  }
  std::deque<Tile> swatches_;  // the rows point at them
};

// MenuOrderScreen: Center shows or hides the highlighted entry (Settings always stays); Next and
// Previous move it down and up, like dragging it on the dashboard.
class MenuOrderScreen : public ShellListScreen {
 public:
  MenuOrderScreen() : ShellListScreen("Main menu") { render(); }
  bool onAction(Action a, int steps) override {
    if (a != Action::Next && a != Action::Prev) return ShellListScreen::onAction(a, steps);
    auto& menu = g_config.theme.menu;
    int at = list_.selected() - 1, to = at + (a == Action::Next ? 1 : -1);
    if (at < 0 || at >= int(menu.size()) || to < 0 || to >= int(menu.size())) return true;
    focus_ = menu[size_t(at)].id;
    MenuEntry m = menu[size_t(at)];
    menu.erase(menu.begin() + at);
    menu.insert(menu.begin() + to, m);
    themeChanged();
    render();
    return true;
  }

 private:
  void render() {
    const auto& menu = g_config.theme.menu;
    std::vector<Row> rows;
    Row h;
    h.header = true;
    h.title = "Center: show or hide \xc2\xb7 Next/Prev: move";
    rows.push_back(h);
    int at = 0;
    for (size_t i = 0; i < menu.size(); i++) {
      const MenuEntry& m = menu[i];
      bool locked = m.id == "settings";
      if (m.id == focus_) at = int(i);
      Row r;
      r.title = menuLabel(m.id);
      r.trailing = locked ? "Always shown" : m.visible ? "Shown" : "Hidden";
      std::string id = m.id;
      r.onSelect = [this, id, locked] {
        focus_ = id;
        if (!locked)
          for (MenuEntry& e : g_config.theme.menu)
            if (e.id == id) e.visible = !e.visible;
        themeChanged();
        render();
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    list_.selectIndex(1 + at);
  }
  std::string focus_;
};

// ThemeScreen: what the dashboard's Theme page changes, on the device. Every change redraws at once.
class ThemeScreen : public ShellListScreen {
 public:
  ThemeScreen() : ShellListScreen("Theme") { render(); }
  void onShow() override { render(); }

 private:
  void render() {
    const Theme& t = g_config.theme;
    swatch_ = swatch(t.accent, false);
    std::vector<Row> rows;
    Row accent;
    accent.title = "Accent color";
    accent.trailing = accentName(t.accent);
    accent.tile = &swatch_;
    accent.chevron = true;
    accent.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new AccentScreen())); };
    rows.push_back(accent);
    Row look;
    look.title = "Appearance";
    look.trailing = t.appearance == "dark" ? "Dark" : "Light";
    look.tile = &tiles::theme;
    look.onSelect = [this] {
      g_config.theme.appearance = g_config.theme.appearance == "dark" ? "light" : "dark";
      themeChanged();
      render();
    };
    rows.push_back(look);
    Row size;
    size.title = "Text size";
    size.trailing = textSizeName(t.textScale);
    size.tile = &tiles::textSize;
    size.chevron = true;
    size.onSelect = [] {
      std::vector<ChoiceScreen::Choice> c;
      for (float s : kTextScales) c.push_back({textSizeName(s), std::to_string(s)});
      g_app->push(choice(
          "Text size", c, [] { return std::to_string(g_config.theme.textScale); },
          [](const std::string& v) {
            g_config.theme.textScale = strtof(v.c_str(), nullptr);
            themeChanged();
          },
          [](const std::string& v) { return fabsf(strtof(v.c_str(), nullptr) - Theme().textScale) < 0.01f ? "standard" : ""; }));
    };
    rows.push_back(size);
    Row menu;
    menu.title = "Main menu";
    size_t shown = 0;
    for (auto& m : t.menu) shown += m.visible;
    menu.trailing = std::to_string(shown) + " of " + std::to_string(t.menu.size()) + " shown";
    menu.tile = &tiles::menuOrder;
    menu.chevron = true;
    menu.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new MenuOrderScreen())); };
    rows.push_back(menu);
    list_.setRows(std::move(rows), true);
  }
  Tile swatch_{Icon::Check, 0, 0};
};

// the address of wlan0, or "" (About's IP address row)
std::string wifiAddress() {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return "";
  ifreq r;
  memset(&r, 0, sizeof(r));
  strncpy(r.ifr_name, "wlan0", IFNAMSIZ - 1);
  std::string out;
  if (ioctl(s, SIOCGIFADDR, &r) == 0) {
    auto* a = reinterpret_cast<sockaddr_in*>(&r.ifr_addr);
    if (a->sin_addr.s_addr) out = inet_ntoa(a->sin_addr);
  }
  close(s);
  return out;
}

}  // namespace

std::string bytes(uint64_t n) {
  char b[32];
  if (n >= (1ull << 30)) snprintf(b, sizeof(b), "%.1f GB", double(n) / double(1ull << 30));
  else if (n >= (1ull << 20)) snprintf(b, sizeof(b), "%.0f MB", double(n) / double(1ull << 20));
  else snprintf(b, sizeof(b), "%llu KB", (unsigned long long)(n / 1024));
  return b;
}

namespace {

bool charging() {
  for (const char* p : {"/sys/class/power_supply/ac/online", "/sys/class/power_supply/usb/online"}) {
    FILE* f = fopen(p, "r");
    if (!f) continue;
    int v = 0;
    if (fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    if (v) return true;
  }
  return false;
}

// the Wi-Fi's address while it's on, else the one in its NVRAM record (bytes 4-9; none: "")
std::string macAddress() {
  char s[32] = "";
  if (FILE* f = fopen("/sys/class/net/wlan0/address", "r")) {
    if (!fgets(s, sizeof(s), f)) s[0] = 0;
    fclose(f);
  }
  std::string mac = s;
  while (!mac.empty() && (mac.back() == '\n' || mac.back() == ' ')) mac.pop_back();
  if (mac.size() == 17) return mac;
  unsigned char b[10] = {};
  FILE* f = fopen("/data/nvram/APCFG/APRDEB/WIFI", "rb");
  bool ok = f && fread(b, 1, sizeof(b), f) == sizeof(b);
  if (f) fclose(f);
  if (!ok || !(b[4] | b[5] | b[6] | b[7] | b[8] | b[9])) return "";
  snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", b[4], b[5], b[6], b[7], b[8], b[9]);
  return s;
}

// "Europe/Lisbon · 17:52": the zone and the time in it
std::string timeZone() {
  char zone[PROP_VALUE_MAX] = "";
  __system_property_get("persist.sys.timezone", zone);
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char clock[8];
  strftime(clock, sizeof(clock), "%H:%M", &t);
  return std::string(zone[0] ? zone : "UTC") + " \xc2\xb7 " + clock;
}

// AboutScreen: what the dashboard's Overview shows about the device; battery and Wi-Fi every 10 s
class AboutScreen : public ShellListScreen {
 public:
  AboutScreen() : ShellListScreen("About") { render(); }
  bool tick(int64_t now) override {
    if (now - last_ < 10 * kSec) return false;
    render();
    return true;
  }

 private:
  void render() {
    last_ = now_ns();
    std::vector<Row> rows;
    auto add = [&](const char* title, std::string trailing, const Tile* tile = nullptr) {
      Row r;
      r.title = title;
      r.trailing = std::move(trailing);
      r.tile = tile;
      rows.push_back(std::move(r));
    };
    add("Rondo", kVersion, &tiles::about);
    add("Model", "Innioasis Y1");
    const Status& st = g_app->status();
    add("Battery", std::to_string(int(floorf(st.battery * 100 + 0.5f))) + "%" + (charging() ? " \xc2\xb7 charging" : ""));
    for (auto [path, name] : {std::pair<const char*, const char*>{"/storage/sdcard0", "Card"}, {"/storage/sdcard1", "Internal storage"}}) {
      struct statfs s;
      if (statfs(path, &s) != 0) continue;
      uint64_t total = uint64_t(s.f_blocks) * s.f_bsize, free = uint64_t(s.f_bavail) * s.f_bsize;
      if (total > 0) add(name, bytes(free) + " free of " + bytes(total));
    }
    add("Wi-Fi", wifiName());
    std::string ip = wifiAddress();
    if (!ip.empty()) add("IP address", ip);
    // 2.0: the Wi-Fi's fixed address and the time zone the server named (wifi.cpp, clock.h)
    std::string mac = macAddress();
    if (!mac.empty()) add("Wi-Fi MAC address", mac);
    add("Time zone", timeZone());
    std::string server = g_config.server.url;
    for (const char* p : {"https://", "http://"})
      if (server.compare(0, strlen(p), p) == 0) server = server.substr(strlen(p));
    add("Server", directMode() ? "Directly to YouTube" : server.empty() ? "Not set up" : server);
    list_.setRows(std::move(rows), true);
  }
  int64_t last_ = 0;
};

// SettingsScreen
// What Settings' rows say, found by the top list and shown in its groups too: the server's state (a
// request) and a newer Rondo (another)
std::string g_serverState = "Checking\xe2\x80\xa6", g_latestVersion;
// direct mode's YouTube account (cookies.txt from the dashboard): "" until asked, "in", "out", "expired"
std::string g_directAccount, g_directName;
// the code sign-in's state (direct/tv_auth.h): "" until asked, "in", "out"; its name
std::string g_tvState, g_tvName;
std::string field(const std::string& body, const char* name) {  // a string member of a small answer
  std::string k = std::string("\"") + name + "\":\"";
  size_t at = body.find(k);
  return at == std::string::npos ? "" : body.substr(at + k.size(), body.find('"', at + k.size()) - at - k.size());
}
void takeTv(const std::string& body) {
  g_tvState = body.find("\"signedIn\":true") != std::string::npos ? "in" : "out";
  g_tvName = field(body, "name");
}
// a sign-in or out: what was cached for the other account goes, the tabs follow
void accountSwitched() {
  unlink((g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "api-cache/server").c_str());
  if (g_onServerChanged) g_onServerChanged();
}

// Settings › YouTube › YouTube account › Sign in with a code: the code to enter at google.com/device,
// as a smart TV shows it; it closes by itself once Google says yes. Menu cancels.
class TvCodeScreen : public Screen {
 public:
  TvCodeScreen() {
    auto alive = alive_;
    g_core.get("/tv/begin", 0, [this, alive](int status, const std::string& body) {
      if (!*alive) return;
      take(status, body);
    });
  }
  ~TvCodeScreen() override {
    *alive_ = false;
    if (!code_.empty() && !done_) g_core.get("/tv/cancel", 0, [](int, const std::string&) {});
  }
  std::string title() const override { return "Sign in"; }
  bool keepScreenOn() const override { return true; }  // while the code is typed elsewhere
  bool onAction(Action, int) override { return false; }
  bool tick(int64_t now) override {
    if (done_ || code_.empty() || now < nextAsk_) return secondChanged(now);
    nextAsk_ = now + 3000 * kMs;
    auto alive = alive_;
    g_core.get("/tv", 0, [this, alive](int status, const std::string& body) {
      if (!*alive) return;
      take(status, body);
    });
    return secondChanged(now);
  }
  int64_t wakeAt() const override { return done_ ? 0 : now_ns() + 1000 * kMs; }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.shellBg);
    e.c.layer(1);
    float y = float(e.dp(18)), pad = float(e.dp(16));
    auto line = [&](const std::string& t, int size, bool bold, ui::Color c, float gap) {
      Label l = label(e, t, size, bold, w - 2 * pad);
      drawLabel(e, l, (w - l.w) / 2, y, c);
      y += l.h + gap;
    };
    if (!problem_.empty()) {
      line("Couldn't get a code", 16, true, e.p.text, float(e.dp(8)));
      line(problem_, 12, false, e.p.muted, 0);
      return;
    }
    if (code_.empty()) {
      line("Asking YouTube for a code\xe2\x80\xa6", 14, false, e.p.muted, 0);
      return;
    }
    line("On your phone or computer, open", 13, false, e.p.muted, float(e.dp(2)));
    line(url_, 17, true, e.p.text, float(e.dp(14)));
    line("and enter", 13, false, e.p.muted, float(e.dp(8)));
    line(code_, 34, true, e.p.accent, float(e.dp(14)));
    int64_t left = std::max<int64_t>(0, expiresAt_ - int64_t(time(nullptr)) * 1000) / 1000;
    char b[64];
    snprintf(b, sizeof(b), "Waiting for Google \xc2\xb7 expires in %d:%02d", int(left / 60), int(left % 60));
    line(b, 11, false, e.p.muted, float(e.dp(4)));
    line("Pick the Google account for your Rondo and allow access", 11, false, e.p.muted, 0);
  }

 private:
  void take(int status, const std::string& body) {
    if (status != 200) {
      problem_ = "Is the Rondo online? Try again in a moment.";
      return;
    }
    takeTv(body);
    if (g_tvState == "in") {
      done_ = true;
      accountSwitched();
      g_app->toast(g_tvName.empty() ? "Signed in to YouTube" : "Signed in as " + g_tvName);
      g_app->pop();
      return;
    }
    std::string code = field(body, "code");
    if (code.empty() && !code_.empty()) {  // no code any more: refused or expired
      std::string err = field(body, "error");
      problem_ = err == "denied" ? "The sign-in was refused at Google." : "The code expired before it was entered.";
      code_.clear();
      return;
    }
    if (code.empty()) {
      problem_ = "YouTube didn't give a code. Check Wi-Fi and try again.";
      return;
    }
    code_ = code, url_ = field(body, "url");
    if (url_.compare(0, 8, "https://") == 0) url_ = url_.substr(8);
    size_t at = body.find("\"expiresAt\":");
    expiresAt_ = at == std::string::npos ? 0 : atoll(body.c_str() + at + 12);
    nextAsk_ = now_ns() + 3000 * kMs;
  }
  bool secondChanged(int64_t now) {
    int64_t sec = now / (1000 * kMs);
    if (sec == lastSec_) return false;
    lastSec_ = sec;
    return true;
  }
  std::string code_, url_, problem_;
  int64_t expiresAt_ = 0, nextAsk_ = 0, lastSec_ = 0;
  bool done_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

void askDirectAccount() {
  if (!directMode()) return;
  g_core.get("/tv", 0, [](int status, const std::string& body) {
    if (status == 200) takeTv(body);
  });
  g_core.get("/account", 0, [](int status, const std::string& body) {
    if (status != 200) return;
    bool in = body.find("\"signedIn\":true") != std::string::npos;
    g_directAccount = in ? "in" : body.find("\"expired\":true") != std::string::npos ? "expired" : "out";
    size_t at = body.find("\"name\":\"");
    g_directName = at == std::string::npos ? "" : body.substr(at + 8, body.find('"', at + 8) - at - 8);
  });
}

using Rows = std::vector<Row>;
Row settingRow(const char* title, std::string trailing, const Tile& tile, bool chevron, std::function<void()> on) {
  Row r;
  r.title = title, r.trailing = std::move(trailing), r.tile = &tile, r.chevron = chevron;
  r.onSelect = std::move(on);
  return r;
}

// One group of Settings: its rows made again when it shows and whenever what they say changes (Wi-Fi
// joined, the clock, a toggle)
class SettingsGroupScreen : public ShellListScreen {
 public:
  using Make = std::function<Rows(SettingsGroupScreen&)>;
  SettingsGroupScreen(std::string title, Make make, std::function<void()> shown)
      : ShellListScreen(std::move(title)), make_(std::move(make)), shown_(std::move(shown)) {
    render();
  }
  void onShow() override {
    if (shown_) shown_();
    render();
  }
  bool tick(int64_t) override {
    Rows rows = make_(*this);
    if (same(rows, list_.rows())) return false;
    list_.setRows(std::move(rows), true);
    return true;
  }
  void render() { list_.setRows(make_(*this), true); }

 private:
  static bool same(const Rows& a, const Rows& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); k++)
      if (a[k].title != b[k].title || a[k].trailing != b[k].trailing || a[k].subtitle != b[k].subtitle) return false;
    return true;
  }
  Make make_;
  std::function<void()> shown_;  // each time it shows (YouTube: the server asked again)
};
void pushGroup(const char* title, SettingsGroupScreen::Make make, std::function<void()> shown = nullptr) {
  g_app->push(std::unique_ptr<Screen>(new SettingsGroupScreen(title, std::move(make), std::move(shown))));
}

// Connections: Wi-Fi and Bluetooth
Rows connectionsRows(SettingsGroupScreen&) {
  return {settingRow("Wi-Fi", wifiName(), tiles::wifi, true, [] { g_app->push(makeWifi()); }),
          settingRow("Bluetooth", bluetoothState(), tiles::bluetooth, true, [] { g_app->push(makeBluetooth()); })};
}

// YouTube: where it comes from and how videos play
Rows youtubeRows(SettingsGroupScreen&) {
  Rows rows;
  rows.push_back(settingRow("Connection", g_serverState, tiles::server, true, [] { g_app->push(makeServer()); }));
  // the account: the server's own (its admin page); in direct mode the code sign-in, here or on the
  // dashboard, and cookies.txt from the dashboard
  if (directMode()) {
    bool tvIn = g_tvState == "in";
    std::string state = tvIn ? (g_tvName.empty() ? "Signed in" : g_tvName)
                        : g_directAccount == "in" ? (g_directName.empty() ? "Signed in" : g_directName)
                        : g_tvState.empty() ? "" : "Signed out";
    Row r = settingRow("YouTube account", state, tiles::account, true, [tvIn] {
      if (!tvIn) return g_app->push(std::unique_ptr<Screen>(new TvCodeScreen()));
      g_app->push(makeChoice(
          "YouTube account", {{"Stay signed in", "stay"}, {"Sign out", "out"}}, [] { return std::string("stay"); },
          [](const std::string& v) {
            if (v != "out") return;
            g_core.get("/tv/signout", 0, [](int status, const std::string& body) {
              if (status == 200) takeTv(body);
              accountSwitched();
              g_app->toast("Signed out of YouTube");
            });
          },
          [](const std::string& v) { return std::string(v == "out" ? "the code sign-in, here and at Google" : ""); }));
    });
    r.subtitle = tvIn ? "Home, Subscriptions, History, playlists"
                 : g_directAccount == "in" ? "With cookies.txt from the dashboard"
                                           : "Sign in with a code, like a smart TV";
    rows.push_back(std::move(r));
  }
  rows.push_back(settingRow("Video quality", g_prefs.videoQuality ? std::to_string(g_prefs.videoQuality) + "p" : "Auto", tiles::quality,
                            true, [] {
                              g_app->push(choice(
                                  "Video quality", {{"Auto", "0"}, {"720p", "720"}, {"480p", "480"}, {"360p", "360"}},
                                  [] { return std::to_string(g_prefs.videoQuality); },
                                  [](const std::string& v) {
                                    g_prefs.videoQuality = atoi(v.c_str());
                                    savePrefsNow();
                                  },
                                  [](const std::string& v) { return v == "0" ? "up to 360p" : ""; }));
                            }));
  return rows;
}

// Settings › Display › Subtitles: shown or not, the language looked for first, and their look, with
// a line of them over a picture at the top as they will look (the dashboard's General page has the same)
const char* const kSubLangs[] = {"", "en", "az", "tr", "ru", "de", "fr", "es", "it", "pt", "ar", "fa", "uk", "ja", "ko", "zh", "hi"};

class SubtitlesScreen : public ShellListScreen {
 public:
  SubtitlesScreen() : ShellListScreen("Subtitles") { render(); }
  void onShow() override { render(); }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.shellBg);
    // the preview: a dusk-colored "picture" with a subtitle line on it
    float ph = float(e.dp(62)), m = float(e.dp(8));
    Rect pic{m, float(e.dp(4)), w - 2 * m, ph};
    e.c.shape(pic, e.dpf(6), rgb(0x3b4b6b), rgb(0x1a1f2b));
    e.c.pushClip(pic);
    e.c.pushOffset(pic.x, 0);
    drawSubtitleSample(e, "Subtitles look like this", pic.w, pic.y + pic.h - e.dpf(8));
    e.c.popOffset();
    e.c.popClip();
    float top = pic.y + ph + float(e.dp(4));
    e.c.layer(0);
    list_.layout(e, w, h - top);
    list_.draw(e, 0, top);
  }

 private:
  void render() {
    Rows rows;
    auto row = [&](const char* title, std::string value, std::function<void()> on, bool chevron = true) {
      Row r;
      r.title = title, r.trailing = std::move(value), r.chevron = chevron, r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    auto pick = [](const char* title, std::vector<std::pair<std::string, std::string>> c, int* field) {
      g_app->push(makeChoice(
          title, std::move(c), [field] { return std::to_string(*field); },
          [field](const std::string& v) {
            *field = atoi(v.c_str());
            savePrefsNow();
          },
          [field](const std::string& v) {
            Prefs d;  // YouTube's own: the default
            int* dflt = field == &g_prefs.subSize ? &d.subSize : field == &g_prefs.subColor ? &d.subColor
                        : field == &g_prefs.subOpacity ? &d.subOpacity : field == &g_prefs.subBack ? &d.subBack
                        : field == &g_prefs.subBackOpacity ? &d.subBackOpacity : &d.subEdge;
            return std::string(atoi(v.c_str()) == *dflt ? "YouTube's" : "");
          }));
    };
    auto colors = [] {
      std::vector<std::pair<std::string, std::string>> c;
      for (int i = 0; i < 8; i++) c.push_back({kSubColors[i].name, std::to_string(i)});
      return c;
    };
    auto percents = [](std::initializer_list<int> list) {
      std::vector<std::pair<std::string, std::string>> c;
      for (int v : list) c.push_back({v ? std::to_string(v) + "%" : "None", std::to_string(v)});
      return c;
    };
    row("Subtitles", g_prefs.subtitles ? "On" : "Off", [this] {
      g_prefs.subtitles = !g_prefs.subtitles;
      savePrefsNow();
      render();
    }, false);
    row("Language", g_prefs.subLang.empty() ? "The video's own" : languageName(g_prefs.subLang), [] {
      std::vector<std::pair<std::string, std::string>> c;
      for (const char* l : kSubLangs) c.push_back({*l ? languageName(l) : "The video's own", l});
      g_app->push(makeChoice(
          "Language", std::move(c), [] { return g_prefs.subLang; },
          [](const std::string& v) {
            g_prefs.subLang = v;
            savePrefsNow();
          },
          [](const std::string& v) { return std::string(v.empty() ? "its first track" : ""); }));
    });
    row("Size", kSubSizes[g_prefs.subSize], [pick] {
      std::vector<std::pair<std::string, std::string>> c;
      for (int i = 0; i < 4; i++) c.push_back({kSubSizes[i], std::to_string(i)});
      pick("Size", c, &g_prefs.subSize);
    });
    row("Text color", kSubColors[g_prefs.subColor].name, [pick, colors] { pick("Text color", colors(), &g_prefs.subColor); });
    row("Text opacity", std::to_string(g_prefs.subOpacity) + "%", [pick, percents] { pick("Text opacity", percents({25, 50, 75, 100}), &g_prefs.subOpacity); });
    row("Background", kSubColors[g_prefs.subBack].name, [pick, colors] { pick("Background", colors(), &g_prefs.subBack); });
    row("Background opacity", g_prefs.subBackOpacity ? std::to_string(g_prefs.subBackOpacity) + "%" : "None",
        [pick, percents] { pick("Background opacity", percents({0, 25, 50, 75, 100}), &g_prefs.subBackOpacity); });
    row("Edges", kSubEdges[g_prefs.subEdge], [pick] {
      std::vector<std::pair<std::string, std::string>> c;
      for (int i = 0; i < 5; i++) c.push_back({kSubEdges[i], std::to_string(i)});
      pick("Edges", c, &g_prefs.subEdge);
    });
    row("Use YouTube's look", "", [this] {
      resetSubtitleStyle();
      savePrefsNow();
      render();
      g_app->toast("Subtitles look as on YouTube");
    }, false);
    list_.setRows(std::move(rows), true);
  }
};

// Display: the look, the screen, what Now Playing shows
Rows displayRows(SettingsGroupScreen& screen) {
  Rows rows;
  rows.push_back(settingRow("Theme", accentName(g_config.theme.accent), tiles::theme, true,
                            [] { g_app->push(std::unique_ptr<Screen>(new ThemeScreen())); }));
  rows.push_back(settingRow("Brightness", std::to_string(brightnessPercent()) + "%", tiles::brightness, true, [] {
    std::vector<ChoiceScreen::Choice> c;
    for (int v : {10, 25, 40, 60, 80, 100}) c.push_back({std::to_string(v) + "%", std::to_string(v)});
    g_app->push(choice(
        "Brightness", c,
        [] {  // the nearest choice to the backlight's value
          int best = 10;
          for (int v : {10, 25, 40, 60, 80, 100})
            if (abs(v * 255 / 100 - g_prefs.brightness) < abs(best * 255 / 100 - g_prefs.brightness)) best = v;
          return std::to_string(best);
        },
        [](const std::string& v) {
          g_prefs.brightness = std::max(10, std::min(255, atoi(v.c_str()) * 255 / 100));
          applyBrightness(g_prefs.brightness);
          savePrefsNow();
        },
        [](const std::string& v) { return atoi(v.c_str()) <= 40 ? "saves battery" : ""; }));
  }));
  rows.push_back(settingRow("Screen off after", timeoutName(g_prefs.screenTimeoutMs), tiles::timer, true, [] {
    std::vector<ChoiceScreen::Choice> c;
    for (int v : {15000, 30000, 60000, 120000, 300000, 600000}) c.push_back({timeoutName(v), std::to_string(v)});
    g_app->push(choice(
        "Screen off after", c,
        [] {
          int best = 15000;
          for (int v : {15000, 30000, 60000, 120000, 300000, 600000})
            if (abs(v - g_prefs.screenTimeoutMs) < abs(best - g_prefs.screenTimeoutMs)) best = v;
          return std::to_string(best);
        },
        [](const std::string& v) {  // power_screens.cpp turns the screen off after it
          g_prefs.screenTimeoutMs = atoi(v.c_str());
          savePrefsNow();
        },
        [](const std::string& v) { return atoi(v.c_str()) <= 30000 ? "saves battery" : ""; }));
  }));
  rows.push_back(settingRow("Subtitles", g_prefs.subtitles ? "On" : "Off", tiles::subtitles, true,
                            [] { g_app->push(std::unique_ptr<Screen>(new SubtitlesScreen())); }));
  SettingsGroupScreen* self = &screen;
  rows.push_back(settingRow("Open lyrics automatically", onOff(g_config.behavior.lyricsOnOpen), tiles::lyrics, false, [self] {
    g_config.behavior.lyricsOnOpen = !g_config.behavior.lyricsOnOpen;
    saveConfigNow();
    self->render();
  }));
  return rows;
}

// Controls: the wheel and the buttons
Rows controlsRows(SettingsGroupScreen& screen) {
  Rows rows;
  rows.push_back(settingRow("Wheel acceleration", accelerationName(g_config.behavior.wheelAcceleration), tiles::accel, true, [] {
    g_app->push(choice(
        "Wheel acceleration", {{"Off", "off"}, {"Normal", "normal"}, {"Fast", "fast"}},
        [] { return g_config.behavior.wheelAcceleration; },
        [](const std::string& v) {
          g_config.behavior.wheelAcceleration = v;
          saveConfigNow();
          if (g_app->onAccelerationChanged) g_app->onAccelerationChanged();
        },
        [](const std::string& v) { return v == "off" ? "one step a pulse" : v == "fast" ? "after a short spin" : "after a long spin"; }));
  }));
  rows.push_back(settingRow("Wheel vibration", vibrationName(g_config.behavior.wheelVibration), tiles::wheel, true, [] {
    g_app->push(choice(
        "Wheel vibration", {{"Off", "off"}, {"Light", "light"}, {"Normal", "normal"}, {"Strong", "strong"}},
        [] { return g_config.behavior.wheelVibration; },
        [](const std::string& v) {
          g_config.behavior.wheelVibration = v;
          saveConfigNow();
        },
        [](const std::string& v) { return v == "normal" ? "like Rockbox" : ""; }));
  }));
  SettingsGroupScreen* self = &screen;
  rows.push_back(settingRow("Click sound", onOff(g_prefs.clickSound), tiles::sound, false, [self] {
    g_prefs.clickSound = !g_prefs.clickSound;
    savePrefsNow();
    self->render();
  }));
  return rows;
}

// System: the clock, updates, this Rondo, turning it off
Rows systemRows(SettingsGroupScreen&) {
  return {settingRow("Date & time", clockTime(time(nullptr), g_prefs.clock24), tiles::clock, true, [] { g_app->push(makeDateTime()); }),
          settingRow("Software update", g_latestVersion.empty() ? "" : g_latestVersion + " is out", tiles::update, true,
                     [] { g_app->push(makeSoftwareUpdate()); }),
          settingRow("About", std::string("Rondo ") + kVersion, tiles::about, true,
                     [] { g_app->push(std::unique_ptr<Screen>(new AboutScreen())); }),
          settingRow("Shut down", "", tiles::power, true, [] { g_app->push(makePower()); })};
}

// Settings: the dashboard, then the groups, each saying what matters most in it
class SettingsScreen : public ShellListScreen {
 public:
  SettingsScreen() : ShellListScreen("Settings") { render(); }
  ~SettingsScreen() override { *alive_ = false; }
  void onShow() override {
    render();
    checkServer();
    // a newer Rondo, when GitHub has one (the update page's latest.json, through the Y1's own direct server)
    {
      auto alive = alive_;
      g_core.get(std::string(Api::kLocal) + "update?firmware=" + std::to_string(firmwareLevel()), 0, [this, alive](int status, const std::string& body) {
        if (!*alive || status != 200) return;
        std::string v = updateVersion(body);
        g_latestVersion = isNewerVersion(v, kVersion) ? v : "";
        render();
      });
    }
  }
  bool tick(int64_t) override {
    std::string w = wifiName();
    if (w == wifi_) return false;
    render();
    return true;
  }

 private:
  void render() {
    wifi_ = wifiName();
    Rows rows;
    rows.push_back(settingRow("Dashboard", "", tiles::dashboard, true, [] { g_app->push(makeDashboard()); }));
    rows.push_back(settingRow("Connections", wifi_, tiles::connections, true, [] { pushGroup("Connections", connectionsRows); }));
    rows.push_back(settingRow("YouTube", g_serverState, tiles::video, true, [] {
      // the connection asked again each time the group shows: back from switching it, it says the new one
      pushGroup("YouTube", youtubeRows, [] {
        serverState([](const std::string& s) { g_serverState = s; });
        askDirectAccount();
      });
    }));
    rows.push_back(settingRow("Sound", soundState(), tiles::equalizer, true, [] { g_app->push(makeSound()); }));
    rows.push_back(settingRow("Display", accentName(g_config.theme.accent), tiles::display, true, [] { pushGroup("Display", displayRows); }));
    rows.push_back(settingRow("Controls", "", tiles::controls, true, [] { pushGroup("Controls", controlsRows); }));
    rows.push_back(settingRow("System", g_latestVersion.empty() ? std::string("Rondo ") + kVersion : g_latestVersion + " is out", tiles::system,
                              true, [] { pushGroup("System", systemRows); }));
    list_.setRows(std::move(rows), true);
  }
  // on opening Settings: the YouTube row says Checking… until the server answers
  void checkServer() {
    g_serverState = "Checking\xe2\x80\xa6";
    render();
    auto alive = alive_;
    serverState([this, alive](const std::string& s) {
      g_serverState = s;
      if (*alive) render();
    });
  }
  std::string wifi_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

// Settings › About's Wi-Fi address and time zone, for the dashboard
std::string aboutMac() { return macAddress(); }
std::string aboutTimeZone() { return timeZone(); }

std::unique_ptr<Screen> makeSettings() { return std::unique_ptr<Screen>(new SettingsScreen()); }

std::unique_ptr<Screen> makeChoice(std::string title, std::vector<std::pair<std::string, std::string>> choices,
                                   std::function<std::string()> current, std::function<void(const std::string&)> pick,
                                   std::function<std::string(const std::string&)> note) {
  std::vector<ChoiceScreen::Choice> c;
  for (auto& p : choices) c.push_back({p.first, p.second});
  return choice(std::move(title), std::move(c), std::move(current), std::move(pick), std::move(note));
}

}  // namespace rondo::shell
