// rss.h: podcasts from RSS feeds, from the podcast hosts, Apple's directory and Podcast Index.
#include "rss.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#include <algorithm>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "../core/rss_ids.h"
#ifdef __ANDROID__
#include "../core/net.h"
#else
// the Mac's dcli (tools/native/direct-host.sh): core/net.cpp is the Y1's (as innertube.cpp does)
#include "../core/version.h"
namespace rondo::net {
inline bool retryable(CURLcode rc) { return rc == CURLE_COULDNT_CONNECT || rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR; }
inline const char* userAgent() {
  static const std::string ua = std::string("Rondo/") + kRondoVersion + " (Innioasis Y1)";
  return ua.c_str();
}
}  // namespace rondo::net
#endif
#include "innertube.h"
#include "json.h"
#include "rss_parse.h"

namespace rondo::direct::rss {
namespace {

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

int64_t wallMs() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
int64_t monoMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

std::string param(const Request& r, const char* k) {
  auto it = r.query.find(k);
  return it == r.query.end() ? "" : it->second;
}

std::mutex g_cfgMu;
std::string g_server, g_token;
int64_t g_noFeedRouteUntil = 0, g_noIndexUntil = 0;  // monoMs(): the server has no such route, no key

bool serverFor(std::string& base, std::string& token) {
  std::lock_guard<std::mutex> l(g_cfgMu);
  base = g_server, token = g_token;
  return !base.empty() && !token.empty();
}

// ---- HTTP: the podcast hosts, Apple and Podcast Index (never with the Rondo server's token) ----

struct Got {
  int status = 0;
  std::string body, type, etag, lastModified, effective, range;
  int64_t length = -1;
  CURLcode rc = CURLE_OK;
};

size_t onHead(char* d, size_t s, size_t n, void* p) {
  Got* g = static_cast<Got*>(p);
  size_t len = s * n;
  std::string line(d, len);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  auto value = [&](const char* name) -> const char* {
    size_t l = strlen(name);
    return line.size() > l && strncasecmp(line.c_str(), name, l) == 0 ? line.c_str() + l : nullptr;
  };
  if (line.compare(0, 5, "HTTP/") == 0) {  // a redirect's own headers don't count
    g->type.clear(), g->etag.clear(), g->lastModified.clear(), g->range.clear(), g->length = -1;
  } else if (const char* v = value("content-type:")) {
    g->type = trim(v);
  } else if (const char* v = value("etag:")) {
    g->etag = trim(v);
  } else if (const char* v = value("last-modified:")) {
    g->lastModified = trim(v);
  } else if (const char* v = value("content-range:")) {
    g->range = trim(v);
  } else if (const char* v = value("content-length:")) {
    g->length = atoll(v);
  }
  return len;
}

CURL* handle(bool compressed) {
  static thread_local CURL* c = nullptr;
  if (!c) c = curl_easy_init();
  else curl_easy_reset(c);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  // 20 s without a byte: given up
  curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);  // podcast links go through a tracker or two first
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(c, CURLOPT_USERAGENT, net::userAgent());
  if (compressed) curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
#ifdef __ANDROID__
  net::trustPodcastHosts(c);  // Android 4.2's roots, the bundled ones and Apple's (native/assets/certs)
#endif
  return c;
}

// as innertube.cpp's perform(): a refused connect or a reset tried again at once, up to 20 times; not
// once a byte has come (a transfer stopped on purpose ends as a receive error on HTTP/2)
CURLcode perform(CURL* h, const std::function<void()>& again) {
  CURLcode rc = curl_easy_perform(h);
  for (int tries = 1; tries < 20 && net::retryable(rc); tries++) {
    curl_off_t got = 0;
    curl_easy_getinfo(h, CURLINFO_SIZE_DOWNLOAD_T, &got);
    if (got > 0) break;
    if (again) again();
    usleep(250000);
    rc = curl_easy_perform(h);
  }
  return rc;
}

struct Sink {
  std::string* body;
  size_t max;
};
size_t collect(char* d, size_t s, size_t n, void* p) {
  Sink* k = static_cast<Sink*>(p);
  size_t len = s * n;
  if (k->body->size() + len > k->max) return 0;
  k->body->append(d, len);
  return len;
}

// compressed: feeds and JSON come gzipped; pictures not (one picture host sent what curl couldn't
// unpack when asked, and the fetch failed)
Got fetch(const std::string& url, const std::vector<std::string>& headers = {}, size_t maxBytes = 2 << 20, long timeoutMs = 25000,
          bool compressed = true) {
  Got g;
  CURL* h = handle(compressed);
  curl_slist* hs = nullptr;
  for (const std::string& x : headers) hs = curl_slist_append(hs, x.c_str());
  Sink k{&g.body, maxBytes};
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  if (hs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, timeoutMs);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &k);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHead);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &g);
  g.rc = perform(h, [&] { g.body.clear(); });
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  char* eff = nullptr;
  curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
  if (eff) g.effective = eff;
  if (hs) curl_slist_free_all(hs);
  g.status = g.rc == CURLE_OK ? int(status) : 0;
  return g;
}

using Doc = std::unique_ptr<yyjson_doc, void (*)(yyjson_doc*)>;
Doc readJson(const std::string& s) { return Doc(yyjson_read(s.data(), s.size(), 0), yyjson_doc_free); }
std::string sval(yyjson_val* o, const char* k) { return str(key(o, k)); }

// ---- the directory: Apple, Podcast Index ----

struct Show {
  std::string feed, title, author, art, genre;
  int64_t apple = 0, count = 0, updated = 0;
};

std::string showJson(const Show& s) {
  return "{\"id\":" + quote(rssid::showId(s.feed)) + ",\"feed\":" + quote(s.feed) + ",\"title\":" + quote(s.title) + ",\"author\":" + quote(s.author) +
         ",\"art\":" + quote(s.art) + ",\"genre\":" + quote(s.genre) + ",\"count\":" + std::to_string(s.count) +
         ",\"updated\":" + std::to_string(s.updated) + ",\"apple\":" + std::to_string(s.apple) + "}";
}

std::string country(const Request& r) {
  std::string c = param(r, "country");
  for (char& ch : c) ch = char(tolower((unsigned char)ch));
  bool ok = c.size() == 2 && isalpha((unsigned char)c[0]) && isalpha((unsigned char)c[1]);
  return ok ? c : "us";
}

// Apple's own answers' pictures can be had at any size: the address names it ("…/600x600bb.jpg")
std::string appleArt(yyjson_val* v) {
  std::string a = sval(v, "artworkUrl600");
  if (a.empty()) a = sval(v, "artworkUrl100");
  return a;
}

void appleShows(yyjson_val* results, std::vector<Show>& out) {
  each(results, [&](yyjson_val* v) {
    Show s;
    s.feed = sval(v, "feedUrl");
    if (s.feed.empty()) return;  // Apple's own shows (subscriptions): no feed to read
    s.title = sval(v, "collectionName"), s.author = sval(v, "artistName"), s.art = appleArt(v), s.genre = sval(v, "primaryGenreName");
    s.apple = num(key(v, "collectionId")), s.count = num(key(v, "trackCount"));
    s.updated = rss::parseDate(sval(v, "releaseDate"));
    out.push_back(std::move(s));
  });
}

std::mutex g_dirMu;
struct Kept {
  std::string body;
  int64_t at;
};
std::map<std::string, Kept> g_dir;  // search and chart answers, kept a while (Apple allows ~20 a minute)
bool dirCached(const std::string& k, int64_t ttl, std::string& body) {
  std::lock_guard<std::mutex> l(g_dirMu);
  auto it = g_dir.find(k);
  if (it == g_dir.end() || monoMs() - it->second.at > ttl) return false;
  body = it->second.body;
  return true;
}
void dirKeep(const std::string& k, const std::string& body) {
  std::lock_guard<std::mutex> l(g_dirMu);
  if (g_dir.size() > 60) g_dir.clear();
  g_dir[k] = {body, monoMs()};
}

int appleSearch(const std::string& q, const std::string& cc, std::vector<Show>& out) {
  Got g = fetch("https://itunes.apple.com/search?media=podcast&entity=podcast&limit=25&country=" + cc + "&term=" + uriComponent(q));
  if (g.status != 200) return g.status ? g.status : 502;
  Doc d = readJson(g.body);
  if (!d) return 502;
  appleShows(key(yyjson_doc_get_root(d.get()), "results"), out);
  return 200;
}

// the Y1's own key (the dashboard's, when it has no server): <data>/podcastindex.json
std::string keyPath() {
  std::string dir = dataDir();
  return dir.empty() ? "" : dir + "/podcastindex.json";
}
bool ownKey(std::string& k, std::string& secret) {
  std::string p = keyPath();
  if (p.empty()) return false;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  std::string s;
  char b[1024];
  size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0 && s.size() < 8192) s.append(b, n);
  fclose(f);
  Doc d = readJson(s);
  if (!d) return false;
  k = sval(yyjson_doc_get_root(d.get()), "key"), secret = sval(yyjson_doc_get_root(d.get()), "secret");
  return !k.empty() && !secret.empty();
}

std::string sha1hex(const std::string& s) {
  unsigned char md[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), md);
  static const char* hx = "0123456789abcdef";
  std::string o;
  for (unsigned char c : md) o += hx[c >> 4], o += hx[c & 15];
  return o;
}

void indexShows(yyjson_val* list, std::vector<Show>& out) {
  each(list, [&](yyjson_val* v) {
    if (num(key(v, "dead")) == 1) return;
    Show s;
    s.feed = sval(v, "feed");
    if (s.feed.empty()) s.feed = sval(v, "url");
    if (s.feed.empty()) return;
    s.title = sval(v, "title"), s.author = sval(v, "author");
    s.art = sval(v, "art");
    if (s.art.empty()) s.art = sval(v, "artwork");
    if (s.art.empty()) s.art = sval(v, "image");
    s.apple = num(key(v, "itunesId")), s.count = num(key(v, "count"), num(key(v, "episodeCount")));
    s.updated = num(key(v, "newestItemPublishTime"));
    out.push_back(std::move(s));
  });
}

// Podcast Index: through the Rondo server when it has a key, else with the Y1's own; "" none tried
std::string indexSearch(const std::string& q, std::vector<Show>& out) {
  std::string base, token;
  bool server = serverFor(base, token);
  if (server && monoMs() >= g_noIndexUntil) {
    Got g = fetch(base + "/podcasts/index/search?max=20&q=" + uriComponent(q), {"Authorization: Bearer " + token});
    if (g.status == 200) {
      if (Doc d = readJson(g.body)) indexShows(key(yyjson_doc_get_root(d.get()), "items"), out);
      return "server";
    }
    // a server before podcasts (404) or without a key (503): not asked again for a while
    if (g.status == 404 || g.status == 503) g_noIndexUntil = monoMs() + (g.status == 404 ? 3600000 : 600000);
  }
  std::string k, secret;
  if (!ownKey(k, secret)) return "";
  std::string date = std::to_string(time(nullptr));
  Got g = fetch("https://api.podcastindex.org/api/1.0/search/byterm?max=20&q=" + uriComponent(q),
                {"X-Auth-Key: " + k, "X-Auth-Date: " + date, "Authorization: " + sha1hex(k + secret + date)});
  if (g.status != 200) return "";
  if (Doc d = readJson(g.body)) indexShows(key(yyjson_doc_get_root(d.get()), "feeds"), out);
  return "own";
}

void searchRoute(const Request& r, Response& out) {
  std::string q = trim(param(r, "q")), cc = country(r);
  if (q.empty()) return out.error(400, "query");
  std::string k = "search|" + cc + "|" + q, body;
  if (dirCached(k, 600000, body)) return void(out.body = body);
  std::vector<Show> apple, index;
  int a = appleSearch(q, cc, apple);
  std::string via = indexSearch(q, index);
  if (a != 200 && via.empty()) return out.error(a == 403 || a == 429 ? 429 : 502, a == 403 || a == 429 ? "busy" : "upstream");
  // Apple's first (its order is a good one), then the shows only Podcast Index has
  std::set<std::string> seen;
  std::set<int64_t> apples;
  std::string items;
  int n = 0;
  auto add = [&](const Show& s) {
    std::string id = rssid::showId(s.feed);
    if (seen.count(id) || (s.apple && apples.count(s.apple))) return;
    seen.insert(id);
    if (s.apple) apples.insert(s.apple);
    items += (n++ ? "," : "") + showJson(s);
  };
  for (const Show& s : apple) add(s);
  for (const Show& s : index) add(s);
  body = "{\"items\":[" + items + "],\"apple\":" + (a == 200 ? "true" : "false") + ",\"index\":" + quote(via) + "}";
  dirKeep(k, body);
  out.body = body;
}

// Apple's charts: the top 50 of a country (in a genre), their feeds from a lookup of all 50 at once
void topRoute(const Request& r, Response& out) {
  std::string cc = country(r), genre = param(r, "genre");
  if (!genre.empty() && genre.find_first_not_of("0123456789") != std::string::npos) return out.error(400, "genre");
  std::string k = "top|" + cc + "|" + genre, body;
  if (dirCached(k, 6 * 3600000LL, body)) return void(out.body = body);
  Got g = fetch("https://itunes.apple.com/" + cc + "/rss/toppodcasts/limit=50" + (genre.empty() ? "" : "/genre=" + genre) + "/json");
  if (g.status != 200) return out.error(502, "upstream");
  Doc d = readJson(g.body);
  if (!d) return out.error(502, "upstream");
  std::vector<int64_t> ids;
  std::map<int64_t, Show> named;
  yyjson_val* entries = path(yyjson_doc_get_root(d.get()), "feed", "entry");
  auto take = [&](yyjson_val* e) {
    int64_t id = num(path(e, "id", "attributes", "im:id"));
    if (!id) return;
    ids.push_back(id);
    Show s;
    s.title = str(path(e, "im:name", "label")), s.author = str(path(e, "im:artist", "label"));
    s.genre = str(path(e, "category", "attributes", "label"));
    named[id] = s;
  };
  if (yyjson_is_arr(entries)) each(entries, take);
  else if (yyjson_is_obj(entries)) take(entries);
  if (ids.empty()) return void(out.body = "{\"items\":[]}");
  std::string list;
  for (size_t i = 0; i < ids.size(); i++) list += (i ? "," : "") + std::to_string(ids[i]);
  Got l = fetch("https://itunes.apple.com/lookup?entity=podcast&country=" + cc + "&id=" + list);
  if (l.status != 200) return out.error(502, "upstream");
  Doc ld = readJson(l.body);
  if (!ld) return out.error(502, "upstream");
  std::vector<Show> found;
  appleShows(key(yyjson_doc_get_root(ld.get()), "results"), found);
  std::map<int64_t, Show> byId;
  for (Show& s : found) byId[s.apple] = s;
  std::string items;
  int n = 0;
  for (int64_t id : ids) {
    auto it = byId.find(id);
    if (it == byId.end()) continue;  // no feed (Apple's own)
    Show s = it->second;
    if (s.genre.empty()) s.genre = named[id].genre;
    items += (n++ ? "," : "") + showJson(s);
  }
  body = "{\"items\":[" + items + "]}";
  dirKeep(k, body);
  out.body = body;
}

// ---- feeds ----

std::string cacheDir() {
  std::string d = dataDir();
  if (d.empty()) return "";
  d += "/rss";
  mkdir(d.c_str(), 0700);
  return d;
}

bool readFile(const std::string& p, std::string& out, size_t max = 64 << 20) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char b[65536];
  size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0 && out.size() < max) out.append(b, n);
  fclose(f);
  return true;
}
bool writeFile(const std::string& p, const std::string& s) {
  std::string tmp = p + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(s.data(), 1, s.size(), f) == s.size() && fflush(f) == 0;
  fclose(f);
  return ok && rename(tmp.c_str(), p.c_str()) == 0;
}

struct Feed {
  std::string id, url;  // url: the address it's read from now (a feed that moved: its new one)
  rss::Channel ch;
  std::vector<rss::Episode> eps;  // without their notes (kept apart: <id>.notes.json)
  std::string etag, lastModified;
  int64_t checked = 0;  // wallMs() the host (or the server) last answered
  int64_t bytes = 0;    // the feed's size the last time it was read whole
  bool viaServer = false;
};

void episodeJson(std::string& o, const std::string& showId, const rss::Episode& e, bool url = true) {
  o += "{\"id\":" + quote(rssid::episodeId(showId, e.guid)) + ",\"guid\":" + quote(e.guid) + ",\"title\":" + quote(e.title) +
       ",\"date\":" + std::to_string(e.date) + ",\"duration\":" + std::to_string(e.duration);
  if (url) o += ",\"url\":" + quote(e.url);
  o += ",\"type\":" + quote(e.type) + ",\"size\":" + std::to_string(e.size);
  if (!e.image.empty()) o += ",\"image\":" + quote(e.image);
  if (!e.chapters.empty()) o += ",\"chapters\":" + quote(e.chapters);
  if (e.season) o += ",\"season\":" + std::to_string(e.season);
  if (e.number) o += ",\"episode\":" + std::to_string(e.number);
  if (e.episodeType != "full" && !e.episodeType.empty()) o += ",\"episodeType\":" + quote(e.episodeType);
  o += "}";
}

// the answer (and the file kept): the channel, then the episodes (since: only those after it; urls
// false: without their links, which the file route finds by the episode's id: a long list stays small)
std::string feedJson(const Feed& f, int64_t since, bool meta, bool urls = true) {
  const rss::Channel& c = f.ch;
  std::string o = "{\"id\":" + quote(f.id) + ",\"feed\":" + quote(f.url) + ",\"title\":" + quote(c.title) + ",\"author\":" + quote(c.author) +
                  ",\"description\":" + quote(c.description) + ",\"art\":" + quote(c.art) + ",\"link\":" + quote(c.link) +
                  ",\"language\":" + quote(c.language) + ",\"serial\":" + (c.serial ? "true" : "false") +
                  ",\"explicit\":" + (c.explicitContent ? "true" : "false") + ",\"complete\":" + (c.complete ? "true" : "false") +
                  ",\"count\":" + std::to_string(f.eps.size()) + ",\"checked\":" + std::to_string(f.checked);
  if (meta)
    o += ",\"etag\":" + quote(f.etag) + ",\"lastModified\":" + quote(f.lastModified) + ",\"bytes\":" + std::to_string(f.bytes) +
         ",\"viaServer\":" + (f.viaServer ? "true" : "false") + ",\"newFeedUrl\":" + quote(c.newFeedUrl);
  o += ",\"episodes\":[";
  o.reserve(o.size() + f.eps.size() * 300);
  bool first = true;
  for (const rss::Episode& e : f.eps) {
    if (since > 0 && e.date <= since) continue;
    if (!first) o += ",";
    first = false;
    episodeJson(o, f.id, e, urls || meta);
  }
  return o + "]}";
}

rss::Episode episodeFrom(yyjson_val* v) {
  rss::Episode e;
  e.guid = sval(v, "guid"), e.title = sval(v, "title"), e.url = sval(v, "url"), e.type = sval(v, "type");
  e.image = sval(v, "image"), e.chapters = sval(v, "chapters"), e.notes = sval(v, "notes");
  std::string t = sval(v, "episodeType");
  if (!t.empty()) e.episodeType = t;
  e.date = num(key(v, "date")), e.size = num(key(v, "size")), e.duration = int(num(key(v, "duration")));
  e.season = int(num(key(v, "season"))), e.number = int(num(key(v, "episode")));
  return e;
}
void channelFrom(yyjson_val* r, rss::Channel& c) {
  c.title = sval(r, "title"), c.author = sval(r, "author"), c.description = sval(r, "description"), c.art = sval(r, "art");
  c.link = sval(r, "link"), c.language = sval(r, "language"), c.newFeedUrl = sval(r, "newFeedUrl");
  c.serial = yyjson_get_bool(key(r, "serial")), c.explicitContent = yyjson_get_bool(key(r, "explicit")), c.complete = yyjson_get_bool(key(r, "complete"));
}

std::shared_ptr<Feed> loadFeed(const std::string& id) {
  std::string dir = cacheDir(), s;
  if (dir.empty() || !readFile(dir + "/" + id + ".json", s)) return nullptr;
  Doc d = readJson(s);
  if (!d) return nullptr;
  yyjson_val* r = yyjson_doc_get_root(d.get());
  auto f = std::make_shared<Feed>();
  f->id = id, f->url = sval(r, "feed");
  channelFrom(r, f->ch);
  f->etag = sval(r, "etag"), f->lastModified = sval(r, "lastModified");
  f->checked = num(key(r, "checked")), f->bytes = num(key(r, "bytes"));
  f->viaServer = yyjson_get_bool(key(r, "viaServer"));
  each(key(r, "episodes"), [&](yyjson_val* v) { f->eps.push_back(episodeFrom(v)); });
  return f;
}

void saveNotes(const std::string& id, const std::map<std::string, std::string>& notes) {
  std::string dir = cacheDir();
  if (dir.empty()) return;
  std::string o = "{";
  int n = 0;
  for (auto& [guid, text] : notes)
    if (!text.empty()) o += (n++ ? "," : "") + quote(guid) + ":" + quote(text);
  writeFile(dir + "/" + id + ".notes.json", o + "}");
}

// The feeds read lately, in memory (the Podcasts screens ask for the same few again and again)
std::mutex g_feedMu;
std::condition_variable g_feedCv;
std::list<std::shared_ptr<Feed>> g_recent;  // the most recent first, 6 at most
std::set<std::string> g_reading;            // being read now
std::map<std::string, std::string> g_failed;  // the last read's failure ("" none), by id
int g_readers = 0;                             // reads running (two at most at once: the Y1 is small)

std::shared_ptr<Feed> recent(const std::string& id) {
  for (auto it = g_recent.begin(); it != g_recent.end(); ++it)
    if ((*it)->id == id) {
      auto f = *it;
      g_recent.erase(it);
      g_recent.push_front(f);
      return f;
    }
  return nullptr;
}
void remember(const std::shared_ptr<Feed>& f) {
  for (auto it = g_recent.begin(); it != g_recent.end(); ++it)
    if ((*it)->id == f->id) {
      g_recent.erase(it);
      break;
    }
  g_recent.push_front(f);
  while (g_recent.size() > 6) g_recent.pop_back();
}

constexpr int64_t kStaleMs = 30 * 60000;    // a feed read is good for half an hour
constexpr int64_t kBigFeed = 2 << 20;       // bigger than this: the server reads it, when it can
constexpr size_t kMaxFeed = 60u << 20;      // a feed this size is refused
constexpr size_t kMaxEpisodes = 5000;

// the feed whole from the Rondo server, its pages of 500 (since: only what's after it, for a feed
// kept here). false: it couldn't (an older server, the host refused it there): read it here instead
bool fromServer(const std::string& url, int64_t since, Feed& f, std::map<std::string, std::string>& notes, std::string& why) {
  std::string base, token;
  if (!serverFor(base, token) || monoMs() < g_noFeedRouteUntil) return false;
  for (int64_t offset = 0;;) {
    std::string q = base + "/podcasts/feed?limit=500&offset=" + std::to_string(offset) + "&url=" + uriComponent(url);
    if (since > 0) q += "&since=" + std::to_string(since);
    Got g = fetch(q, {"Authorization: Bearer " + token}, 48u << 20, 90000);
    if (g.status == 404) g_noFeedRouteUntil = monoMs() + 3600000;  // an older server (the route never answers 404 itself)
    if (g.status != 200) {
      why = g.status ? "upstream" : "offline";
      return false;
    }
    Doc d = readJson(g.body);
    if (!d) return false;
    yyjson_val* r = yyjson_doc_get_root(d.get());
    if (offset == 0) {
      channelFrom(r, f.ch);
      std::string moved = sval(r, "feed");
      if (!moved.empty()) f.url = moved;
    }
    each(key(r, "episodes"), [&](yyjson_val* v) {
      rss::Episode e = episodeFrom(v);
      if (e.guid.empty() || f.eps.size() >= kMaxEpisodes) return;
      if (!e.notes.empty()) notes[e.guid] = std::move(e.notes), e.notes.clear();
      f.eps.push_back(std::move(e));
    });
    yyjson_val* next = key(r, "next");
    if (!yyjson_is_num(next) || num(next) <= offset) {
      f.bytes = since > 0 ? f.bytes : num(key(r, "count")) * 1500;  // about its size, for next time
      return true;
    }
    offset = num(next);
  }
}

// the feed read here, as it comes (a 10 MB feed is never whole in memory). 304: unchanged since `old`
int fromHost(const std::string& url, const Feed* old, Feed& f, std::map<std::string, std::string>& notes, std::string& why) {
  CURL* h = handle(true);
  std::vector<std::string> hdrs = {"Accept: application/rss+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.5"};
  if (old && !old->etag.empty()) hdrs.push_back("If-None-Match: " + old->etag);
  if (old && !old->lastModified.empty()) hdrs.push_back("If-Modified-Since: " + old->lastModified);
  curl_slist* hs = nullptr;
  for (const std::string& x : hdrs) hs = curl_slist_append(hs, x.c_str());
  std::set<std::string> guids;
  struct Read {
    std::unique_ptr<rss::Parser> parser;
    CURL* h;
    size_t bytes = 0;
    bool notFeed = false, tooBig = false;
    long status = 0;
  } rd;
  auto make = [&] {
    rd.parser.reset(new rss::Parser([&](rss::Episode&& e) {
      if (e.guid.empty() || f.eps.size() >= kMaxEpisodes || !guids.insert(e.guid).second) return;  // a guid twice: the first
      if (!e.notes.empty()) notes[e.guid] = std::move(e.notes);
      e.notes.clear();
      f.eps.push_back(std::move(e));
    }));
    rd.bytes = 0, rd.notFeed = rd.tooBig = false;
    f.eps.clear(), notes.clear(), guids.clear();
  };
  make();
  rd.h = h;
  Got g;
  auto write = [](char* d, size_t s, size_t n, void* p) -> size_t {
    Read* r = static_cast<Read*>(p);
    if (!r->status) curl_easy_getinfo(r->h, CURLINFO_RESPONSE_CODE, &r->status);
    size_t len = s * n;
    if (r->status != 200) return len;  // an error page: read past
    r->bytes += len;
    if (r->bytes > kMaxFeed) return r->tooBig = true, 0;
    if (!r->parser->feed(d, len)) return r->notFeed = true, 0;
    return len;
  };
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 180000L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +write);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &rd);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHead);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &g);
  CURLcode rc = perform(h, [&] {
    make();
    rd.status = 0;
  });
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  char* eff = nullptr;
  curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
  curl_slist_free_all(hs);
  if (rd.tooBig) return why = "too_big", 413;
  if (rd.notFeed) return why = "not_feed", 422;
  if (rc != CURLE_OK) return why = "offline", 0;
  if (status == 304) return 304;
  if (status != 200) return why = "upstream", int(status);
  if (!rd.parser->finish(f.ch)) return why = "not_feed", 422;
  f.etag = g.etag, f.lastModified = g.lastModified, f.bytes = int64_t(rd.bytes);
  // moved for good (a 301, a 308, or the feed's own itunes:new-feed-url): read from there next time
  long redirects = 0;
  curl_easy_getinfo(h, CURLINFO_REDIRECT_COUNT, &redirects);
  f.url = url;
  if (!f.ch.newFeedUrl.empty() && starts(f.ch.newFeedUrl, "http")) f.url = f.ch.newFeedUrl;
  (void)eff;
  (void)redirects;
  return 200;
}

// The feed read again (from the host, or the server for a big public one), kept, answered to whoever
// waits. A changed big feed kept here asks the server only for what's new since its newest episode.
void readFeed(std::string id, std::string url, bool priv) {
  std::shared_ptr<Feed> old;
  {
    std::unique_lock<std::mutex> l(g_feedMu);
    old = recent(id);
    g_feedCv.wait(l, [] { return g_readers < 2; });
    g_readers++;
  }
  if (!old) old = loadFeed(id);
  auto f = std::make_shared<Feed>();
  f->id = id, f->url = old && !old->url.empty() ? old->url : url;
  std::map<std::string, std::string> notes;
  std::string why;
  int status = 0;
  bool merged = false;
  bool big = old && (old->bytes > kBigFeed || old->viaServer);
  if (!priv && big) {
    int64_t newest = 0;
    for (const rss::Episode& e : old->eps) newest = std::max(newest, e.date);
    // what's new since the newest kept, put before them; a feed whose episodes changed otherwise
    // (count not adding up) read whole
    Feed delta;
    delta.url = f->url;
    if (newest > 0 && fromServer(f->url, newest, delta, notes, why)) {
      std::set<std::string> have;
      for (const rss::Episode& e : old->eps) have.insert(e.guid);
      std::vector<rss::Episode> fresh;
      for (rss::Episode& e : delta.eps)
        if (!have.count(e.guid)) fresh.push_back(std::move(e));
      *f = *old;
      f->ch = delta.ch;
      f->eps.insert(f->eps.begin(), fresh.begin(), fresh.end());
      f->viaServer = true;
      merged = true;
      status = 200;
    } else if (fromServer(f->url, 0, *f, notes, why)) {
      f->viaServer = true;
      status = 200;
    }
  }
  if (!status) {
    Feed fresh;
    fresh.id = id;
    status = fromHost(f->url, old.get(), fresh, notes, why);
    if (status == 304 && old) {
      *f = *old;
    } else if (status == 200) {
      fresh.viaServer = false;
      *f = std::move(fresh);
    }
  }
  if (status == 200 || status == 304) {
    f->id = id;
    f->checked = wallMs();
    if (std::string dir = cacheDir(); !dir.empty()) {
      writeFile(dir + "/" + id + ".json", feedJson(*f, 0, true));
      if (status == 200) {
        if (merged) {  // the notes kept before, and the new episodes'
          std::string had;
          if (readFile(dir + "/" + id + ".notes.json", had))
            if (Doc d = readJson(had)) {
              size_t i, n;
              yyjson_val *k, *v;
              yyjson_obj_foreach(yyjson_doc_get_root(d.get()), i, n, k, v) notes.emplace(str(k), str(v));
            }
        }
        saveNotes(id, notes);
      }
    }
  } else {
    fprintf(stderr, "rondo-rss: %s: %s (%d)\n", id.c_str(), why.c_str(), status);
  }
  std::lock_guard<std::mutex> l(g_feedMu);
  if (status == 200 || status == 304) remember(f), g_failed[id] = "";
  else g_failed[id] = why.empty() ? "upstream" : why + (status > 0 ? ":" + std::to_string(status) : "");
  g_reading.erase(id);
  g_readers--;
  g_feedCv.notify_all();
}

void feedRoute(const Request& r, Response& out) {
  std::string url = trim(param(r, "url"));
  if (!starts(url, "http://") && !starts(url, "https://")) return out.error(400, "url");
  std::string id = rssid::showId(url);
  bool priv = param(r, "private") == "1", fresh = param(r, "fresh") == "1";
  int64_t since = atoll(param(r, "since").c_str());
  std::shared_ptr<Feed> f;
  std::string failed;
  {
    std::unique_lock<std::mutex> l(g_feedMu);
    f = recent(id);
    if (!f && !g_reading.count(id)) {
      l.unlock();
      f = loadFeed(id);
      l.lock();
      if (f) remember(f);
    }
    bool due = !f || fresh || wallMs() - f->checked > kStaleMs || wallMs() < f->checked;
    if (due && !g_reading.count(id)) {
      g_reading.insert(id);
      g_failed.erase(id);
      std::thread(readFeed, id, url, priv).detach();
    }
    // a moment for the read to end (an answer within the screen's patience); else what's kept, or "pending"
    if (g_reading.count(id))
      g_feedCv.wait_for(l, std::chrono::milliseconds(f && !fresh ? 2500 : 20000), [&] { return !g_reading.count(id); });
    if (!g_reading.count(id)) {
      if (auto now = recent(id)) f = now;
      failed = g_failed[id];
    }
    if (!f && g_reading.count(id)) {
      out.status = 202;
      out.body = "{\"pending\":true}";
      return;
    }
  }
  if (!f) {
    std::string why = failed.substr(0, failed.find(':'));
    int code = why == "not_feed" ? 422 : why == "too_big" ? 413 : why == "offline" ? 504 : 502;
    out.status = code;
    out.body = "{\"error\":" + quote(why.empty() ? "upstream" : why) + (failed.find(':') != std::string::npos ? ",\"status\":" + failed.substr(failed.find(':') + 1) : "") + "}";
    return;
  }
  std::string body = feedJson(*f, since, false, param(r, "urls") != "0");
  // kept but not read again just now (the host failed): it says so
  if (!failed.empty()) body.insert(body.size() - 1, ",\"stale\":true");
  out.body = std::move(body);
}

void notesRoute(const Request& r, Response& out) {
  std::string url = trim(param(r, "url")), guid = param(r, "guid");
  std::string dir = cacheDir(), s;
  if (url.empty() || dir.empty() || !readFile(dir + "/" + rssid::showId(url) + ".notes.json", s)) return void(out.body = "{\"notes\":\"\"}");
  Doc d = readJson(s);
  out.body = "{\"notes\":" + quote(d ? sval(yyjson_doc_get_root(d.get()), guid.c_str()) : "") + "}";
}

// ---- an episode's file ----

struct Target {
  std::string url, type;  // where its redirects end, and what it is
  int64_t total = -1;
  int64_t at = 0;  // monoMs() it was found
};
std::mutex g_targetMu;
std::map<std::string, Target> g_targets;

// where the link really is (trackers and CDNs redirect two or three times): asked once, kept 30 min
bool target(const std::string& u, Target& t, bool again) {
  {
    std::lock_guard<std::mutex> l(g_targetMu);
    auto it = g_targets.find(u);
    if (!again && it != g_targets.end() && monoMs() - it->second.at < 30 * 60000) return t = it->second, true;
  }
  Got g;
  CURL* h = handle(false);
  curl_slist* hs = curl_slist_append(nullptr, "Range: bytes=0-0");
  curl_easy_setopt(h, CURLOPT_URL, u.c_str());
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 20000L);
  // its byte read past; a host that ignores the range (the whole file) stopped after 64 KB
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) -> size_t { return s * n; });
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, +[](void*, curl_off_t, curl_off_t now, curl_off_t, curl_off_t) -> int { return now > (64 << 10) ? 1 : 0; });
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, onHead);
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &g);
  CURLcode rc = perform(h, nullptr);
  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  char* eff = nullptr;
  curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &eff);
  curl_slist_free_all(hs);
  if (rc != CURLE_OK && rc != CURLE_ABORTED_BY_CALLBACK) return false;
  if (status != 200 && status != 206) return false;
  t = Target();
  t.url = eff ? eff : u;
  t.type = g.type;
  size_t slash = g.range.rfind('/');
  if (status == 206 && slash != std::string::npos && g.range.compare(slash + 1, 1, "*") != 0) t.total = atoll(g.range.c_str() + slash + 1);
  else if (status == 200) t.total = g.length;
  t.at = monoMs();
  std::lock_guard<std::mutex> l(g_targetMu);
  if (g_targets.size() > 100) g_targets.clear();
  g_targets[u] = t;
  return true;
}

struct Span {
  bool has = false, bad = false;
  int64_t start = 0, end = -1;
};
Span span(const std::string& h, int64_t total) {
  Span s;
  std::string v = trim(h);
  if (v.empty()) return s;
  s.has = true;
  if (v.compare(0, 6, "bytes=") != 0 || v.find(',') != std::string::npos) return s.bad = true, s;
  v = v.substr(6);
  size_t dash = v.find('-');
  if (dash == std::string::npos) return s.bad = true, s;
  std::string a = v.substr(0, dash), b = v.substr(dash + 1);
  if (a.empty()) {
    if (total < 0 || b.empty()) return s.bad = true, s;
    s.start = std::max<int64_t>(0, total - atoll(b.c_str())), s.end = total - 1;
    return s;
  }
  s.start = atoll(a.c_str());
  s.end = b.empty() ? -1 : atoll(b.c_str());
  if (total >= 0) s.end = s.end < 0 ? total - 1 : std::min(s.end, total - 1);
  if (s.end >= 0 && s.end < s.start) s.bad = true;
  return s;
}

// bytes start..end (end -1: to the end) of the file, to `sink` as they come (false from it stops); the
// host's status (0: no connection, or it broke off). A host that ignores ranges (200) has the bytes
// before start read past.
int pieces(const std::string& url, int64_t start, int64_t end, const std::function<bool(const char*, size_t)>& sink) {
  struct St {
    CURL* h;
    long status = 0;
    int64_t skip = 0;
    bool began = false, stopped = false;
    const std::function<bool(const char*, size_t)>* sink;
  } st;
  st.h = handle(false);
  st.sink = &sink;
  std::string range = "Range: bytes=" + std::to_string(start) + "-" + (end >= 0 ? std::to_string(end) : std::string());
  curl_slist* hs = curl_slist_append(nullptr, range.c_str());
  auto write = [](char* d, size_t s, size_t n, void* p) -> size_t {
    St* t = static_cast<St*>(p);
    size_t len = s * n;
    if (!t->began) {
      t->began = true;
      curl_easy_getinfo(t->h, CURLINFO_RESPONSE_CODE, &t->status);
      t->skip = t->status == 200 ? t->skip : 0;  // a 206 starts where it was asked
    }
    if (t->status != 206 && t->status != 200) return 0;
    if (t->skip > 0) {
      size_t past = size_t(std::min<int64_t>(t->skip, int64_t(len)));
      t->skip -= int64_t(past);
      if (past == len) return len;
      d += past, len -= past;
    }
    if ((*t->sink)(d, len)) return s * n;
    t->stopped = true;
    return 0;
  };
  auto reset = [&] {
    st.began = st.stopped = false, st.status = 0;
    st.skip = start;
  };
  reset();
  curl_easy_setopt(st.h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(st.h, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(st.h, CURLOPT_TIMEOUT_MS, 0L);
  curl_easy_setopt(st.h, CURLOPT_WRITEFUNCTION, +write);
  curl_easy_setopt(st.h, CURLOPT_WRITEDATA, &st);
  CURLcode rc = perform(st.h, reset);
  long status = 0;
  curl_easy_getinfo(st.h, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(hs);
  if (!st.began) return rc == CURLE_OK ? int(status) : 0;  // no body: an error, or nothing there
  if (rc != CURLE_OK && !st.stopped) return 0;  // broke off on the way (stopped here: what was wanted came)
  return int(st.status);
}

// an episode's link from its show's feed kept here (a link too long for the player's paths)
std::string linkOf(const std::string& show, const std::string& episode) {
  static std::mutex mu;
  static std::map<std::string, std::string> links;
  {
    std::lock_guard<std::mutex> l(mu);
    auto it = links.find(episode);
    if (it != links.end()) return it->second;
  }
  std::shared_ptr<Feed> f;
  {
    std::lock_guard<std::mutex> l(g_feedMu);
    f = recent(show);
  }
  if (!f) f = loadFeed(show);
  if (!f) return "";
  std::string found;
  for (const rss::Episode& e : f->eps)
    if (rssid::episodeId(show, e.guid) == episode) found = e.url;
  std::lock_guard<std::mutex> l(mu);
  if (links.size() > 200) links.clear();
  if (!found.empty()) links[episode] = found;
  return found;
}

void fileRoute(const Request& r, Response& out) {
  std::string u = trim(param(r, "u"));
  if (u.empty()) u = linkOf(param(r, "s"), param(r, "e"));
  if (!starts(u, "http://") && !starts(u, "https://")) return out.error(u.empty() ? 404 : 400, u.empty() ? "no_episode" : "url");
  Target t;
  if (!target(u, t, false) && !target(u, t, true)) return out.error(502, "upstream");
  Span s = span(r.range, t.total);
  if (s.bad || (s.has && t.total >= 0 && s.start >= t.total)) {
    out.error(416, "range");
    out.headers.push_back("Content-Range: bytes */" + (t.total >= 0 ? std::to_string(t.total) : std::string("*")));
    return;
  }
  int64_t start = s.has ? s.start : 0, end = s.has ? s.end : t.total - 1;
  std::string type = t.type.empty() ? "application/octet-stream" : t.type;
  out.headers.push_back("Accept-Ranges: bytes");
  out.headers.push_back("Cache-Control: no-store");
  auto contentRange = [&](int64_t a, int64_t b) {
    return "Content-Range: bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + (t.total >= 0 ? std::to_string(t.total) : std::string("*"));
  };
  // the player's 256 KB pieces: fetched whole first, so a failure can still say so (and a link that
  // expired is found again once)
  if (end >= 0 && end - start + 1 <= (1 << 20)) {
    std::string body;
    int64_t want = end - start + 1;
    int code = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
      body.clear();
      code = pieces(t.url, start, end, [&](const char* d, size_t n) {
        body.append(d, size_t(std::min<int64_t>(int64_t(n), want - int64_t(body.size()))));
        return int64_t(body.size()) < want;  // enough: the rest of a whole file isn't wanted
      });
      if ((code == 206 || code == 200) && !body.empty()) break;
      if (attempt == 0 && !target(u, t, true)) break;  // expired: from the original link once more
    }
    if ((code != 206 && code != 200) || body.empty()) return out.headers.clear(), out.error(502, "upstream");
    out.type = type;
    out.status = s.has ? 206 : 200;
    if (s.has) out.headers.push_back(contentRange(start, start + int64_t(body.size()) - 1));
    out.body = std::move(body);
    return;
  }
  // a large or open range (a download): as it comes
  out.type = type;
  out.status = s.has ? 206 : 200;
  if (end >= 0) {
    out.length = end - start + 1;
    if (s.has) out.headers.push_back(contentRange(start, end));
  }
  std::string url = t.url;
  out.stream = [url, start, end](const Write& write) {
    int64_t sent = 0, want = end >= 0 ? end - start + 1 : -1;
    int code = pieces(url, start, end, [&](const char* d, size_t n) {
      if (want >= 0 && sent + int64_t(n) > want) n = size_t(want - sent);
      sent += int64_t(n);
      if (n && !write(d, n)) return false;
      return want < 0 || sent < want;
    });
    return (code == 206 || code == 200) && (want < 0 || sent == want);
  };
}

// ---- pictures, chapters ----

// Apple's pictures at the size asked ("…/600x600bb.jpg" -> "…/240x240bb.jpg"); others as they are
std::string sizedArt(const std::string& u, int bucket) {
  size_t slash = u.rfind('/');
  if (u.find("mzstatic.com/") == std::string::npos || slash == std::string::npos) return u;
  std::string last = u.substr(slash + 1);
  size_t x = last.find('x'), bb = last.find("bb");
  if (x == std::string::npos || bb == std::string::npos || x == 0 || bb < x) return u;
  return u.substr(0, slash + 1) + std::to_string(bucket) + "x" + std::to_string(bucket) + "bb.jpg";
}

// The pictures fetched, in memory (10 MB, the least used go first); a size Apple refuses: the original
bool artBytes(const std::string& want, const std::string& original, std::string& type, std::string& body) {
  struct Pic {
    std::string type, body;
    uint64_t used = 0;
  };
  static std::mutex mu;
  static std::map<std::string, Pic> pics;
  static uint64_t clock = 0;
  static size_t bytes = 0;
  {
    std::lock_guard<std::mutex> l(mu);
    auto hit = pics.find(want);
    if (hit != pics.end()) {
      hit->second.used = ++clock;
      type = hit->second.type, body = hit->second.body;
      return true;
    }
  }
  std::vector<std::string> hs = {"Accept: image/jpeg,image/png;q=0.9,*/*;q=0.1"};
  Got g = fetch(want, hs, 6 << 20, 20000, false);
  if (g.status != 200 && want != original) g = fetch(original, hs, 6 << 20, 20000, false);
  bool picture = g.status == 200 && g.body.size() > 3 &&
                 ((uint8_t(g.body[0]) == 0xff && uint8_t(g.body[1]) == 0xd8) || !g.body.compare(1, 3, "PNG") || !g.body.compare(0, 4, "RIFF") || !g.body.compare(0, 3, "GIF"));
  if (!picture) return false;  // an error page, not a picture
  type = g.type, body = g.body;
  std::lock_guard<std::mutex> l(mu);
  bytes += body.size();
  pics[want] = {type, body, ++clock};
  while (bytes > (10u << 20) && !pics.empty()) {
    auto old = pics.begin();
    for (auto it = pics.begin(); it != pics.end(); ++it)
      if (it->second.used < old->second.used) old = it;
    bytes -= old->second.body.size();
    pics.erase(old);
  }
  return true;
}

void artRoute(const Request& r, Response& out) {
  std::string u = trim(param(r, "u"));
  if (!starts(u, "http://") && !starts(u, "https://")) return out.error(400, "url");
  int w = atoi(param(r, "w").c_str()), h = atoi(param(r, "h").c_str());
  int want = std::max(std::max(w, h), 60) * 2, bucket = want <= 120 ? 120 : want <= 240 ? 240 : want <= 480 ? 480 : 720;
  std::string sized = sizedArt(u, bucket), type, body;
  if (!artBytes(sized, u, type, body)) return out.error(502, "upstream");
  out.type = type.empty() ? "image/jpeg" : type;
  out.headers.push_back("Cache-Control: public, max-age=604800");
  out.body = std::move(body);
}

void chaptersRoute(const Request& r, Response& out) {
  std::string u = trim(param(r, "u"));
  if (!starts(u, "http://") && !starts(u, "https://")) return out.error(400, "url");
  std::string k = "chapters|" + u, body;
  if (dirCached(k, 3600000, body)) return void(out.body = body);
  Got g = fetch(u, {"Accept: application/json+chapters, application/json;q=0.9"}, 1 << 20);
  if (g.status != 200) return out.error(502, "upstream");
  Doc d = readJson(g.body);
  if (!d) return out.error(422, "not_chapters");
  std::vector<std::pair<int64_t, std::string>> cs;
  each(key(yyjson_doc_get_root(d.get()), "chapters"), [&](yyjson_val* c) {
    if (yyjson_is_bool(key(c, "toc")) && !yyjson_get_bool(key(c, "toc"))) return;  // not for the table of contents
    yyjson_val* at = key(c, "startTime");
    if (!yyjson_is_num(at)) return;
    cs.push_back({int64_t(yyjson_get_num(at) * 1000), rss::plainText(sval(c, "title"), 200, true)});
  });
  std::stable_sort(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.first < b.first; });
  body = "{\"chapters\":[";
  for (size_t i = 0; i < cs.size(); i++) body += (i ? "," : "") + std::string("{\"ms\":") + std::to_string(cs[i].first) + ",\"title\":" + quote(cs[i].second) + "}";
  body += "]}";
  dirKeep(k, body);
  out.body = body;
}

// An MP3's own chapters (ID3v2 CHAP frames, as Podlove, Hindenburg and Auphonic write them): the tag
// at the file's start read from its host (a 10-byte header, then the tag; 4 MB at most, as a big
// cover picture can make it)
std::string id3Text(const unsigned char* p, size_t n) {
  if (!n) return "";
  int enc = p[0];
  p++, n--;
  std::string o;
  if (enc == 0) {  // ISO-8859-1
    for (size_t i = 0; i < n && p[i]; i++) {
      unsigned c = p[i];
      if (c < 0x80) o += char(c);
      else o += char(0xc0 | (c >> 6)), o += char(0x80 | (c & 0x3f));
    }
  } else if (enc == 3) {  // UTF-8
    for (size_t i = 0; i < n && p[i]; i++) o += char(p[i]);
  } else {  // UTF-16 with a BOM (1), or big-endian without (2)
    bool be = enc == 2;
    size_t i = 0;
    if (enc == 1 && n >= 2) {
      be = p[0] == 0xfe && p[1] == 0xff;
      i = 2;
    }
    for (; i + 1 < n; i += 2) {
      unsigned c = be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
      if (!c) break;
      if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {  // a surrogate pair
        unsigned lo = be ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
        c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
        i += 2;
      }
      if (c < 0x80) o += char(c);
      else if (c < 0x800) o += char(0xc0 | (c >> 6)), o += char(0x80 | (c & 0x3f));
      else if (c < 0x10000) o += char(0xe0 | (c >> 12)), o += char(0x80 | ((c >> 6) & 0x3f)), o += char(0x80 | (c & 0x3f));
      else o += char(0xf0 | (c >> 18)), o += char(0x80 | ((c >> 12) & 0x3f)), o += char(0x80 | ((c >> 6) & 0x3f)), o += char(0x80 | (c & 0x3f));
    }
  }
  return rss::plainText(o, 200, true);
}

void id3Route(const Request& r, Response& out) {
  std::string u = trim(param(r, "u"));
  if (u.empty()) u = linkOf(param(r, "s"), param(r, "e"));
  if (!starts(u, "http://") && !starts(u, "https://")) return out.error(400, "url");
  std::string k = "id3|" + u, body;
  if (dirCached(k, 3600000, body)) return void(out.body = body);
  Target t;
  if (!target(u, t, false)) return out.error(502, "upstream");
  std::string head;
  int code = pieces(t.url, 0, 9, [&](const char* d, size_t n) {
    head.append(d, std::min(n, size_t(10) - head.size()));
    return head.size() < 10;
  });
  std::vector<std::pair<int64_t, std::string>> cs;
  auto done = [&] {
    std::stable_sort(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.first < b.first; });
    body = "{\"chapters\":[";
    for (size_t i = 0; i < cs.size(); i++) body += (i ? "," : "") + std::string("{\"ms\":") + std::to_string(cs[i].first) + ",\"title\":" + quote(cs[i].second) + "}";
    body += "]}";
    dirKeep(k, body);
    out.body = body;
  };
  if ((code != 206 && code != 200) || head.size() < 10 || head.compare(0, 3, "ID3") != 0) return done();
  const unsigned char* h = reinterpret_cast<const unsigned char*>(head.data());
  int version = h[3];
  size_t size = size_t(h[6] & 0x7f) << 21 | size_t(h[7] & 0x7f) << 14 | size_t(h[8] & 0x7f) << 7 | size_t(h[9] & 0x7f);
  if ((version != 3 && version != 4) || size < 10 || size > (4u << 20)) return done();
  std::string tag;
  pieces(t.url, 10, int64_t(10 + size - 1), [&](const char* d, size_t n) {
    tag.append(d, std::min(n, size - tag.size()));
    return tag.size() < size;
  });
  const unsigned char* p = reinterpret_cast<const unsigned char*>(tag.data());
  size_t at = 0;
  if (h[5] & 0x40 && tag.size() >= 4) {  // an extended header first
    size_t ext = version == 4 ? (size_t(p[0] & 0x7f) << 21 | size_t(p[1] & 0x7f) << 14 | size_t(p[2] & 0x7f) << 7 | size_t(p[3] & 0x7f))
                              : (size_t(p[0]) << 24 | size_t(p[1]) << 16 | size_t(p[2]) << 8 | size_t(p[3])) + 4;
    at = ext;
  }
  auto frameSize = [&](const unsigned char* f) {
    return version == 4 ? size_t(f[0] & 0x7f) << 21 | size_t(f[1] & 0x7f) << 14 | size_t(f[2] & 0x7f) << 7 | size_t(f[3] & 0x7f)
                        : size_t(f[0]) << 24 | size_t(f[1]) << 16 | size_t(f[2]) << 8 | size_t(f[3]);
  };
  while (at + 10 <= tag.size() && p[at] >= 'A' && p[at] <= 'Z') {
    size_t fs = frameSize(p + at + 4);
    if (fs == 0 || at + 10 + fs > tag.size()) break;
    if (!memcmp(p + at, "CHAP", 4)) {
      const unsigned char* f = p + at + 10;
      size_t idEnd = 0;
      while (idEnd < fs && f[idEnd]) idEnd++;
      if (idEnd + 17 <= fs) {
        const unsigned char* tm = f + idEnd + 1;
        int64_t start = int64_t(tm[0]) << 24 | int64_t(tm[1]) << 16 | int64_t(tm[2]) << 8 | int64_t(tm[3]);
        std::string title;
        size_t sub = idEnd + 17;
        while (sub + 10 <= fs) {  // its own frames: the title (TIT2)
          size_t ss = frameSize(f + sub + 4);
          if (ss == 0 || sub + 10 + ss > fs) break;
          if (!memcmp(f + sub, "TIT2", 4)) title = id3Text(f + sub + 10, ss);
          sub += 10 + ss;
        }
        cs.push_back({start, title});
      }
    }
    at += 10 + fs;
  }
  done();
}

void keyRoute(const Request& r, Response& out) {
  if (r.method == "POST") {
    std::string p = keyPath();
    if (p.empty()) return out.error(503, "no_data");
    Doc d = readJson(r.body);
    std::string k = d ? trim(sval(yyjson_doc_get_root(d.get()), "key")) : "", s = d ? trim(sval(yyjson_doc_get_root(d.get()), "secret")) : "";
    if (k.empty() || s.empty()) {
      unlink(p.c_str());
    } else {
      if (k.size() > 200 || s.size() > 200) return out.error(400, "key");
      std::string tmp = p + ".part";
      int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0) return out.error(500, "write");
      std::string text = "{\"key\":" + quote(k) + ",\"secret\":" + quote(s) + "}\n";
      bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
      close(fd);
      if (!ok || rename(tmp.c_str(), p.c_str()) != 0) return out.error(500, "write");
    }
    std::lock_guard<std::mutex> l(g_dirMu);
    g_dir.clear();  // searches again with or without it
  }
  std::string k, s;
  out.body = std::string("{\"own\":") + (ownKey(k, s) ? "true" : "false") + "}";
}

}  // namespace

void configure(const std::string& server, const std::string& token) {
  std::lock_guard<std::mutex> l(g_cfgMu);
  if (server != g_server) g_noFeedRouteUntil = g_noIndexUntil = 0;
  g_server = server, g_token = token;
  while (!g_server.empty() && g_server.back() == '/') g_server.pop_back();
}

bool route(const Request& r, Response& out) {
  const std::string& p = r.path;
  if (!starts(p, "/rss/")) return false;
  if (p == "/rss/key") return keyRoute(r, out), true;
  if (r.method == "POST") return out.error(404, "not_found"), true;
  if (p == "/rss/search") return searchRoute(r, out), true;
  if (p == "/rss/top") return topRoute(r, out), true;
  if (p == "/rss/feed") return feedRoute(r, out), true;
  if (p == "/rss/notes") return notesRoute(r, out), true;
  if (p == "/rss/file") return fileRoute(r, out), true;
  if (p == "/rss/art") return artRoute(r, out), true;
  if (p == "/rss/chapters") return chaptersRoute(r, out), true;
  if (p == "/rss/id3") return id3Route(r, out), true;
  out.error(404, "not_found");
  return true;
}

}  // namespace rondo::direct::rss
