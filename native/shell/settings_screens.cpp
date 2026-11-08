// Settings (SettingsScreen.kt and ThemeScreens.kt in 1.0.x), ported 1:1: the list, the choice
// screens, Theme with its accent, text size and main menu order, and About; each system service
// (Wi-Fi, Bluetooth, Dashboard, Server, Software update, Sound, Shut down) leads to its screen.
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
#include <unistd.h>

#include <deque>

#include "app.h"
#include "screens.h"

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
    size.tile = &tiles::lyrics;
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
    menu.tile = &tiles::accel;
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
    std::string server = g_config.server.url;
    for (const char* p : {"https://", "http://"})
      if (server.compare(0, strlen(p), p) == 0) server = server.substr(strlen(p));
    add("Server", server.empty() ? "Not set up" : server);
    list_.setRows(std::move(rows), true);
  }
  int64_t last_ = 0;
};

// SettingsScreen
class SettingsScreen : public ShellListScreen {
 public:
  SettingsScreen() : ShellListScreen("Settings") { render(); }
  ~SettingsScreen() override { *alive_ = false; }
  void onShow() override {
    render();
    checkServer();
    // a newer Rondo, when the server knows one
    if (!g_config.server.url.empty()) {
      auto alive = alive_;
      g_core.get("/update?firmware=" + std::to_string(firmwareLevel()), 0, [this, alive](int status, const std::string& body) {
        if (!*alive || status != 200) return;
        std::string v = updateVersion(body);
        latest_ = isNewerVersion(v, kVersion) ? v : "";
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
    std::vector<Row> rows;
    auto add = [&](const char* title, std::string trailing, const Tile& tile, bool chevron, std::function<void()> on) {
      Row r;
      r.title = title, r.trailing = std::move(trailing), r.tile = &tile, r.chevron = chevron;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    add("Dashboard", "", tiles::dashboard, true, [] { g_app->push(makeDashboard()); });
    add("Wi-Fi", wifiName(), tiles::wifi, true, [] { g_app->push(makeWifi()); });
    add("Bluetooth", bluetoothState(), tiles::bluetooth, true, [] { g_app->push(makeBluetooth()); });
    add("Sound", soundState(), tiles::equalizer, true, [] { g_app->push(makeSound()); });
    wifi_ = wifiName();
    add("Theme", accentName(g_config.theme.accent), tiles::theme, true,
        [] { g_app->push(std::unique_ptr<Screen>(new ThemeScreen())); });
    add("Video quality", g_prefs.videoQuality ? std::to_string(g_prefs.videoQuality) + "p" : "Auto", tiles::quality, true, [] {
      g_app->push(choice(
          "Video quality", {{"Auto", "0"}, {"720p", "720"}, {"480p", "480"}, {"360p", "360"}},
          [] { return std::to_string(g_prefs.videoQuality); },
          [](const std::string& v) {
            g_prefs.videoQuality = atoi(v.c_str());
            savePrefsNow();
          },
          [](const std::string& v) { return v == "0" ? "up to 360p" : ""; }));
    });
    add("Brightness", std::to_string(brightnessPercent()) + "%", tiles::brightness, true, [] {
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
    });
    add("Screen off after", timeoutName(g_prefs.screenTimeoutMs), tiles::timer, true, [] {
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
    });
    add("Open lyrics automatically", onOff(g_config.behavior.lyricsOnOpen), tiles::lyrics, false, [this] {
      g_config.behavior.lyricsOnOpen = !g_config.behavior.lyricsOnOpen;
      saveConfigNow();
      render();
    });
    add("Wheel acceleration", accelerationName(g_config.behavior.wheelAcceleration), tiles::accel, true, [] {
      g_app->push(choice(
          "Wheel acceleration", {{"Off", "off"}, {"Normal", "normal"}, {"Fast", "fast"}},
          [] { return g_config.behavior.wheelAcceleration; },
          [](const std::string& v) {
            g_config.behavior.wheelAcceleration = v;
            saveConfigNow();
            if (g_app->onAccelerationChanged) g_app->onAccelerationChanged();
          },
          [](const std::string& v) {
            return v == "off" ? "one step a pulse" : v == "fast" ? "after a short spin" : "after a long spin";
          }));
    });
    add("Wheel vibration", vibrationName(g_config.behavior.wheelVibration), tiles::wheel, true, [] {
      g_app->push(choice(
          "Wheel vibration", {{"Off", "off"}, {"Light", "light"}, {"Normal", "normal"}, {"Strong", "strong"}},
          [] { return g_config.behavior.wheelVibration; },
          [](const std::string& v) {
            g_config.behavior.wheelVibration = v;
            saveConfigNow();
          },
          [](const std::string& v) { return v == "normal" ? "like Rockbox" : ""; }));
    });
    add("Click sound", onOff(g_prefs.clickSound), tiles::sound, false, [this] {
      g_prefs.clickSound = !g_prefs.clickSound;
      savePrefsNow();
      render();
    });
    add("Server", server_, tiles::server, true, [] { g_app->push(makeServer()); });
    add("Software update", latest_.empty() ? "" : latest_ + " is out", tiles::update, true, [] { g_app->push(makeSoftwareUpdate()); });
    add("About", std::string("Rondo ") + kVersion, tiles::about, true,
        [] { g_app->push(std::unique_ptr<Screen>(new AboutScreen())); });
    add("Shut down", "", tiles::power, true, [] { g_app->push(makePower()); });
    list_.setRows(std::move(rows), true);
  }
  // on opening Settings: the row says Checking… until the server answers
  void checkServer() {
    server_ = "Checking\xe2\x80\xa6";
    render();
    auto alive = alive_;
    serverState([this, alive](const std::string& s) {
      if (!*alive) return;
      server_ = s;
      render();
    });
  }
  std::string wifi_, server_ = "Checking\xe2\x80\xa6", latest_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

std::unique_ptr<Screen> makeSettings() { return std::unique_ptr<Screen>(new SettingsScreen()); }

std::unique_ptr<Screen> makeChoice(std::string title, std::vector<std::pair<std::string, std::string>> choices,
                                   std::function<std::string()> current, std::function<void(const std::string&)> pick,
                                   std::function<std::string(const std::string&)> note) {
  std::vector<ChoiceScreen::Choice> c;
  for (auto& p : choices) c.push_back({p.first, p.second});
  return choice(std::move(title), std::move(c), std::move(current), std::move(pick), std::move(note));
}

}  // namespace rondo::shell
