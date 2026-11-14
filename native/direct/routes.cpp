// routes.h.
#include "routes.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mutex>

#include "innertube.h"
#include "tv_auth.h"
#include "json.h"
#include "lyrics.h"
#include "model.h"
#include "music.h"
#include "streams.h"
#include "video.h"

namespace rondo::direct {
namespace {

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

// The first page of a list or a page, kept two minutes (browse.ts's TtlCache): a screen opened again
// opens at once. Its `next` token stays good: tokens live 30 minutes.
struct Cached {
  std::string body;
  int64_t at;
};
std::mutex g_cacheMu;
std::map<std::string, Cached> g_cache;

// answers are the account's: another sign-in (or none) starts afresh
std::string accountKey(const std::string& k) { return std::to_string(accountGeneration()) + "|" + k; }
bool fromCache(const std::string& key, std::string& body) {
  std::string k = accountKey(key);
  std::lock_guard<std::mutex> l(g_cacheMu);
  auto it = g_cache.find(k);
  if (it == g_cache.end() || nowMs() - it->second.at > 120000) return false;
  body = it->second.body;
  return true;
}
void toCache(const std::string& key, const std::string& body) {
  std::string k = accountKey(key);
  std::lock_guard<std::mutex> l(g_cacheMu);
  if (g_cache.size() > 200) g_cache.clear();
  g_cache[k] = {body, nowMs()};
}

// one retry: a lone failure between the Y1 and YouTube is common (browse.ts's run())
std::string twice(const std::function<std::string()>& load) {
  std::string s = load();
  if (s.empty()) {
    usleep(300000);
    s = load();
  }
  return s;
}

void answer(Response& out, const std::string& body, const char* failure = "browse") {
  if (body.empty()) out.error(502, failure);
  else out.body = body;
}

// a list's first page (cached) or the page its `next` token points to
void list(const Request& r, Response& out, const std::string& scope, const std::function<std::string()>& first, bool cache = true) {
  auto next = r.query.find("next");
  if (next != r.query.end() && !next->second.empty()) {
    std::function<std::string()> load;
    if (!tokens().resume(scope, next->second, load)) return out.error(410, "expired");
    return answer(out, twice(load));
  }
  std::string body;
  if (cache && fromCache(r.path + "?" + (r.query.count("q") ? r.query.at("q") : "") + (r.query.count("type") ? r.query.at("type") : ""), body))
    return answer(out, body);
  body = twice(first);
  if (cache && !body.empty()) toCache(r.path + "?" + (r.query.count("q") ? r.query.at("q") : "") + (r.query.count("type") ? r.query.at("type") : ""), body);
  answer(out, body);
}

std::string param(const Request& r, const char* k) {
  auto it = r.query.find(k);
  return it == r.query.end() ? "" : it->second;
}

// lyrics.ts's field(): spaces squeezed, at most 200 bytes
std::string squeezeField(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (isspace((unsigned char)c)) {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += c;
    }
  }
  out = trim(out);
  if (out.size() > 200) out.resize(200);
  return out;
}

bool validPlaylist(const std::string& id) {
  if (id.size() < 2 || id.size() > 80) return false;
  for (char c : id)
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

// a few minutes' memory of a small answer from elsewhere (latest.json, the time zone)
struct Remembered {
  std::mutex mu;
  std::string body;
  int64_t until = 0;
};
std::string remember(Remembered& r, int64_t ms, const std::function<std::string()>& load) {
  {
    std::lock_guard<std::mutex> l(r.mu);
    if (!r.body.empty() && nowMs() < r.until) return r.body;
  }
  std::string b = load();
  std::lock_guard<std::mutex> l(r.mu);
  if (!b.empty()) r.body = b, r.until = nowMs() + ms;
  return b.empty() ? r.body : b;
}

// The time zone of this Y1's own public address (timezone.ts, asked from the Y1 itself): ipwho.is,
// then ipapi.co. Only the address goes to them, as it does with any request.
std::string timezone() {
  static Remembered r;
  return remember(r, 12 * 3600000, [] {
    Fetched a = get("https://ipwho.is/?fields=success,timezone", {"Accept: application/json"}, 65536);
    if (a.status == 200) {
      Doc d(yyjson_read(a.body.data(), a.body.size(), 0));
      std::string z = str(path(root(d), "timezone", "id"));
      if (!z.empty() && yyjson_is_true(key(root(d), "success"))) return "{\"timezone\":" + quote(z) + "}";
    }
    Fetched b = get("https://ipapi.co/timezone/", {}, 4096);
    std::string z = trim(b.body);
    bool name = b.status == 200 && !z.empty() && z.size() < 64 && z.find_first_of(" <>{}\"") == std::string::npos;
    return name ? "{\"timezone\":" + quote(z) + "}" : std::string();
  });
}

// The update page's latest.json (updates.ts): {"version", "file", "size", "firmware", "notes",
// "bundle", "bundleSize"}, files next to it
const char* kLatest = "https://jarvissax.github.io/rondo/latest.json";
std::string latest() {
  static Remembered r;
  return remember(r, 10 * 60000, [] {
    Fetched f = get(kLatest, {"Accept: application/json"}, 65536);
    return f.status == 200 ? f.body : std::string();
  });
}

void update(const Request& r, Response& out) {
  std::string body = latest();
  Doc d(yyjson_read(body.data(), body.size(), 0));
  J j = root(d);
  std::string version = str(key(j, "version"));
  if (version.empty()) return (void)(out.body = "{\"version\":null}");
  int firmware = int(num(key(j, "firmware"), 1));
  if (firmware < 1) firmware = 1;
  int asked = atoi(param(r, "firmware").c_str());
  std::string notes = str(key(j, "notes"));
  if (notes.empty()) notes = "https://github.com/jarvissax/rondo/releases/tag/v" + version;
  bool bundle = isStr(key(j, "bundle")) && key(j, "bundleSize");
  bool apk = isStr(key(j, "file")) && str(key(j, "file")).size() > 4 && str(key(j, "file")).compare(str(key(j, "file")).size() - 4, 4, ".apk") == 0;
  int64_t size = asked >= 2 ? (bundle ? num(key(j, "bundleSize")) : -1) : (apk ? num(key(j, "size")) : -1);
  auto reply = [&](int64_t s) {
    out.body = "{\"version\":" + quote(version) + ",\"size\":" + std::to_string(s) + ",\"notes\":" + quote(notes) + ",\"firmware\":" + std::to_string(firmware) + "}";
  };
  if (!asked && firmware > 1) return (void)(out.body = "{\"version\":null}");
  if (size >= 0) return reply(size);
  if (bundle && firmware > asked) return reply(0);  // a newer firmware's release: the Y1 says so
  out.body = "{\"version\":null}";
}

// the release's bundle (or APK), passed through as it downloads
void updateFile(const char* kind, Response& out) {
  std::string body = latest();
  Doc d(yyjson_read(body.data(), body.size(), 0));
  std::string file = str(key(root(d), kind));
  if (file.empty() || file.find('/') != std::string::npos) return out.error(404, "none");
  std::string url = std::string(kLatest).substr(0, std::string(kLatest).rfind('/') + 1) + file;
  int64_t size = num(key(root(d), !strcmp(kind, "bundle") ? "bundleSize" : "size"), -1);
  out.type = !strcmp(kind, "bundle") ? "application/octet-stream" : "application/vnd.android.package-archive";
  out.headers.push_back("Cache-Control: no-store");
  out.length = size;
  out.stream = [url](const Write& write) {
    bool fine = true;
    int status = getStreamed(
        url, {}, [&](int s, const std::string&, int64_t, const std::string&) { return fine = s == 200; },
        [&](const char* p, size_t n) { return write(p, n); });
    return fine && status == 200;
  };
}

}  // namespace

Request parseTarget(const std::string& method, const std::string& target) {
  Request r;
  r.method = method;
  size_t q = target.find('?');
  r.path = target.substr(0, q);
  if (q == std::string::npos) return r;
  std::string qs = target.substr(q + 1);
  size_t a = 0;
  while (a <= qs.size()) {
    size_t b = qs.find('&', a);
    std::string pair = qs.substr(a, b == std::string::npos ? std::string::npos : b - a);
    size_t eq = pair.find('=');
    if (!pair.empty()) r.query[uriDecode(pair.substr(0, eq))] = eq == std::string::npos ? "" : uriDecode(pair.substr(eq + 1));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return r;
}

void route(const Request& r, Response& out) {
  const std::string& p = r.path;
  if (p == "/healthz") {
    // what the screens read: up, and whether the account's tabs show (signed in with cookies)
    int ok = lastResolveOk();
    const char* in = signedIn() || tv::signedIn() ? "true" : "false";  // cookies.txt, or the code sign-in
    out.body = std::string("{\"ok\":true,\"ytdlp\":\"direct\",\"lastResolveOk\":") + (ok < 0 ? "null" : ok ? "true" : "false") +
               ",\"cookiesOk\":" + (hasCookies() ? "true" : "false") + ",\"account\":{\"music\":" + in + ",\"videos\":" + in + "}}";
    return;
  }
  // the code sign-in (tv_auth.h): its state, a new code, cancel, sign out
  if (p == "/tv") return void(out.body = tv::stateJson());
  if (p == "/tv/begin") return void(out.body = tv::begin());
  if (p == "/tv/cancel") return tv::cancel(), void(out.body = tv::stateJson());
  if (p == "/tv/signout") return tv::signOut(), void(out.body = tv::stateJson());
  if (p == "/account") {  // who the cookies sign in as (the dashboard, Settings › YouTube)
    if (!param(r, "reload").empty()) reloadAccount();
    out.body = accountJson();
    return;
  }
  if (p == "/time") {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    out.body = "{\"epochMs\":" + std::to_string(int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000) + "}";
    return;
  }
  // --- YouTube Music ---
  if (p == "/music/home") return list(r, out, "music/home", [] { return music::home(); });
  if (p == "/music/search") {
    std::string q = trim(param(r, "q")), type = param(r, "type").empty() ? "song" : param(r, "type");
    if (param(r, "next").empty() && q.empty()) return out.error(400, "query");
    return list(r, out, "music/search", [q, type] { return music::search(q, type); }, false);
  }
  if (p == "/music/library") {  // cookies.txt's (YouTube Music's own), else the code sign-in's
    if (signedIn()) return list(r, out, "music/library", [] { return music::library(); });
    if (tv::signedIn()) return list(r, out, "music/library", [] { return video::tvLibrary(); });
    return out.error(503, "no_cookies");
  }
  if (starts(p, "/music/playlist/")) {
    std::string id = uriDecode(p.substr(16));
    if (starts(id, "VL") && id.size() > 3) id = id.substr(2);
    if (!validPlaylist(id)) return out.error(400, "id");
    // Liked Music and private playlists: YouTube Music's own with cookies.txt, else the TV app's with the code sign-in
    return list(r, out, "music/playlist/" + id, [id] {
      bool tvOnly = tv::signedIn() && !signedIn();
      std::string page = tvOnly && id == "LM" ? "" : music::playlist(id);
      return page.empty() && tvOnly ? video::tvPlaylist(id) : page;
    });
  }
  if (starts(p, "/music/album/")) {
    std::string id = uriDecode(p.substr(13));
    if (!starts(id, "MPRE")) return out.error(400, "id");
    return list(r, out, "music/album/" + id, [id] { return music::album(id); });
  }
  if (starts(p, "/music/artist/")) {
    std::string id = uriDecode(p.substr(14));
    if (!starts(id, "UC") || id.size() != 24) return out.error(400, "id");
    return list(r, out, "music/artist/" + id, [id] { return music::artist(id); });
  }
  if (starts(p, "/music/song/")) {
    std::string id = uriDecode(p.substr(12));
    if (!isVideoId(id)) return out.error(400, "id");
    return list(r, out, "music/song/" + id, [id] { return music::song(id); });
  }
  if (starts(p, "/music/next/")) {
    std::string id = uriDecode(p.substr(12));
    if (!isVideoId(id)) return out.error(400, "id");
    return list(r, out, "music/next/" + id, [id] { return music::next(id); });
  }
  // --- YouTube ---
  if (p == "/video/home")  // signed in: what YouTube picks for the account; else the TV app's topics
    return list(r, out, "video/home", [] {
      std::string picks = tv::signedIn() ? video::tvFeed("video/home", "FEwhat_to_watch")
                          : signedIn()   ? video::feed("video/home", "FEwhat_to_watch")
                                         : "";
      return picks.empty() || picks.find("\"id\"") == std::string::npos ? video::home() : picks;
    });
  if (p == "/video/subscriptions" || p == "/video/history") {  // the code sign-in's, else cookies.txt's
    std::string id = p == "/video/history" ? "FEhistory" : "FEsubscriptions";
    if (tv::signedIn()) return list(r, out, p.substr(1), [p, id] { return video::tvFeed(p.substr(1), id); });
    if (!signedIn()) return out.error(503, "no_cookies");
    return list(r, out, p.substr(1), [p, id] { return video::feed(p.substr(1), id); });
  }
  if (p == "/video/search") {
    std::string q = trim(param(r, "q"));
    if (param(r, "next").empty() && q.empty()) return out.error(400, "query");
    return list(r, out, "video/search", [q] { return video::search(q); }, false);
  }
  if (p == "/video/shorts") return list(r, out, "video/shorts", [] { return video::shorts(); });
  if (starts(p, "/video/short/")) {
    std::string id = uriDecode(p.substr(13));
    if (!isVideoId(id)) return out.error(400, "id");
    return list(r, out, "video/short/" + id, [id] { return video::shortInfo(id); });
  }
  // --- streams, artwork, lyrics ---
  if (starts(p, "/s/")) return streamRoute(r, out);
  if (starts(p, "/mpd/")) return mpdRoute(r, out);
  if (starts(p, "/resolve/")) return resolveRoute(r, out);
  if (starts(p, "/captions/")) return captionsRoute(r, out);
  if (p == "/thumb") return thumbRoute(r, out);
  if (p == "/lyrics") {
    std::string title = squeezeField(param(r, "title"));
    if (title.empty()) return out.error(400, "title");
    std::string id = param(r, "id");
    if (!id.empty() && !isVideoId(id)) return out.error(400, "id");
    int duration = atoi(param(r, "duration").c_str());
    out.body = lyricsJson(title, squeezeField(param(r, "artist")), squeezeField(param(r, "album")), duration > 0 && duration <= 86400 ? duration : 0, id);
    return;
  }
  // --- the rest of a Rondo server's API, as far as it goes without one ---
  if (p == "/timezone") {
    std::string tz = timezone();
    out.body = tz.empty() ? "{\"timezone\":null}" : tz;
    return;
  }
  if (p == "/update") return update(r, out);
  if (p == "/update/rondo.rondo") return updateFile("bundle", out);
  if (p == "/update/rondo.apk") return updateFile("file", out);
  // Settings › Sound › Headphone match (AutoEq) lives on the server
  if (starts(p, "/eq/")) return out.error(503, "needs_server");
  out.error(404, "not_found");
}

}  // namespace rondo::direct
