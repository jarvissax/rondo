// Settings › Server (ServerScreen.kt in 1.0.x), ported 1:1: which Rondo server this Y1 uses, a
// connection test, and connecting to one with its address and the code from its admin page
// (docs/dev/server-api.md), or with a token for servers started with API_TOKEN. Also Settings' Server
// row (SettingsScreen.serverState) and the account check the main menu makes (Accounts.kt).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#include <algorithm>
#include <memory>

#include <sys/system_properties.h>

#include "../core/api.h"
#include "../core/version.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {

std::function<void()> g_onServerChanged;

namespace {

std::string hostOf(std::string url) {
  for (const char* p : {"https://", "http://"})
    if (url.compare(0, strlen(p), p) == 0) return url.substr(strlen(p));
  return url;
}

// Api.health(): the accounts the server has feed YouTube Music's Library and YouTube's Subscriptions and
// History; remembered so the tabs don't jump before the next check (servers older than this send
// nothing and keep every tab)
// what the last GET /healthz said of the server's version (serverUpdateNeeded)
std::string g_serverVersion;
bool g_serverChecked = false;

void takeAccounts(const Health& h) {
  g_serverVersion = h.version, g_serverChecked = true;
  if (h.music == g_prefs.accountMusic && h.videos == g_prefs.accountVideos) return;
  g_prefs.accountMusic = h.music, g_prefs.accountVideos = h.videos;
  savePrefsNow();
}

void useServer(const std::string& url, const std::string& token) {
  g_config.server.url = url, g_config.server.token = token, g_config.server.direct = false;
  saveConfigNow();
  if (g_onServerChanged) g_onServerChanged();
}

void useDirect(bool on) {
  if (g_config.server.direct == on) return;
  g_config.server.direct = on;
  saveConfigNow();
  if (g_onServerChanged) g_onServerChanged();
}

Row entry(const std::string& title, const std::string& subtitle = "", const std::string& trailing = "") {
  Row r;
  r.title = title, r.subtitle = subtitle, r.trailing = trailing;
  return r;
}

class ServerScreen : public ShellListScreen {
 public:
  ServerScreen() : ShellListScreen("Server") { render(); }
  ~ServerScreen() override { *alive_ = false; }
  void onShow() override { render(); }

 private:
  void render() {
    std::vector<Row> rows;
    // Connect to: YouTube straight from the Y1, or the user's Rondo server (2.0, Nov 9)
    rows.push_back(header("Connect to"));
    bool direct = directMode();
    Row yt = entry("Directly to YouTube", "No server needed", direct ? "Current" : "");
    yt.onSelect = [this] {
      useDirect(true);
      test_.clear();
      render();
    };
    rows.push_back(yt);
    Row mine = entry("My server", g_config.server.url.empty() ? "Not set up yet" : hostOf(g_config.server.url), direct ? "" : "Current");
    mine.onSelect = [this] {
      if (g_config.server.url.empty()) return askAddress();
      useDirect(false);
      test_.clear();
      render();
    };
    rows.push_back(mine);
    rows.push_back(header("Status"));
    Row address = entry("Address", "", direct ? "YouTube" : hostOf(g_config.server.url));
    address.tile = &tiles::server;
    rows.push_back(address);
    Row test = entry("Connection test", direct ? "Asks YouTube and reports back" : "Calls the server and reports back");
    test.onSelect = [this] { runTest(); };
    rows.push_back(test);
    if (!test_.empty()) {
      rows.push_back(header("Test"));
      for (const Row& r : test_) rows.push_back(r);
    }
    rows.push_back(header("Connect"));
    Row connect = entry("Connect to a server", "Its address and the code from its admin page");
    connect.chevron = true;
    connect.onSelect = [this] { askAddress(); };
    rows.push_back(connect);
    Row token = entry("Use a token instead", "For servers started with API_TOKEN");
    token.chevron = true;
    token.onSelect = [this] { askToken(); };
    rows.push_back(token);
    list_.setRows(std::move(rows), true);
  }
  static Row header(const std::string& title) {
    Row h;
    h.header = true;
    h.title = title;
    return h;
  }
  void show(std::vector<Row> rows) {
    test_ = std::move(rows);
    render();
  }

  void runTest() {
    Row calling = entry(directMode() ? "Asking YouTube\xe2\x80\xa6" : "Calling the server\xe2\x80\xa6");
    calling.enabled = false;
    show({calling});
    auto alive = alive_;
    int64_t started = now_ns();  // the real clock: g_app->now() is the frame's
    if (directMode()) {
      // a search answered and a song's streams found: what playing needs
      g_core.get("/music/search?type=song&q=" + Api::escape("Rondo alla turca"), 0, [this, alive, started](int status, const std::string& body) {
        if (!*alive) return;
        int ms = int((now_ns() - started) / 1000000);
        if (status != 200) return show({entry(status == 0 ? "YouTube didn't answer" : "YouTube answered HTTP " + std::to_string(status), "Check the Wi-Fi")});
        std::string id;
        if (yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0)) {
          yyjson_val* first = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "items"), 0);
          if (yyjson_is_str(yyjson_obj_get(first, "id"))) id = yyjson_get_str(yyjson_obj_get(first, "id"));
          yyjson_doc_free(doc);
        }
        g_core.get("/resolve/" + id, 0, [this, alive, ms](int status, const std::string&) {
          if (!*alive) return;
          show({entry("Answered in", "", std::to_string(ms) + " ms"), entry("Playback", "", status == 200 ? "working" : "failing"),
                entry("YouTube account", "", "signed out")});
        });
      });
      return;
    }
    if (g_config.server.url.empty()) return show({entry("No server yet", "Connect to one below")});
    g_core.get("/healthz", 0, [this, alive, started](int status, const std::string& body) {
      if (!*alive) return;
      int ms = int((now_ns() - started) / 1000000);
      Health h;
      if (status == 200 && !parseHealth(body, h)) status = -1;
      if (status != 200) return failed(status);
      takeAccounts(h);
      g_core.get("/time", 0, [this, alive, ms, h](int status, const std::string&) {  // needs the token
        if (!*alive) return;
        if (status != 200) return failed(status);
        std::vector<Row> rows = {entry("Answered in", "", std::to_string(ms) + " ms")};
        if (serverTooOld(h.version))
          rows.push_back(entry("Server needs an update", std::string("This Rondo needs ") + kServerVersionNeeded + " or newer: update it on its admin page"));
        rows.push_back(entry("Server", "", h.version.empty() ? "older than 3.1.0" : h.version));
        rows.push_back(entry("yt-dlp", "", h.ytdlp.empty() ? "missing" : h.ytdlp));
        rows.push_back(entry("Playback", "", h.lastResolveOk == 1 ? "working" : h.lastResolveOk == 0 ? "failing" : "not tried yet"));
        rows.push_back(entry("YouTube account", "", h.music || h.videos ? "signed in" : "signed out"));
        show(rows);
      });
    });
  }
  // HttpException's codes, or no answer at all (0) or an answer that isn't the server's (-1)
  void failed(int status) {
    if (status == 401 || status == 403) show({entry("The server refused this Rondo", "Connect again with a new code")});
    else if (status > 0) show({entry("The server answered HTTP " + std::to_string(status), "Connect again with a new code")});
    else show({entry("The server didn't answer", "Check the Wi-Fi and the address")});
  }

  // two steps: the address, then the connect code
  void askAddress() {
    auto alive = alive_;
    g_app->push(makeTextEntry(
        "Server address", g_config.server.url.empty() ? "https://" : g_config.server.url, "Next",
        "Like https://music.example.com or http://192.168.1.20:3000", 120, nullptr,
        [](const std::string& url, std::function<void(const std::string&)> answer) {
          answer(url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0 ? "" : "Start with https:// or http://");
        },
        [this, alive](const std::string& typed) {
          if (!*alive) return;
          std::string url = typed;
          while (!url.empty() && url.back() == '/') url.pop_back();
          askCode(url);
        }));
  }

  void askCode(const std::string& url) {
    auto alive = alive_;
    auto digits = [](const std::string& s) {
      std::string d;
      for (char c : s)
        if (c >= '0' && c <= '9') d += c;
      return d;
    };
    g_app->push(makeTextEntry(
        "Connect code", "", "Connect", "On the server's admin page: Device \xe2\x80\xba Connect a Rondo", 6,
        [digits](const std::string& typed) {
          std::string d = digits(typed);
          return d.size() > 3 ? d.substr(0, 3) + " " + d.substr(3) : d;
        },
        [this, alive, url, digits](const std::string& typed, std::function<void(const std::string&)> answer) {
          std::string code = digits(typed);
          if (code.size() != 6) return answer("The code has 6 digits");
          std::string json = "{\"code\":\"" + code + "\",\"name\":\"Rondo\"}";
          g_core.post(url + "/device/pair", json, [this, alive, url, answer](int status, const std::string& body) {
            if (status == 403) return answer("Wrong or expired code");
            if (status == 0) return answer("The server didn't answer");
            if (status < 200 || status >= 300) return answer("The server answered HTTP " + std::to_string(status));
            std::string token;
            if (yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0)) {
              yyjson_val* t = yyjson_obj_get(yyjson_doc_get_root(doc), "token");
              if (yyjson_is_str(t)) token = yyjson_get_str(t);
              yyjson_doc_free(doc);
            }
            if (token.empty()) return answer("Wrong or expired code");
            useServer(url, token);
            fprintf(stderr, "rondo: server: connected with a connect code\n");
            if (*alive) test_ = {entry("Connected", "Run the test to check it")};
            answer("");
          });
        },
        nullptr));
  }

  void askToken() {
    auto alive = alive_;
    g_app->push(makeTextEntry(
        "API token", "", "Save", "The API_TOKEN the server was started with", 128, nullptr,
        [this, alive](const std::string& token, std::function<void(const std::string&)> answer) {
          if (token.size() < 8) return answer("That's too short for a token");
          useServer(g_config.server.url, token);
          if (*alive) test_ = {entry("Token saved", "Run the test to check it")};
          answer("");
        },
        nullptr));
  }

  std::vector<Row> test_;  // the last connection test's answer
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

std::unique_ptr<Screen> makeServer() { return std::unique_ptr<Screen>(new ServerScreen()); }

// Settings' row (SettingsScreen.serverState): what the server says about itself (GET /healthz) and
// whether it takes this Rondo's token (GET /time)
bool directMode() { return g_config.server.direct || g_config.server.url.empty(); }
std::string apiBase() { return directMode() ? "http://127.0.0.1:47811" : g_config.server.url; }  // direct/server.h's kBase
std::string apiToken() { return directMode() ? std::string("direct") : g_config.server.token; }

void serverState(std::function<void(const std::string&)> done) {
  if (directMode()) return done("Directly to YouTube");
  if (g_config.server.url.empty()) return done("Not set up");
  g_core.get("/healthz", 0, [done](int status, const std::string& body) {
    Health h;
    if (status != 200 || !parseHealth(body, h)) return done("Unreachable");
    takeAccounts(h);
    g_core.get("/time", 0, [done, h](int status, const std::string&) {
      if (status == 401 || status == 403) return done("Token refused");
      if (status == 0) return done("Unreachable");
      if (serverTooOld(h.version)) return done("Server needs an update");
      if (h.ytdlp.empty()) return done("yt-dlp missing");
      if (h.lastResolveOk == 0) return done("Playback failing");
      done(h.music || h.videos ? "OK \xc2\xb7 signed in" : "OK \xc2\xb7 signed out");
    });
  });
}

// the main menu's check: whether the server has an account, so YouTube and YouTube Music open with the
// right tabs (offline, the last known answer stands)
void checkAccounts() {
  g_core.get("/healthz", 0, [](int status, const std::string& body) {
    Health h;
    if (status == 200 && parseHealth(body, h)) takeAccounts(h);
  });
}

}  // namespace rondo::shell

namespace rondo::shell {

// SoftwareUpdate.firmware(): this Y1's firmware number (ro.rondo.firmware, written by firmware/build.sh),
// else 1 for the first Rondo firmware (1.0.0 had none), else 0
int firmwareLevel() {
  char v[PROP_VALUE_MAX] = {0};
  if (__system_property_get("ro.rondo.firmware", v) > 0) return atoi(v);
  __system_property_get("ro.build.display.id", v);
  return strncmp(v, "Rondo", 5) == 0 ? 1 : 0;
}

// GET /update's "version" (empty when the server knows none)
std::string updateVersion(const std::string& json) {
  std::string v;
  if (yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0)) {
    yyjson_val* s = yyjson_obj_get(yyjson_doc_get_root(doc), "version");
    if (yyjson_is_str(s)) v = yyjson_get_str(s);
    yyjson_doc_free(doc);
  }
  return v;
}

// "1.0.10" is newer than "1.0.9" (server/src/updates.ts compares the same way); a part that isn't a
// number makes any different version newer
bool serverTooOld(const std::string& version) { return version.empty() || isNewerVersion(kServerVersionNeeded, version); }

std::string serverUpdateNeeded() {
  if (directMode() || g_config.server.url.empty() || !g_serverChecked || !serverTooOld(g_serverVersion)) return "";
  return std::string("Your Rondo server is too old for this Rondo: it needs ") + kServerVersionNeeded + " or newer" +
         (g_serverVersion.empty() ? "" : ", yours is " + g_serverVersion) + ".\nUpdate it on the server's admin page.";
}

bool isNewerVersion(const std::string& latest, const std::string& current) {
  if (latest.empty() || current.empty()) return false;
  auto parts = [](const std::string& s) {
    std::vector<std::string> out(1);
    for (char c : s) {
      if (c == '.' || c == '-') out.emplace_back();
      else out.back() += c;
    }
    return out;
  };
  auto num = [](const std::string& p, long& out) {
    if (p.empty()) return false;
    char* end = nullptr;
    out = strtol(p.c_str(), &end, 10);
    return *end == 0;
  };
  std::vector<std::string> a = parts(latest), b = parts(current);
  for (size_t i = 0; i < std::max(a.size(), b.size()); i++) {
    long x, y;
    if (!num(i < a.size() ? a[i] : "0", x) || !num(i < b.size() ? b[i] : "0", y)) return latest != current;
    if (x != y) return x > y;
  }
  return false;
}

}  // namespace rondo::shell
