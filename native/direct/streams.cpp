// streams.h.
#include "streams.h"

#include <string.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "innertube.h"
#include "json.h"
#include "model.h"

namespace rondo::direct {
namespace {

// what the Y1 plays (resolver.ts): H.264 720p/480p/360p, AAC 128k, Opus, and 360p with sound
const std::set<int> kItags = {136, 135, 134, 140, 251, 18};
// googlevideo throttles single requests above ~10 MB to playback speed (stream.ts): bigger ranges in pieces
constexpr int64_t kPiece = 8 << 20;

struct Format {
  std::string url, mime, codecs;
  int64_t length = -1;
  int bitrate = 0, w = 0, h = 0;
};

struct Resolved {
  std::map<int, Format> formats;
  VideoDetails details;
  int64_t at = 0, expires = 0;  // monotonic ms
};

std::mutex g_mu;
std::condition_variable g_cv;
std::map<std::string, std::shared_ptr<const Resolved>> g_cache;
std::deque<std::string> g_order;
std::set<std::string> g_inflight;
std::atomic<int> g_lastOk{-1};
std::atomic<int> g_preparing{0};

std::shared_ptr<const Resolved> ask(const std::string& id) {
  Doc d;
  std::string fields = "\"videoId\":" + quote(id) + ",\"contentCheckOk\":true,\"racyCheckOk\":true";
  int status = innertube(rules().player, "player", fields, d);
  J r = root(d);
  std::string playable = str(path(r, "playabilityStatus", "status"));
  if (status != 200 || playable != "OK") {
    fprintf(stderr, "rondo-direct: %s not playable (%d %s: %s)\n", id.c_str(), status, playable.c_str(),
            str(path(r, "playabilityStatus", "reason")).c_str());
    return nullptr;
  }
  auto out = std::make_shared<Resolved>();
  J sd = key(r, "streamingData");
  auto take = [&](J f) {
    int itag = int(num(key(f, "itag")));
    std::string url = str(key(f, "url"));
    if (!kItags.count(itag) || url.empty() || out->formats.count(itag)) return;  // a cipher needs the player JavaScript
    Format x;
    x.url = url;
    std::string mt = str(key(f, "mimeType"));
    x.mime = trim(mt.substr(0, mt.find(';')));
    size_t c = mt.find("codecs=\"");
    if (c != std::string::npos) x.codecs = mt.substr(c + 8, mt.find('"', c + 8) - c - 8);
    x.length = num(key(f, "contentLength"), -1);
    x.bitrate = int(num(key(f, "bitrate")));
    x.w = int(num(key(f, "width"))), x.h = int(num(key(f, "height")));
    out->formats[itag] = x;
  };
  each(key(sd, "adaptiveFormats"), take);
  each(key(sd, "formats"), take);
  if (out->formats.empty()) {
    fprintf(stderr, "rondo-direct: %s has no format the Y1 plays\n", id.c_str());
    return nullptr;
  }
  J vd = key(r, "videoDetails");
  out->details.title = str(key(vd, "title"));
  out->details.author = str(key(vd, "author"));
  out->details.views = num(key(vd, "viewCount"), -1);
  out->details.seconds = int(num(key(vd, "lengthSeconds")));
  int64_t life = num(key(sd, "expiresInSeconds"), 21540) * 1000;
  out->at = nowMs();
  out->expires = out->at + std::max<int64_t>(60000, life - 10 * 60000);  // ten minutes before the links stop
  return out;
}

// the video's links: cached until shortly before they expire; `fresh` asks again (a link refused)
std::shared_ptr<const Resolved> resolve(const std::string& id, bool fresh = false) {
  std::unique_lock<std::mutex> l(g_mu);
  if (fresh) g_cache.erase(id);
  g_cv.wait(l, [&] { return !g_inflight.count(id); });
  auto it = g_cache.find(id);
  if (it != g_cache.end() && nowMs() < it->second->expires) return it->second;
  g_inflight.insert(id);
  l.unlock();
  auto got = ask(id);
  l.lock();
  g_inflight.erase(id);
  g_lastOk = got ? 1 : 0;
  if (got) {
    g_cache[id] = got;
    g_order.push_back(id);
    while (g_order.size() > 64) {
      g_cache.erase(g_order.front());
      g_order.pop_front();
    }
  }
  g_cv.notify_all();
  return got;
}

const char* kDelaysMs[] = {"0", "300", "600", "1200"};

// [start, end] of one format into `body`: 0, or the status to answer with. A refused link (403/410,
// or no connection) is tried again on the same link with backoff, then resolved once more (stream.ts)
int fetch(const std::string& id, int itag, int64_t start, int64_t end, std::string& body) {
  for (int round = 0; round < 2; round++) {
    auto r = resolve(id, round > 0);
    if (!r) return 502;
    auto f = r->formats.find(itag);
    if (f == r->formats.end()) return 404;
    // a link answers 403 for a moment after it is made (TV links, Oct 30): a short wait the first time
    int64_t young = r->at + 300 - nowMs();
    if (young > 0) usleep(useconds_t(young * 1000));
    std::string url = f->second.url + "&range=" + std::to_string(start) + "-" + std::to_string(end);
    for (int attempt = 0; attempt < 4; attempt++) {
      if (attempt) usleep(useconds_t(atoi(kDelaysMs[attempt]) * 1000));
      Fetched x = get(url, {"User-Agent: " + rules().player.userAgent, "Accept-Encoding: identity"}, size_t(end - start + 1) + 4096);
      if (x.status == 200 || x.status == 206) {
        body = std::move(x.body);
        return 0;
      }
      if (x.status != 403 && x.status != 410 && x.status != 0) return 502;
    }
  }
  return 502;
}

struct Range {
  bool has = false, invalid = false;
  int64_t start = 0, end = -1;
};
// single ranges only: "bytes=a-b", "bytes=a-", "bytes=-n" (stream.ts's parseRange)
Range parseRange(const std::string& h, int64_t total) {
  Range r;
  if (h.empty()) return r;
  r.has = true;
  std::string v = trim(h);
  if (v.compare(0, 6, "bytes=") != 0) return r.invalid = true, r;
  v = v.substr(6);
  size_t dash = v.find('-');
  if (dash == std::string::npos || v.find(',') != std::string::npos) return r.invalid = true, r;
  std::string a = v.substr(0, dash), b = v.substr(dash + 1);
  auto digits = [](const std::string& s) { return s.find_first_not_of("0123456789") == std::string::npos; };
  if ((a.empty() && b.empty()) || !digits(a) || !digits(b)) return r.invalid = true, r;
  if (a.empty()) {
    if (total < 0) return r.invalid = true, r;
    r.start = std::max<int64_t>(0, total - atoll(b.c_str())), r.end = total - 1;
    return r;
  }
  r.start = atoll(a.c_str());
  r.end = b.empty() ? -1 : atoll(b.c_str());
  if (r.end >= 0 && r.end < r.start) return r.invalid = true, r;
  if (total >= 0) r.end = std::min(r.end < 0 ? total - 1 : r.end, total - 1);
  return r;
}

bool parseIdItag(const std::string& p, size_t prefix, std::string& id, int& itag) {
  std::string rest = p.substr(prefix);
  size_t slash = rest.find('/');
  if (slash == std::string::npos) return false;
  id = rest.substr(0, slash);
  itag = atoi(rest.c_str() + slash + 1);
  return true;
}

}  // namespace

bool details(const std::string& id, VideoDetails& out) {
  auto r = resolve(id);
  if (!r) return false;
  out = r->details;
  return true;
}

void prepare(const std::string& id) {
  if (g_preparing >= 4) return;
  g_preparing++;
  std::thread([id] {
    resolve(id);
    g_preparing--;
  }).detach();
}

int lastResolveOk() { return g_lastOk; }

void streamRoute(const Request& r, Response& out) {
  std::string id;
  int itag = 0;
  if (!parseIdItag(r.path, 3, id, itag) || !isVideoId(id)) return out.error(400, "bad_id");
  if (!kItags.count(itag)) return out.error(404, "no_format");
  auto res = resolve(id);
  if (!res) return out.error(502, "resolve");
  auto f = res->formats.find(itag);
  if (f == res->formats.end()) return out.error(404, "no_format");
  int64_t total = f->second.length;
  std::string mime = f->second.mime.empty() ? "application/octet-stream" : f->second.mime;
  Range range = parseRange(r.range, total);
  if (range.invalid || (range.has && total >= 0 && range.start >= total)) {
    out.error(416, "range");
    out.headers.push_back("Content-Range: bytes */" + (total >= 0 ? std::to_string(total) : std::string("*")));
    return;
  }
  int64_t start = range.has ? range.start : 0;
  int64_t end = range.has ? range.end : total >= 0 ? total - 1 : -1;
  std::string totalText = total >= 0 ? std::to_string(total) : "*";
  out.headers.push_back("Accept-Ranges: bytes");
  out.headers.push_back("Cache-Control: no-store");
  out.type = mime;
  if (end >= 0 && end - start + 1 <= kPiece) {  // the player's 256 KB pieces: fetched whole, then answered
    std::string body;
    int code = fetch(id, itag, start, end, body);
    if (code) {
      out.headers.clear();
      return out.error(code, code == 404 ? "no_format" : "upstream");
    }
    out.type = mime;
    out.status = range.has ? 206 : 200;
    if (range.has) out.headers.push_back("Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(start + int64_t(body.size()) - 1) + "/" + totalText);
    out.body = std::move(body);
    return;
  }
  // a large or open range: in 1 MB pieces as they come (the first fetched before answering)
  constexpr int64_t kStep = 1 << 20;
  int64_t firstEnd = end >= 0 ? std::min(end, start + kStep - 1) : start + kStep - 1;
  auto first = std::make_shared<std::string>();
  int code = fetch(id, itag, start, firstEnd, *first);
  if (code) {
    out.headers.clear();
    return out.error(code, "upstream");
  }
  out.status = range.has ? 206 : 200;
  if (end >= 0) {
    out.length = end - start + 1;
    if (range.has) out.headers.push_back("Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + totalText);
  }
  out.stream = [id, itag, start, end, first](const Write& write) {
    if (!write(first->data(), first->size())) return false;
    int64_t at = start + int64_t(first->size());
    if (int64_t(first->size()) < kStep && end < 0) return true;  // the end of a file of unknown length
    while (end < 0 || at <= end) {
      int64_t e = end >= 0 ? std::min(end, at + kStep - 1) : at + kStep - 1;
      std::string piece;
      if (fetch(id, itag, at, e, piece) || piece.empty()) return end < 0;
      if (!write(piece.data(), piece.size())) return false;
      at += int64_t(piece.size());
      if (end < 0 && int64_t(piece.size()) < kStep) return true;
    }
    return true;
  };
}

void mpdRoute(const Request& r, Response& out) {
  std::string id = r.path.substr(5);
  if (!isVideoId(id)) return out.error(400, "bad_id");
  auto res = resolve(id);
  if (!res) return out.error(502, "resolve");
  bool video = res->formats.count(136) || res->formats.count(135) || res->formats.count(134);
  if (!video || !(res->formats.count(140) || res->formats.count(251))) return out.error(404, "no_dash");
  out.type = "application/dash+xml";
  out.headers.push_back("Cache-Control: no-store");
  out.body = "";  // the Y1 asks only to warm the links up; it plays /s/ itself
}

void resolveRoute(const Request& r, Response& out) {
  std::string id = r.path.substr(9);
  if (!isVideoId(id)) return out.error(400, "bad_id");
  auto res = resolve(id);
  if (!res) return out.error(502, "resolve");
  std::string s = "{\"duration\":" + std::to_string(res->details.seconds) + ",\"formats\":{";
  int n = 0;
  for (auto& [itag, f] : res->formats) {
    s += (n++ ? "," : "") + quote(std::to_string(itag)) + ":{\"bitrate\":" + std::to_string(f.bitrate) + ",\"w\":" + std::to_string(f.w) +
         ",\"h\":" + std::to_string(f.h) + ",\"codecs\":" + quote(f.codecs) + ",\"contentLength\":" + std::to_string(f.length) + "}";
  }
  out.body = s + "}}";
}

// Google's image server sizes by the options after the last "=" ("=w120-h120-l90-rj"): the short side
// made at least px, the rest kept (thumb.ts's sizedThumbUrl); other URLs unchanged
static std::string sized(const std::string& url, int px) {
  size_t hostEnd = url.find('/', 8);
  std::string host = url.substr(8, hostEnd == std::string::npos ? std::string::npos : hostEnd - 8);
  auto endsWith = [&](const char* s) {
    size_t l = strlen(s);
    return host.size() >= l && host.compare(host.size() - l, l, s) == 0;
  };
  if (!(endsWith("googleusercontent.com") || endsWith("ggpht.com"))) return url;
  size_t q = url.find('?');
  std::string pathPart = url.substr(0, q), query = q == std::string::npos ? "" : url.substr(q);
  size_t eq = pathPart.rfind('=');
  if (eq == std::string::npos || pathPart.find('/', eq) != std::string::npos || eq < hostEnd) return url;
  std::vector<std::string> opts;
  std::string o = pathPart.substr(eq + 1);
  size_t a = 0;
  while (a <= o.size()) {
    size_t b = o.find('-', a);
    opts.push_back(o.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  auto num = [&](char k) -> int {
    for (auto& x : opts)
      if (x.size() > 1 && x[0] == k && x.find_first_not_of("0123456789", 1) == std::string::npos) return atoi(x.c_str() + 1);
    return -1;
  };
  int w = num('w'), h = num('h'), s = num('s');
  if (w < 0 && h < 0 && s < 0) return url;
  double scale = w > 0 && h > 0 ? double(px) / std::min(w, h) : 1;
  std::string out;
  for (auto& x : opts) {
    bool isNum = x.size() > 1 && x.find_first_not_of("0123456789", 1) == std::string::npos;
    std::string y = x;
    if (isNum && x[0] == 'w' && w >= 0) y = "w" + std::to_string(h > 0 ? int(w * scale + 0.5) : px);
    else if (isNum && x[0] == 'h' && h >= 0) y = "h" + std::to_string(w > 0 ? int(h * scale + 0.5) : px);
    else if (isNum && x[0] == 's' && s >= 0) y = "s" + std::to_string(px);
    out += (out.empty() ? "" : "-") + y;
  }
  return pathPart.substr(0, eq + 1) + out + query;
}

void thumbRoute(const Request& r, Response& out) {
  auto it = r.query.find("u");
  std::string url = it == r.query.end() ? "" : it->second;
  if (thumbLink(url).empty()) return out.error(400, "host");  // Google's image hosts only: no open proxy
  auto px = [&](const char* k) {
    auto q = r.query.find(k);
    int v = q == r.query.end() ? 0 : atoi(q->second.c_str());
    return v <= 0 ? 0 : std::min(std::max(v, 16), 720);
  };
  int w = px("w") ? px("w") : 64, h = px("h");
  // twice the size asked for, rounded up to one of four sizes, so the screens' rows, Now Playing and
  // the dashboard share one picture: the Y1 (and the browser) scale it down themselves
  int want2 = std::max(w, h) * 2, bucket = want2 <= 120 ? 120 : want2 <= 240 ? 240 : want2 <= 480 ? 480 : 720;
  std::string want = sized(url, bucket);
  std::string type, body;
  if (!thumbBytes(want, url, type, body)) return out.error(502, "upstream");
  out.type = type.empty() ? "image/jpeg" : type;
  out.headers.push_back("Cache-Control: public, max-age=604800");
  out.body = std::move(body);
}

// The pictures fetched, in memory (12 MB, the oldest go first); one fetch for a picture asked for by
// several at once (the screen's row, the artwork prefetch, the dashboard); a failure tried once more.
bool thumbBytes(const std::string& want, const std::string& original, std::string& type, std::string& body) {
  struct Pic {
    std::string type, body;
    uint64_t used = 0;
  };
  static std::mutex mu;
  static std::condition_variable cv;
  static std::map<std::string, Pic> pics;
  static std::set<std::string> fetching;
  static uint64_t clock = 0;
  static size_t bytes = 0;
  std::unique_lock<std::mutex> l(mu);
  cv.wait(l, [&] { return !fetching.count(want); });
  auto hit = pics.find(want);
  if (hit != pics.end()) {
    hit->second.used = ++clock;
    type = hit->second.type, body = hit->second.body;
    return true;
  }
  fetching.insert(want);
  l.unlock();
  std::vector<std::string> hs = {"Accept: image/jpeg,image/png;q=0.9,*/*;q=0.1"};
  Fetched f;
  for (int attempt = 0; attempt < 2 && f.status != 200; attempt++) {
    if (attempt) usleep(300000);
    f = get(want, hs, 4 << 20);
    if (f.status != 200 && want != original) f = get(original, hs, 4 << 20);  // a size the image server refuses
  }
  l.lock();
  fetching.erase(want);
  bool ok = f.status == 200 && !f.body.empty();
  if (ok) {
    type = f.type, body = f.body;
    bytes += f.body.size();
    pics[want] = {std::move(f.type), std::move(f.body), ++clock};
    while (bytes > (12u << 20) && !pics.empty()) {
      auto oldest = pics.begin();
      for (auto it = pics.begin(); it != pics.end(); ++it)
        if (it->second.used < oldest->second.used) oldest = it;
      bytes -= oldest->second.body.size();
      pics.erase(oldest);
    }
  }
  cv.notify_all();
  return ok;
}

}  // namespace rondo::direct
