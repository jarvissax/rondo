// Settings › Server (ServerScreen.kt in 1.0.x): the Rondo server this Y1 uses, a connection test, what
// uses it (YouTube when chosen, WhatsApp), and connecting to one with its address and the code from its
// admin page (docs/dev/server-api.md), or with a token for servers started with API_TOKEN. Since 3.0.4
// it stands apart from YouTube, whose own setting picks it or YouTube directly. Also Settings' Server
// row (serverState) and the account check the main menu makes (Accounts.kt).
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
  g_config.server.url = url, g_config.server.token = token;
  saveConfigNow();
  if (g_onServerChanged) g_onServerChanged();
}

}  // namespace

// Settings › YouTube › Connect to: YouTube directly, or through the server (only with one connected)
void useDirect(bool on) {
  if (g_config.server.direct == on) return;
  if (!on && g_config.server.url.empty()) return;
  g_config.server.direct = on;
  saveConfigNow();
  if (g_onServerChanged) g_onServerChanged();
}

namespace {

Row entry(const std::string& title, const std::string& subtitle = "", const std::string& trailing = "") {
  Row r;
  r.title = title, r.subtitle = subtitle, r.trailing = trailing;
  return r;
}

// what a test says of playback: 1 working, 0 failing, else not tried yet (Health.lastResolveOk)
std::string playback(int ok) { return ok == 1 ? tr("server.working") : ok == 0 ? tr("server.failing") : tr("server.notTried"); }

class ServerScreen : public ShellListScreen {
 public:
  ServerScreen() : ShellListScreen(tr("server.title")) { render(); }
  ~ServerScreen() override { *alive_ = false; }
  void onShow() override { render(); }

 private:
  void render() {
    std::vector<Row> rows;
    bool none = g_config.server.url.empty();
    rows.push_back(header(tr("server.status")));
    Row address = entry(tr("server.address"), "", none ? tr("server.noAddress") : hostOf(g_config.server.url));
    address.tile = &tiles::server;
    rows.push_back(address);
    Row test = entry(tr("settings.connectionTest"), tr("server.testNote"));
    test.onSelect = [this] { runTest(); };
    rows.push_back(test);
    if (!test_.empty()) {
      rows.push_back(header(tr("server.test")));
      for (const Row& r : test_) rows.push_back(r);
    }
    // what uses it: YouTube when chosen there (Settings › YouTube › Connect to), WhatsApp always
    rows.push_back(header(tr("server.usedFor")));
    Row yt = entry("YouTube", none ? tr("server.youtubeDirect") : directMode() ? tr("server.youtubeDirectChosen") : tr("server.youtubeThrough"));
    rows.push_back(yt);
    rows.push_back(entry("WhatsApp", none ? tr("server.whatsappNeeds") : tr("server.whatsappLinked")));
    rows.push_back(header(tr("server.connect")));
    Row connect = entry(tr("server.connectTo"), tr("server.connectToNote"));
    connect.chevron = true;
    connect.onSelect = [this] { askAddress(); };
    rows.push_back(connect);
    Row token = entry(tr("server.useToken"), tr("server.useTokenNote"));
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
    if (g_config.server.url.empty()) return show({entry(tr("server.noServer"), tr("server.noServerNote"))});
    Row calling = entry(tr("server.calling"));
    calling.enabled = false;
    show({calling});
    auto alive = alive_;
    int64_t started = now_ns();  // the real clock: g_app->now() is the frame's
    // the server itself, whatever YouTube's source (/@server/: Api::urlFor)
    g_core.get("/@server/healthz", 0, [this, alive, started](int status, const std::string& body) {
      if (!*alive) return;
      int ms = int((now_ns() - started) / 1000000);
      Health h;
      if (status == 200 && !parseHealth(body, h)) status = -1;
      if (status != 200) return failed(status);
      if (!directMode()) takeAccounts(h);
      g_core.get("/@server/time", 0, [this, alive, ms, h](int status, const std::string&) {  // needs the token
        if (!*alive) return;
        if (status != 200) return failed(status);
        std::vector<Row> rows = {entry(tr("server.answeredIn"), "", std::to_string(ms) + " ms")};
        if (serverTooOld(h.version))
          rows.push_back(entry(tr("server.needsUpdate"), tr("server.needsVersion", {{"version", kServerVersionNeeded}})));
        rows.push_back(entry(tr("server.title"), "", h.version.empty() ? tr("server.olderThan", {{"version", "3.1.0"}}) : h.version));
        rows.push_back(entry("yt-dlp", "", h.ytdlp.empty() ? tr("server.missing") : h.ytdlp));
        rows.push_back(entry(tr("server.playback"), "", playback(h.lastResolveOk)));
        rows.push_back(entry(tr("settings.youtube.account"), "", h.music || h.videos ? tr("server.signedIn") : tr("server.signedOut")));
        show(rows);
      });
    });
  }
  // HttpException's codes, or no answer at all (0) or an answer that isn't the server's (-1)
  void failed(int status) {
    if (status == 401 || status == 403) show({entry(tr("server.refused"), tr("server.newCode"))});
    else if (status > 0) show({entry(tr("server.answeredHttp", {{"status", status}}), tr("server.newCode"))});
    else show({entry(tr("common.serverNoAnswer"), tr("server.checkAddress"))});
  }

  // two steps: the address, then the connect code
  void askAddress() {
    auto alive = alive_;
    g_app->push(makeTextEntry(
        tr("server.addressTitle"), g_config.server.url.empty() ? "https://" : g_config.server.url, tr("server.next"),
        tr("server.addressNote"), 120, nullptr,
        [](const std::string& url, std::function<void(const std::string&)> answer) {
          answer(url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0 ? "" : tr("server.addressStart"));
        },
        [this, alive](const std::string& typed) {
          if (!*alive) return;
          std::string url = typed;
          while (!url.empty() && url.back() == '/') url.pop_back();
          askCode(url);
        }, false));
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
        tr("server.code"), "", tr("server.connectButton"), tr("server.codeNote"), 6,
        [digits](const std::string& typed) {
          std::string d = digits(typed);
          return d.size() > 3 ? d.substr(0, 3) + " " + d.substr(3) : d;
        },
        [this, alive, url, digits](const std::string& typed, std::function<void(const std::string&)> answer) {
          std::string code = digits(typed);
          if (code.size() != 6) return answer(tr("server.codeDigits"));
          std::string json = "{\"code\":\"" + code + "\",\"name\":\"Rondo\"}";
          g_core.post(url + "/device/pair", json, [this, alive, url, answer](int status, const std::string& body) {
            if (status == 403) return answer(tr("server.wrongCode"));
            if (status == 0) return answer(tr("common.serverNoAnswer"));
            if (status < 200 || status >= 300) return answer(tr("server.answeredHttp", {{"status", status}}));
            std::string token;
            if (yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0)) {
              yyjson_val* t = yyjson_obj_get(yyjson_doc_get_root(doc), "token");
              if (yyjson_is_str(t)) token = yyjson_get_str(t);
              yyjson_doc_free(doc);
            }
            if (token.empty()) return answer(tr("server.wrongCode"));
            useServer(url, token);
            fprintf(stderr, "rondo: server: connected with a connect code\n");
            if (*alive) test_ = {entry(tr("server.connected"), tr("server.runTest"))};
            answer("");
          });
        },
        nullptr, false));
  }

  void askToken() {
    auto alive = alive_;
    g_app->push(makeTextEntry(
        tr("server.token"), "", tr("common.save"), tr("server.tokenNote"), 128, nullptr,
        [this, alive](const std::string& token, std::function<void(const std::string&)> answer) {
          if (token.size() < 8) return answer(tr("server.tokenShort"));
          useServer(g_config.server.url, token);
          if (*alive) test_ = {entry(tr("server.tokenSaved"), tr("server.runTest"))};
          answer("");
        },
        nullptr, false));
  }

  std::vector<Row> test_;  // the last connection test's answer
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

std::unique_ptr<Screen> makeServer() { return std::unique_ptr<Screen>(new ServerScreen()); }

namespace {

// Settings › YouTube › Connection test: directly, a search answered and a song's streams found (what
// playing needs); through the server, the server's own test (healthz and the token)
class YouTubeTestScreen : public ShellListScreen {
 public:
  YouTubeTestScreen() : ShellListScreen(tr("settings.connectionTest")) {
    Row r = entry(directMode() ? tr("server.askingYouTube") : tr("server.calling"));
    r.enabled = false;
    list_.setRows({r});
    auto alive = alive_;
    int64_t started = now_ns();
    if (!directMode()) {
      g_core.get("/healthz", 0, [this, alive, started](int status, const std::string& body) {
        if (!*alive) return;
        int ms = int((now_ns() - started) / 1000000);
        Health h;
        if (status != 200 || !parseHealth(body, h)) return show({entry(tr("common.serverNoAnswer"), tr("server.checkSettings"))});
        takeAccounts(h);
        show({entry(tr("server.answeredIn"), "", std::to_string(ms) + " ms"), entry(tr("server.playback"), "", playback(h.lastResolveOk)),
              entry(tr("settings.youtube.account"), "", h.music || h.videos ? tr("server.signedIn") : tr("server.signedOut"))});
      });
      return;
    }
    g_core.get("/music/search?type=song&q=" + Api::escape("Rondo alla turca"), 0, [this, alive, started](int status, const std::string& body) {
      if (!*alive) return;
      int ms = int((now_ns() - started) / 1000000);
      if (status != 200)
        return show({entry(status == 0 ? tr("server.youtubeNoAnswer") : tr("server.youtubeHttp", {{"status", status}}), tr("server.checkWifi"))});
      std::string id;
      if (yyjson_doc* doc = yyjson_read(body.data(), body.size(), 0)) {
        yyjson_val* first = yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(doc), "items"), 0);
        if (yyjson_is_str(yyjson_obj_get(first, "id"))) id = yyjson_get_str(yyjson_obj_get(first, "id"));
        yyjson_doc_free(doc);
      }
      g_core.get("/resolve/" + id, 0, [this, alive, ms](int status, const std::string&) {
        if (!*alive) return;
        show({entry(tr("server.answeredIn"), "", std::to_string(ms) + " ms"), entry(tr("server.playback"), "", playback(status == 200 ? 1 : 0))});
      });
    });
  }
  ~YouTubeTestScreen() override { *alive_ = false; }

 private:
  void show(std::vector<Row> rows) { list_.setRows(std::move(rows)); }
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace

std::unique_ptr<Screen> makeYouTubeTest() { return std::unique_ptr<Screen>(new YouTubeTestScreen()); }

// Settings' Server row: what the server says about itself (GET /healthz) and whether it takes this
// Rondo's token (GET /time), whatever YouTube's source
bool directMode() { return g_config.server.direct || g_config.server.url.empty(); }
std::string apiBase() { return directMode() ? "http://127.0.0.1:47811" : g_config.server.url; }  // direct/server.h's kBase
std::string apiToken() { return directMode() ? std::string("direct") : g_config.server.token; }
bool serverConnected() { return !g_config.server.url.empty() && !g_config.server.token.empty(); }
std::string serverUrl() { return g_config.server.url; }
std::string serverToken() { return g_config.server.token; }

void serverState(std::function<void(const std::string&)> done) {
  if (g_config.server.url.empty()) return done(tr("server.notSetUp"));
  g_core.get("/@server/healthz", 0, [done](int status, const std::string& body) {
    Health h;
    if (status != 200 || !parseHealth(body, h)) return done(tr("server.unreachable"));
    if (!directMode()) takeAccounts(h);
    g_core.get("/@server/time", 0, [done, h](int status, const std::string&) {
      if (status == 401 || status == 403) return done(tr("server.tokenRefused"));
      if (status == 0) return done(tr("server.unreachable"));
      if (serverTooOld(h.version)) return done(tr("server.needsUpdate"));
      if (h.ytdlp.empty()) return done(tr("server.ytdlpMissing"));
      if (h.lastResolveOk == 0) return done(tr("server.playbackFailing"));
      done(h.music || h.videos ? tr("server.okSignedIn") : tr("server.okSignedOut"));
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
  if (g_serverVersion.empty()) return tr("server.tooOld", {{"needed", kServerVersionNeeded}});
  return tr("server.tooOldYours", {{"needed", kServerVersionNeeded}, {"yours", g_serverVersion}});
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
