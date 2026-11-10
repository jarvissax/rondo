// innertube.h.
#include "innertube.h"

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
