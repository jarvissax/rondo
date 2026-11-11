// innertube.h.
#include "innertube.h"

#include <openssl/sha.h>
#include <sys/stat.h>
#include <time.h>

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include <mutex>

#include "json.h"
#ifdef __ANDROID__
#include "../core/net.h"
#endif

namespace rondo::direct {
namespace {

std::mutex g_mu;
std::string g_dataDir, g_visitor;
Rules g_rules;
bool g_rulesLoaded = false;

const char* kDesktopUa = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/134.0.0.0 Safari/537.36";

// the clients as yt-dlp 2026.10 and youtubei.js 18.3 send them (Nov 9, 2026)
Rules builtIn() {
  Rules r;
  r.music = {"WEB_REMIX", "1.20260707.12.00", kDesktopUa, "music.youtube.com", 67, {}};
  r.web = {"WEB", "2.20260708.00.00", kDesktopUa, "www.youtube.com", 1, {}};
  r.tv = {"TVHTML5", "7.20260707.07.00",
          "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold (unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)",
          "www.youtube.com", 7, {}};
  r.player = {"VISIONOS", "1.02",
              "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15",
              "www.youtube.com", 101,
              {{"deviceMake", "Apple"}, {"deviceModel", "RealityDevice17,1"}, {"osName", "visionOS"}, {"osVersion", "26.5.23O471"}}};
  r.androidMusic = {"ANDROID_MUSIC", "7.21.50", "com.google.android.apps.youtube.music/7.21.50 (Linux; U; Android 14) gzip",
                    "music.youtube.com", 21,
                    {{"androidSdkVersion", "34"}, {"osName", "Android"}, {"osVersion", "14"}, {"platform", "MOBILE"}, {"clientFormFactor", "SMALL_FORM_FACTOR"}}};
  return r;
}

std::string readFile(const std::string& path) {
  std::string s;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
  }
  return s;
}

void writeFile(const std::string& path, const std::string& s) {
  std::string tmp = path + ".part";
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp.c_str(), path.c_str());
  }
}

// <data>/youtube-rules.json: {"music": {"name", "version", "userAgent", "host", "id", "extra": {…}}, …}
void loadRules() {
  g_rules = builtIn();
  g_rulesLoaded = true;
  if (g_dataDir.empty()) return;
  std::string s = readFile(g_dataDir + "/youtube-rules.json");
  yyjson_doc* d = s.empty() ? nullptr : yyjson_read(s.data(), s.size(), 0);
  if (!d) return;
  J r = yyjson_doc_get_root(d);
  auto take = [&](const char* name, Client& c) {
    J o = key(r, name);
    if (!o) return;
    if (isStr(key(o, "name"))) c.name = str(key(o, "name"));
    if (isStr(key(o, "version"))) c.version = str(key(o, "version"));
    if (isStr(key(o, "userAgent"))) c.userAgent = str(key(o, "userAgent"));
    if (isStr(key(o, "host"))) c.host = str(key(o, "host"));
    if (key(o, "id")) c.id = int(num(key(o, "id")));
    if (J e = key(o, "extra")) {
      c.extra.clear();
      size_t i, n;
      J k, v;
      yyjson_obj_foreach(e, i, n, k, v) c.extra.push_back({str(k), isStr(v) ? str(v) : std::to_string(num(v))});
    }
  };
  take("music", g_rules.music);
  take("web", g_rules.web);
  take("tv", g_rules.tv);
  take("player", g_rules.player);
  take("androidMusic", g_rules.androidMusic);
  yyjson_doc_free(d);
  fprintf(stderr, "rondo-direct: rules from %s/youtube-rules.json\n", g_dataDir.c_str());
}

// one handle a thread: its connections stay open between requests
CURL* handle() {
  static thread_local CURL* c = nullptr;
  if (!c) c = curl_easy_init();
  else curl_easy_reset(c);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#ifdef __ANDROID__
  net::trust(c, true);  // Android 4.2's roots, Let's Encrypt's and Google's own (native/assets/certs)
#endif
  return c;
}

struct Collect {
  std::string* body;
  size_t max;
};
size_t collect(char* d, size_t s, size_t n, void* p) {
  Collect* c = static_cast<Collect*>(p);
  size_t len = s * n;
  if (c->body->size() + len > c->max) return 0;
  c->body->append(d, len);
  return len;
}

struct Heads {
  std::string type, range;
  int64_t length = -1;
};
size_t onHeader(char* d, size_t s, size_t n, void* p) {
  Heads* h = static_cast<Heads*>(p);
  size_t len = s * n;
  std::string line(d, len);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  auto value = [&](const char* name) -> const char* {
    size_t l = strlen(name);
    return line.size() > l && strncasecmp(line.c_str(), name, l) == 0 ? line.c_str() + l : nullptr;
  };
  if (line.compare(0, 5, "HTTP/") == 0) *h = Heads();  // a redirect's own headers don't count
  else if (const char* v = value("content-type:")) h->type = trim(v);
  else if (const char* v = value("content-range:")) h->range = trim(v);
  else if (const char* v = value("content-length:")) h->length = atoll(v);
  return len;
}

}  // namespace

void setDataDir(const std::string& dir) {
  std::lock_guard<std::mutex> l(g_mu);
  g_dataDir = dir;
  g_visitor = dir.empty() ? "" : trim(readFile(dir + "/youtube-visitor"));
  loadRules();
}

// --- the account ---

namespace {

struct Account {
  std::string header;                        // Cookie: for youtube.com
  std::string sapisid, sapisid1p, sapisid3p;  // what SAPISIDHASH signs with
};
std::mutex g_accMu;
Account g_account;
int64_t g_accStatMs = 0, g_accMtime = -1, g_accSize = -1;
int g_accGen = 0;
int g_accChecked = -1;  // the generation the last account menu was for
int64_t g_accCheckedMs = 0;
std::string g_accJson;
bool g_accSignedIn = true;

int64_t monoMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

Account parseCookies(const std::string& text) {
  Account a;
  time_t now = time(nullptr);
  size_t at = 0;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
    at = nl == std::string::npos ? text.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, 10, "#HttpOnly_") == 0) line = line.substr(10);
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    for (size_t a0 = 0;;) {
      size_t t = line.find('\t', a0);
      f.push_back(line.substr(a0, t == std::string::npos ? std::string::npos : t - a0));
      if (t == std::string::npos) break;
      a0 = t + 1;
    }
    if (f.size() < 7) continue;
    const std::string& domain = f[0];
    bool yt = domain == "youtube.com" || (domain.size() >= 12 && domain.compare(domain.size() - 12, 12, ".youtube.com") == 0);
    if (!yt) continue;
    long long exp = atoll(f[4].c_str());
    if (exp > 0 && exp < now) continue;
    a.header += (a.header.empty() ? "" : "; ") + f[5] + "=" + f[6];
    if (f[5] == "SAPISID") a.sapisid = f[6];
    else if (f[5] == "__Secure-1PAPISID") a.sapisid1p = f[6];
    else if (f[5] == "__Secure-3PAPISID") a.sapisid3p = f[6];
  }
  if (a.sapisid.empty()) a.sapisid = a.sapisid3p;
  return a;
}

// the file read again when it changed (looked at every 2 s at most); under g_accMu
void refreshAccountLocked() {
  int64_t now = monoMs();
  if (g_accStatMs && now - g_accStatMs < 2000) return;
  g_accStatMs = now;
  std::string dir;
  {
    std::lock_guard<std::mutex> l(g_mu);
    dir = g_dataDir;
  }
  struct stat st;
  int64_t mtime = -1, size = -1;
  std::string file = dir + "/youtube-cookies.txt";
  if (!dir.empty() && stat(file.c_str(), &st) == 0) mtime = int64_t(st.st_mtime), size = int64_t(st.st_size);
  if (mtime == g_accMtime && size == g_accSize) return;
  g_accMtime = mtime, g_accSize = size;
  g_account = mtime < 0 ? Account() : parseCookies(readFile(file));
  g_accGen++;
  g_accSignedIn = !g_account.header.empty();
  fprintf(stderr, "rondo-direct: YouTube account %s\n", g_account.header.empty() ? "signed out" : "cookies read");
}

std::string sha1Hex(const std::string& s) {
  unsigned char md[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), md);
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned char b : md) out += hex[b >> 4], out += hex[b & 15];
  return out;
}

// SAPISIDHASH <time>_<sha1(time SAPISID origin)> (and the 1P and 3P forms when the cookies have them),
// as youtube.com's own pages send it
std::string authorization(const Account& a, const std::string& origin) {
  std::string ts = std::to_string(time(nullptr)), out;
  auto one = [&](const char* name, const std::string& sid) {
    if (sid.empty()) return;
    out += (out.empty() ? "" : " ") + std::string(name) + " " + ts + "_" + sha1Hex(ts + " " + sid + " " + origin);
  };
  one("SAPISIDHASH", a.sapisid);
  one("SAPISID1PHASH", a.sapisid1p);
  one("SAPISID3PHASH", a.sapisid3p);
  return out;
}

}  // namespace

bool hasCookies() {
  std::lock_guard<std::mutex> l(g_accMu);
  refreshAccountLocked();
  return !g_account.header.empty();
}
int accountGeneration() {
  std::lock_guard<std::mutex> l(g_accMu);
  refreshAccountLocked();
  return g_accGen;
}
bool signedIn() {
  std::lock_guard<std::mutex> l(g_accMu);
  refreshAccountLocked();
  return !g_account.header.empty() && g_accSignedIn;
}
bool isCookiesText(const std::string& text) { return !parseCookies(text).header.empty(); }
void reloadAccount() {
  std::lock_guard<std::mutex> l(g_accMu);
  g_accStatMs = 0;
  refreshAccountLocked();
}

// the account menu's header: who the cookies sign in as (none: they're signed out, or expired)
namespace {
void findHeader(J o, J& found, int depth = 0) {
  if (!o || found || depth > 30) return;
  if (yyjson_is_obj(o)) {
    if (J h = key(o, "activeAccountHeaderRenderer")) {
      found = h;
      return;
    }
    size_t k, n;
    J kk, v;
    yyjson_obj_foreach(o, k, n, kk, v) findHeader(v, found, depth + 1);
  } else if (yyjson_is_arr(o)) {
    size_t k, n;
    J v;
    yyjson_arr_foreach(o, k, n, v) findHeader(v, found, depth + 1);
  }
}
}  // namespace

std::string accountJson() {
  int gen;
  {
    std::lock_guard<std::mutex> l(g_accMu);
    refreshAccountLocked();
    if (g_account.header.empty()) return "{\"signedIn\":false}";
    gen = g_accGen;
    if (g_accChecked == gen && monoMs() - g_accCheckedMs < 10 * 60 * 1000) return g_accJson;
  }
  Doc d;
  int status = innertube(rules().web, "account/account_menu", "", d);
  if (status == 0) return "{\"signedIn\":true,\"unchecked\":true}";  // no connection: as far as we know
  J h = nullptr;
  findHeader(root(d), h);
  std::string name = h ? text(key(h, "accountName")) : "", handle = h ? text(key(h, "channelHandle")) : "";
  bool in = h != nullptr;
  std::string j = in ? "{\"signedIn\":true,\"name\":" + quote(name) + ",\"handle\":" + quote(handle) + "}"
                     : "{\"signedIn\":false,\"expired\":true}";
  std::lock_guard<std::mutex> l(g_accMu);
  if (gen == g_accGen) g_accChecked = gen, g_accCheckedMs = monoMs(), g_accJson = j, g_accSignedIn = in;
  return j;
}

const Rules& rules() {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_rulesLoaded) loadRules();
  return g_rules;
}

int innertube(const Client& c, const char* endpointName, const std::string& fields, Doc& out) {
  std::string visitor;
  {
    std::lock_guard<std::mutex> l(g_mu);
    visitor = g_visitor;
  }
  std::string client = "\"clientName\":" + quote(c.name) + ",\"clientVersion\":" + quote(c.version) + ",\"hl\":\"en\"";
  if (!visitor.empty()) client += ",\"visitorData\":" + quote(visitor);
  for (auto& [k, v] : c.extra) {
    // strings all, but the SDK level ("osVersion": "14" sent as a number is a 400)
    bool number = k == "androidSdkVersion" && !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
    client += "," + quote(k) + ":" + (number ? v : quote(v));
  }
  std::string body = "{\"context\":{\"client\":{" + client + "},\"user\":{\"lockedSafetyMode\":false}}";
  if (!fields.empty()) body += "," + fields;
  body += "}";

  CURL* h = handle();
  std::string url = "https://" + c.host + "/youtubei/v1/" + endpointName + "?prettyPrint=false";
  curl_slist* hs = nullptr;
  hs = curl_slist_append(hs, "Content-Type: application/json");
  hs = curl_slist_append(hs, ("X-YouTube-Client-Name: " + std::to_string(c.id)).c_str());
  hs = curl_slist_append(hs, ("X-YouTube-Client-Version: " + c.version).c_str());
  hs = curl_slist_append(hs, ("Origin: https://" + c.host).c_str());
  hs = curl_slist_append(hs, ("Referer: https://" + c.host + "/").c_str());
  hs = curl_slist_append(hs, "Accept-Language: en");
  if (!visitor.empty()) hs = curl_slist_append(hs, ("X-Goog-Visitor-Id: " + visitor).c_str());
  // signed in: the web clients carry the account (the others take no cookies)
  if (c.name == "WEB" || c.name == "WEB_REMIX") {
    Account a;
    {
      std::lock_guard<std::mutex> l(g_accMu);
      refreshAccountLocked();
      a = g_account;
    }
    if (!a.header.empty()) {
      std::string origin = "https://" + c.host;
      hs = curl_slist_append(hs, ("Cookie: " + a.header).c_str());
      if (!a.sapisid.empty()) hs = curl_slist_append(hs, ("Authorization: " + authorization(a, origin)).c_str());
      hs = curl_slist_append(hs, "X-Goog-AuthUser: 0");
      hs = curl_slist_append(hs, ("X-Origin: " + origin).c_str());
      hs = curl_slist_append(hs, "X-Youtube-Bootstrap-Logged-In: true");
    }
  }
  std::string answer;
  Collect col{&answer, 16u << 20};
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_USERAGENT, c.userAgent.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, long(body.size()));
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &col);
  CURLcode rc = curl_easy_perform(h);
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(hs);
  if (rc != CURLE_OK) {
    fprintf(stderr, "rondo-direct: %s %s: %s\n", c.name.c_str(), endpointName, curl_easy_strerror(rc));
    return 0;
  }
  out.reset(yyjson_read(answer.data(), answer.size(), YYJSON_READ_ALLOW_INVALID_UNICODE));
  if (out && visitor.empty()) {  // the first answer names this Y1's visitor: kept, as a browser keeps it
    std::string v = str(path(root(out), "responseContext", "visitorData"));
    if (!v.empty()) {
      std::lock_guard<std::mutex> l(g_mu);
      if (g_visitor.empty()) {
        g_visitor = v;
        if (!g_dataDir.empty()) writeFile(g_dataDir + "/youtube-visitor", v + "\n");
      }
    }
  }
  return int(status);
}

Fetched get(const std::string& url, const std::vector<std::string>& headers, size_t maxBytes) {
  Fetched f;
  CURL* h = handle();
  curl_slist* hs = nullptr;
  for (const std::string& x : headers) hs = curl_slist_append(hs, x.c_str());
  Collect col{&f.body, maxBytes};
  Heads heads;
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  if (hs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_USERAGENT, kDesktopUa);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &col);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &heads);
  CURLcode rc = curl_easy_perform(h);
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  if (hs) curl_slist_free_all(hs);
  f.status = rc == CURLE_OK ? int(status) : 0;
  f.type = heads.type;
  size_t slash = heads.range.rfind('/');
  if (slash != std::string::npos && heads.range.compare(slash + 1, 1, "*") != 0) f.total = atoll(heads.range.c_str() + slash + 1);
  return f;
}

int getStreamed(const std::string& url, const std::vector<std::string>& headers,
                const std::function<bool(int, const std::string&, int64_t, const std::string&)>& head,
                const std::function<bool(const char*, size_t)>& sink) {
  struct State {
    CURL* h;
    Heads heads;
    bool started = false, stop = false;
    const std::function<bool(int, const std::string&, int64_t, const std::string&)>* head;
    const std::function<bool(const char*, size_t)>* sink;
    bool begin() {
      started = true;
      long status = 0;
      curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
      if (!(*head)(int(status), heads.type, heads.length, heads.range)) stop = true;
      return !stop;
    }
  } st;
  st.h = handle();
  st.head = &head, st.sink = &sink;
  curl_slist* hs = nullptr;
  for (const std::string& x : headers) hs = curl_slist_append(hs, x.c_str());
  auto write = [](char* d, size_t s, size_t n, void* p) -> size_t {
    State* st = static_cast<State*>(p);
    if (!st->started && !st->begin()) return 0;
    return (*st->sink)(d, s * n) ? s * n : 0;
  };
  curl_easy_setopt(st.h, CURLOPT_URL, url.c_str());
  if (hs) curl_easy_setopt(st.h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(st.h, CURLOPT_USERAGENT, rules().player.userAgent.c_str());
  curl_easy_setopt(st.h, CURLOPT_ACCEPT_ENCODING, nullptr);  // media as it is: Content-Length must be the bytes'
  curl_easy_setopt(st.h, CURLOPT_WRITEFUNCTION, +write);
  curl_easy_setopt(st.h, CURLOPT_WRITEDATA, &st);
  curl_easy_setopt(st.h, CURLOPT_HEADERFUNCTION, onHeader);
  curl_easy_setopt(st.h, CURLOPT_HEADERDATA, &st.heads);
  CURLcode rc = curl_easy_perform(st.h);
  long status = 0;
  curl_easy_getinfo(st.h, CURLINFO_RESPONSE_CODE, &status);
  if (hs) curl_slist_free_all(hs);
  if (rc == CURLE_OK && !st.started) st.begin();  // no body (a 204, an error without one)
  if (rc != CURLE_OK && !st.started) return 0;
  return int(status);
}

}  // namespace rondo::direct
