// tv_auth.h.
#include "tv_auth.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <random>
#include <thread>

#include "innertube.h"
#include "json.h"
#include "model.h"

namespace rondo::direct::tv {
namespace {

const char* kCobalt = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version";
const char* kScope = "http://gdata.youtube.com https://www.googleapis.com/auth/youtube-paid-content";

struct Tokens {
  std::string clientId, clientSecret, access, refresh;
  int64_t expires = 0;  // s since 1970
};

std::mutex g_mu;
Tokens g_tokens;
bool g_loaded = false;
std::string g_name, g_handle, g_error;  // the account, and how the last sign-in ended
struct Pending {
  std::string url, code;
  int64_t expiresAt = 0;  // ms since 1970
} g_pending;
std::atomic<int> g_attempt{0};  // a newer code or a cancel makes an older one's approval worthless
std::atomic<int> g_generation{0};

int64_t nowS() { return int64_t(time(nullptr)); }
std::string file() { return dataDir().empty() ? "" : dataDir() + "/youtube-tv.json"; }

Doc parse(const std::string& s) { return Doc(yyjson_read(s.data(), s.size(), 0)); }

// under g_mu
void save() {
  std::string f = file();
  if (f.empty()) return;
  if (g_tokens.refresh.empty()) {
    unlink(f.c_str());
    return;
  }
  std::string j = "{\"client_id\":" + quote(g_tokens.clientId) + ",\"client_secret\":" + quote(g_tokens.clientSecret) +
                  ",\"access_token\":" + quote(g_tokens.access) + ",\"refresh_token\":" + quote(g_tokens.refresh) +
                  ",\"expires\":" + std::to_string(g_tokens.expires) + ",\"name\":" + quote(g_name) + ",\"handle\":" + quote(g_handle) + "}";
  std::string tmp = f + ".part";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);  // the account: nobody else's to read
  if (fd < 0) return;
  bool ok = write(fd, j.data(), j.size()) == ssize_t(j.size());
  ok = close(fd) == 0 && ok;
  if (ok) rename(tmp.c_str(), f.c_str());
  else unlink(tmp.c_str());
}
// under g_mu: the saved sign-in, once
void load() {
  if (g_loaded) return;
  std::string f = file();
  if (f.empty()) return;  // no folder yet: asked again later
  g_loaded = true;
  FILE* in = fopen(f.c_str(), "rb");
  if (!in) return;
  std::string s;
  char b[4096];
  for (size_t n; (n = fread(b, 1, sizeof(b), in)) > 0;) s.append(b, n);
  fclose(in);
  Doc d = parse(s);
  J r = root(d);
  g_tokens.clientId = str(key(r, "client_id")), g_tokens.clientSecret = str(key(r, "client_secret"));
  g_tokens.access = str(key(r, "access_token")), g_tokens.refresh = str(key(r, "refresh_token"));
  g_tokens.expires = num(key(r, "expires"));
  g_name = str(key(r, "name")), g_handle = str(key(r, "handle"));
  if (!g_tokens.refresh.empty()) g_generation++;
}

// The TV app's client id and secret, from its script (youtubei.js's OAuth2.getClientID)
bool clientIdentity(std::string& id, std::string& secret) {
  Fetched page = get("https://www.youtube.com/tv", {std::string("User-Agent: ") + kCobalt, "Referer: https://www.youtube.com/tv", "Accept-Language: en-US"}, 4 << 20);
  if (page.status != 200) return false;
  size_t at = page.body.find("id=\"base-js\"");
  if (at == std::string::npos) return false;
  size_t src = page.body.find("src=\"", at);
  if (src == std::string::npos || src > at + 200) return false;
  src += 5;
  std::string url = page.body.substr(src, page.body.find('"', src) - src);
  if (url.compare(0, 4, "http") != 0) url = "https://www.youtube.com" + url;
  Fetched js = get(url, {std::string("User-Agent: ") + kCobalt}, 24 << 20);
  if (js.status != 200) return false;
  // clientId:"<id>",<no quotes>:"<secret>"
  const std::string& s = js.body;
  for (size_t p = s.find("clientId:\""); p != std::string::npos; p = s.find("clientId:\"", p + 1)) {
    size_t a = p + 10, b = s.find('"', a);
    if (b == std::string::npos || s.compare(b, 2, "\",") != 0) continue;
    size_t q = s.find('"', b + 2);
    if (q == std::string::npos || s[q - 1] != ':') continue;
    size_t e = s.find('"', q + 1);
    if (e == std::string::npos) continue;
    id = s.substr(a, b - a), secret = s.substr(q + 1, e - q - 1);
    return !id.empty() && !secret.empty();
  }
  return false;
}

std::string uuid() {
  std::random_device rd;
  char b[40];
  snprintf(b, sizeof(b), "%08x-%04x-4%03x-%04x-%08x%04x", rd(), rd() & 0xffff, rd() & 0xfff, (rd() & 0x3fff) | 0x8000, rd(), rd() & 0xffff);
  return b;
}

// who the tokens sign in as (youtubei.js's account.getInfo: the TV client's accounts list)
void lookUpAccount() {
  Doc d;
  if (innertube(rules().tv, "account/accounts_list", "", d) != 200) return;
  J item = find(root(d), "accountItem");
  if (!item) item = find(root(d), "accountItemRenderer");
  std::string name = item ? text(key(item, "accountName")) : "", handle = item ? text(key(item, "channelHandle")) : "";
  std::lock_guard<std::mutex> l(g_mu);
  if (!name.empty()) g_name = name, g_handle = handle, save();
}

// the approval: asked every `interval` s until it comes, is refused or the code expires
void poll(int attempt, std::string deviceCode, int interval, int64_t until, std::string id, std::string secret) {
  std::string body = "{\"client_id\":" + quote(id) + ",\"client_secret\":" + quote(secret) + ",\"code\":" + quote(deviceCode) +
                     ",\"grant_type\":\"http://oauth.net/grant_type/device/1.0\"}";
  while (g_attempt == attempt && nowS() * 1000 < until) {
    sleep(unsigned(std::max(2, interval)));
    if (g_attempt != attempt) return;
    Fetched r = post("https://www.youtube.com/o/oauth2/token", body, {"Content-Type: application/json"});
    Doc d = parse(r.body);
    std::string err = str(key(root(d), "error"));
    if (err == "authorization_pending") continue;
    if (err == "slow_down") {
      interval += 2;
      continue;
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (g_attempt != attempt) return;
    g_pending = Pending();
    if (!err.empty() || r.status != 200) {
      g_error = err == "access_denied" ? "denied" : err == "expired_token" ? "expired" : "failed";
      fprintf(stderr, "rondo-direct: TV sign-in: %s\n", err.empty() ? std::to_string(r.status).c_str() : err.c_str());
      return;
    }
    g_tokens.clientId = id, g_tokens.clientSecret = secret;
    g_tokens.access = str(key(root(d), "access_token")), g_tokens.refresh = str(key(root(d), "refresh_token"));
    g_tokens.expires = nowS() + num(key(root(d), "expires_in"), 3600) - 60;
    g_name.clear(), g_handle.clear(), g_error.clear();
    save();
    g_generation++;
    fprintf(stderr, "rondo-direct: signed in to YouTube with a code\n");
    std::thread(lookUpAccount).detach();
    return;
  }
  std::lock_guard<std::mutex> l(g_mu);
  if (g_attempt == attempt && !g_pending.code.empty()) g_pending = Pending(), g_error = "expired";
}

}  // namespace

std::string stateJson() {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  bool in = !g_tokens.refresh.empty();
  std::string pending = "null";
  if (!g_pending.code.empty() && g_pending.expiresAt > nowS() * 1000)
    pending = "{\"url\":" + quote(g_pending.url) + ",\"code\":" + quote(g_pending.code) + ",\"expiresAt\":" + std::to_string(g_pending.expiresAt) + "}";
  return std::string("{\"signedIn\":") + (in ? "true" : "false") + ",\"name\":" + quote(g_name) + ",\"handle\":" + quote(g_handle) +
         ",\"pending\":" + pending + ",\"error\":" + (g_error.empty() ? "null" : quote(g_error)) + "}";
}

std::string begin() {
  int attempt = ++g_attempt;
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_error.clear();
    g_pending = Pending();
  }
  std::string id, secret;
  if (!clientIdentity(id, secret)) {
    std::lock_guard<std::mutex> l(g_mu);
    g_error = "failed";
    fprintf(stderr, "rondo-direct: TV sign-in: no client id from the TV app\n");
    return stateJson();
  }
  std::string body = "{\"client_id\":" + quote(id) + ",\"scope\":" + quote(kScope) + ",\"device_id\":" + quote(uuid()) + ",\"device_model\":\"ytlr::\"}";
  Fetched r = post("https://www.youtube.com/o/oauth2/device/code", body, {"Content-Type: application/json"});
  Doc d = parse(r.body);
  J j = root(d);
  std::string device = str(key(j, "device_code")), code = str(key(j, "user_code"));
  if (r.status != 200 || device.empty() || code.empty()) {
    std::lock_guard<std::mutex> l(g_mu);
    g_error = "failed";
    fprintf(stderr, "rondo-direct: TV sign-in: no code (%d)\n", r.status);
    return stateJson();
  }
  int64_t until = nowS() * 1000 + num(key(j, "expires_in"), 1800) * 1000;
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_attempt != attempt) return stateJson();
    g_pending.url = str(key(j, "verification_url"));
    if (g_pending.url.empty()) g_pending.url = "https://www.google.com/device";
    g_pending.code = code, g_pending.expiresAt = until;
  }
  std::thread(poll, attempt, device, int(num(key(j, "interval"), 5)), until, id, secret).detach();
  return stateJson();
}

void cancel() {
  g_attempt++;
  std::lock_guard<std::mutex> l(g_mu);
  g_pending = Pending();
}

void signOut() {
  g_attempt++;
  std::string token;
  {
    std::lock_guard<std::mutex> l(g_mu);
    load();
    token = g_tokens.access.empty() ? g_tokens.refresh : g_tokens.access;
    g_tokens = Tokens(), g_name.clear(), g_handle.clear(), g_error.clear(), g_pending = Pending();
    save();  // the file goes
    g_generation++;
  }
  // given back to Google too (youtubei.js's revokeCredentials)
  if (!token.empty()) post("https://www.youtube.com/o/oauth2/revoke?token=" + token, "", {});
  fprintf(stderr, "rondo-direct: the code sign-in removed\n");
}

bool signedIn() {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  return !g_tokens.refresh.empty();
}

int generation() {
  std::lock_guard<std::mutex> l(g_mu);
  load();
  return g_generation;
}

std::string bearer() {
  std::unique_lock<std::mutex> l(g_mu);
  load();
  if (g_tokens.refresh.empty()) return "";
  if (!g_tokens.access.empty() && nowS() < g_tokens.expires) return g_tokens.access;
  // expired: a new access token (one refresh at a time, the others wait for it)
  Tokens t = g_tokens;
  std::string body = "{\"client_id\":" + quote(t.clientId) + ",\"client_secret\":" + quote(t.clientSecret) + ",\"refresh_token\":" + quote(t.refresh) +
                     ",\"grant_type\":\"refresh_token\"}";
  Fetched r = post("https://www.youtube.com/o/oauth2/token", body, {"Content-Type: application/json"});
  Doc d = parse(r.body);
  std::string access = str(key(root(d), "access_token")), err = str(key(root(d), "error"));
  if (r.status == 200 && !access.empty()) {
    g_tokens.access = access;
    g_tokens.expires = nowS() + num(key(root(d), "expires_in"), 3600) - 60;
    save();
    return access;
  }
  if (err == "invalid_grant") {  // revoked from the Google account: signed out here too
    fprintf(stderr, "rondo-direct: the code sign-in no longer works: sign in again\n");
    g_tokens = Tokens(), g_name.clear(), g_handle.clear(), g_error = "revoked";
    save();
    g_generation++;
  }
  return "";  // no network now: signed-out answers until it's back
}

}  // namespace rondo::direct::tv
