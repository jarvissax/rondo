// The dashboard's server (dashboard.h), DashboardServer.kt ported route by route: the same paths, bodies
// and answers, so 1.0.x's page (index.html, app.js, app.css) runs unchanged.
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "../core/api.h"
#include "../core/bundle.h"
#include "../core/clock.h"
#include "../core/cover.h"
#include "../platform/power.h"
#include "../platform/video_frame.h"
#include "../ui/image.h"
#include "app.h"
#include "dashboard.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell {
namespace {

int64_t monoNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// --- JSON: values written as text, objects and arrays from them ---

std::string str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += char(c);
        }
    }
  }
  return o + "\"";
}
std::string num(int64_t v) { return std::to_string(v); }
std::string num(double v) {
  char b[32];
  snprintf(b, sizeof(b), "%g", v);
  return b;
}
std::string boolean(bool b) { return b ? "true" : "false"; }
const std::string kNull = "null";
std::string obj(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string o = "{";
  for (size_t i = 0; i < fields.size(); i++) o += (i ? "," : "") + str(fields[i].first) + ":" + fields[i].second;
  return o + "}";
}
std::string arr(const std::vector<std::string>& items) {
  std::string o = "[";
  for (size_t i = 0; i < items.size(); i++) o += (i ? "," : "") + items[i];
  return o + "]";
}

// a request's JSON body, read with yyjson
struct Body {
  yyjson_doc* doc = nullptr;
  yyjson_val* root = nullptr;
  explicit Body(const std::string& text) {
    doc = yyjson_read(text.data(), text.size(), 0);
    root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (root && !yyjson_is_obj(root)) root = nullptr;
  }
  ~Body() { yyjson_doc_free(doc); }
  yyjson_val* get(const char* k) const { return root ? yyjson_obj_get(root, k) : nullptr; }
  bool has(const char* k) const { return get(k) != nullptr; }
  bool text(const char* k, std::string& out) const {
    yyjson_val* v = get(k);
    if (!yyjson_is_str(v)) return false;
    out.assign(yyjson_get_str(v), yyjson_get_len(v));
    return true;
  }
  bool integer(const char* k, int64_t& out) const {
    yyjson_val* v = get(k);
    if (yyjson_is_int(v)) return out = yyjson_get_sint(v), true;
    if (yyjson_is_real(v)) return out = int64_t(yyjson_get_real(v)), true;
    return false;
  }
  bool real(const char* k, double& out) const {
    yyjson_val* v = get(k);
    if (!yyjson_is_num(v)) return false;
    out = yyjson_get_num(v);
    return true;
  }
  bool flag(const char* k, bool& out) const {
    yyjson_val* v = get(k);
    if (!yyjson_is_bool(v)) return false;
    out = yyjson_get_bool(v);
    return true;
  }
  std::vector<std::string> strings(const char* k, bool& ok) const {
    std::vector<std::string> out;
    yyjson_val* a = get(k);
    ok = yyjson_is_arr(a);
    size_t i, n;
    yyjson_val* e;
    if (ok) yyjson_arr_foreach(a, i, n, e) {
        if (!yyjson_is_str(e)) return ok = false, std::vector<std::string>();
        out.emplace_back(yyjson_get_str(e), yyjson_get_len(e));
      }
    return out;
  }
};

// --- HTTP ---

struct Request {
  int fd = -1;
  std::string method, path;
  std::map<std::string, std::string> query, headers;  // header names in lower case
  std::string rest;  // what was read past the headers: the body's start
  int64_t bodyRead = 0;  // of the body, what the route took
  int64_t length() const {
    auto it = headers.find("content-length");
    return it == headers.end() ? -1 : atoll(it->second.c_str());
  }
  std::string param(const char* k) const {
    auto it = query.find(k);
    return it == query.end() ? "" : it->second;
  }
  bool hasParam(const char* k) const { return query.count(k) != 0; }
};

struct Response {
  int status = 200;
  std::string type = "application/json; charset=utf-8", body;
  std::vector<std::pair<std::string, std::string>> headers;
  bool close = false;
  std::string file;  // instead of body: this file, read as it's sent (fileBytes long)
  int64_t fileBytes = 0;
};

const char* reason(int s) {
  switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 507: return "Insufficient Storage";
    default: return "";
  }
}

std::string decode(const std::string& s, bool plus) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += char(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      o += plus && s[i] == '+' ? ' ' : s[i];
    }
  }
  return o;
}

std::string encode(const std::string& s) {  // URLEncoder.encode
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '.' || c == '-' || c == '*' || c == '_') o += char(c);
    else if (c == ' ') o += '+';
    else {
      char b[4];
      snprintf(b, sizeof(b), "%%%02X", c);
      o += b;
    }
  }
  return o;
}

bool sendAll(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return false;
    p += w, n -= size_t(w);
  }
  return true;
}

// The request line and headers (at most 16 KB); false when the connection is done.
bool readRequest(int fd, std::string& carry, Request& r) {
  std::string buf = std::move(carry);
  carry.clear();
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    if (buf.size() > 16384) return false;
    char b[4096];
    ssize_t n = recv(fd, b, sizeof(b), 0);
    if (n <= 0) return false;
    buf.append(b, size_t(n));
  }
  std::string head = buf.substr(0, end);
  r.rest = buf.substr(end + 4);
  size_t eol = head.find("\r\n");
  std::string line = head.substr(0, eol);
  size_t a = line.find(' '), b = line.find(' ', a + 1);
  if (a == std::string::npos || b == std::string::npos) return false;
  r.method = line.substr(0, a);
  std::string target = line.substr(a + 1, b - a - 1);
  size_t q = target.find('?');
  r.path = decode(target.substr(0, q), false);
  if (q != std::string::npos) {
    std::string qs = target.substr(q + 1);
    size_t at = 0;
    while (at <= qs.size()) {
      size_t amp = qs.find('&', at);
      std::string kv = qs.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
      size_t eq = kv.find('=');
      if (!kv.empty()) r.query[decode(kv.substr(0, eq), true)] = eq == std::string::npos ? "" : decode(kv.substr(eq + 1), true);
      if (amp == std::string::npos) break;
      at = amp + 1;
    }
  }
  size_t at = eol == std::string::npos ? head.size() : eol + 2;
  while (at < head.size()) {
    size_t e = head.find("\r\n", at);
    std::string h = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
    size_t colon = h.find(':');
    if (colon != std::string::npos) {
      std::string name = h.substr(0, colon), value = h.substr(colon + 1);
      for (char& c : name) c = char(tolower((unsigned char)c));
      while (!value.empty() && value[0] == ' ') value.erase(0, 1);
      r.headers[name] = value;
    }
    if (e == std::string::npos) break;
    at = e + 2;
  }
  r.fd = fd;
  return true;
}

// Streams the body's `length` bytes to sink (false from sink stops); what's left of it stays unread.
bool streamBody(Request& r, int64_t length, const std::function<bool(const char*, size_t)>& sink) {
  int64_t left = length;
  if (!r.rest.empty()) {
    size_t take = size_t(std::min<int64_t>(left, int64_t(r.rest.size())));
    r.bodyRead += int64_t(take);
    if (!sink(r.rest.data(), take)) return false;
    r.rest.erase(0, take);
    left -= int64_t(take);
  }
  std::vector<char> b(64 * 1024);
  while (left > 0) {
    ssize_t n = recv(r.fd, b.data(), size_t(std::min<int64_t>(left, int64_t(b.size()))), 0);
    if (n <= 0) return false;
    r.bodyRead += n;
    if (!sink(b.data(), size_t(n))) return false;
    left -= n;
  }
  return true;
}

// small JSON bodies only, as 1.0.x's body() (uploads stream separately)
std::string smallBody(Request& r) {
  int64_t length = r.length();
  if (length <= 0 || length > 64 * 1024) return "";
  std::string out;
  streamBody(r, length, [&](const char* p, size_t n) {
    out.append(p, n);
    return true;
  });
  return out;
}

Response json(const std::string& body) {
  Response res;
  res.body = body;
  res.headers.push_back({"Cache-Control", "no-store"});
  return res;
}
Response okBody() { return json(obj({{"ok", "true"}})); }
Response error(int status, const std::string& why) {
  Response res;
  res.status = status;
  res.headers.push_back({"Cache-Control", "no-store"});  // never kept: artwork that failed is asked again
  res.body = obj({{"error", str(why)}});
  return res;
}

// --- the UI thread, and rondo-core's answers from it ---

// Runs make() on the UI thread and returns what it wrote; "device busy" after 5 s, as onMain().
Response onUi(std::function<Response()> make) {
  auto out = std::make_shared<Response>();
  if (!onMain([out, make] { *out = make(); })) return error(500, "device busy");
  return *out;
}

// A GET or POST through rondo-core (its replies come on the UI thread), waited for here.
int coreCall(bool post, const std::string& path, const std::string& json, std::string& body, int timeoutMs = 40000) {
  struct Wait {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    bool done = false;
    int status = 0;
    std::string body;
  };
  auto w = std::make_shared<Wait>();
  postToMain([w, post, path, json] {
    auto reply = [w](int status, const std::string& b) {
      pthread_mutex_lock(&w->mu);
      w->status = status, w->body = b, w->done = true;
      pthread_cond_signal(&w->cv);
      pthread_mutex_unlock(&w->mu);
    };
    if (post) g_core.post(path, json, reply);
    else g_core.get(path, 0, reply);
  });
  int64_t until = now_ns() + int64_t(timeoutMs) * kMs;
  pthread_mutex_lock(&w->mu);
  while (!w->done)
    if (waitUntil(&w->cv, &w->mu, until) == ETIMEDOUT) break;
  int status = w->done ? w->status : 0;
  body = w->body;
  pthread_mutex_unlock(&w->mu);
  return status;
}

// --- the card ---

const char* const kRoots[] = {"/storage/sdcard0", "/storage/sdcard1"};
bool isDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::string cardRoot() {
  for (const char* r : kRoots)
    if (isDir(r)) return r;
  return "";
}
bool inside(const std::string& path) {
  if (path.find("..") != std::string::npos) return false;
  for (const char* r : kRoots)
    if (path.compare(0, strlen(r) + 1, std::string(r) + "/") == 0) return true;
  return false;
}
std::string extOf(const std::string& name) {
  size_t dot = name.rfind('.'), slash = name.rfind('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  std::string e = name.substr(dot + 1);
  for (char& c : e) c = char(tolower((unsigned char)c));
  return e;
}
const std::set<std::string> kMediaExt = {"flac", "mp3", "m4a", "aac", "ogg", "opus", "wav", "mp4", "m4v", "mkv", "3gp", "webm", "lrc",
                                         "srt", "ass", "vtt", "jpg", "jpeg", "png", "gif", "webp", "bmp", "m3u8", "m3u"};
const std::set<std::string> kSongExt = {"flac", "mp3", "m4a", "aac", "ogg", "opus", "wav"};

// WifiManager.calculateSignalLevel(rssi, 5)
int signalLevel(int rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -55) return 4;
  return int(float(rssi + 100) * 4.f / 45.f);
}
std::string ipText(uint32_t ip) {
  char b[16];
  snprintf(b, sizeof(b), "%u.%u.%u.%u", ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24);
  return b;
}

// --- the server ---

class Server {
 public:
  Server(int listenFd, std::string assets) : listen_(listenFd), assets_(std::move(assets)) {
    code_ = newCode();
    last_ = monoNs();
  }
  void run();
  void stop() {
    stopping_ = true;
    shutdown(listen_, SHUT_RDWR);
  }
  std::string code() {
    pthread_mutex_lock(&mu_);
    std::string c = code_;
    pthread_mutex_unlock(&mu_);
    return c;
  }
  int browsers() {
    pthread_mutex_lock(&mu_);
    int64_t now = monoNs();
    int n = 0;
    for (auto& s : seen_) n += now - s.second < 30 * 1000000000LL;
    pthread_mutex_unlock(&mu_);
    return n;
  }
  int64_t last() const { return last_.load(); }
  std::atomic<int> connections{0};
  bool stopping() const { return stopping_; }
  void connection(int fd);

 private:
  static std::string newCode() {
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) (void)!read(fd, &v, sizeof(v)), close(fd);
    char b[8];
    snprintf(b, sizeof(b), "%06u", v % 1000000u);
    return b;
  }
  static std::string newToken() {
    uint8_t v[24] = {0};
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) (void)!read(fd, v, sizeof(v)), close(fd);
    std::string t;
    char b[3];
    for (uint8_t c : v) snprintf(b, sizeof(b), "%02x", c), t += b;
    return t;
  }
  Response serve(Request& r);
  Response api(Request& r, const std::string& path);
  Response pair(Request& r);
  Response file(const std::string& path);
  Response index();

  // routes
  Response status();
  Response thumb(const std::string& path);
  Response art(const std::string& path, int size);
  Response upload(Request& r);
  Response update(Request& r);
  Response putServer(const Body& b);
  Response testServer();
  Response joinWifi(const Body& b);

  int listen_;
  std::string assets_;
  std::atomic<bool> stopping_{false};
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
  std::string code_;
  std::set<std::string> sessions_;
  std::map<std::string, int64_t> seen_;
  int wrongCodes_ = 0;
  int64_t pairBlockedUntil_ = 0;
  std::atomic<int64_t> last_{0};
  // the artwork the page asked for, 8 MB of JPEGs, oldest out first
  std::list<std::pair<std::string, std::vector<uint8_t>>> art_;
  size_t artBytes_ = 0;
  bool cachedArt(const std::string& key, std::vector<uint8_t>& out) {
    pthread_mutex_lock(&mu_);
    bool hit = false;
    for (auto it = art_.begin(); it != art_.end(); ++it)
      if (it->first == key) {
        out = it->second;
        art_.splice(art_.begin(), art_, it);
        hit = true;
        break;
      }
    pthread_mutex_unlock(&mu_);
    return hit;
  }
  void keepArt(const std::string& key, const std::vector<uint8_t>& bytes) {
    pthread_mutex_lock(&mu_);
    art_.push_front({key, bytes});
    artBytes_ += bytes.size();
    while (artBytes_ > 8u * 1024 * 1024 && !art_.empty()) artBytes_ -= art_.back().second.size(), art_.pop_back();
    pthread_mutex_unlock(&mu_);
  }
};

Server* g_server = nullptr;  // the one running (UI thread)
std::shared_ptr<Server> g_running;

// A join started from the page, and whether the network refused its password (GET /api/wifi passes
// both on); a rescan a few seconds after the last upload or delete. UI thread.
std::string g_joining, g_joinError;
uint64_t g_rescanToken = 0;
// Bluetooth searches while the page polls: every GET keeps it going, 30 s without one stops it
int64_t g_btPolledAt = 0, g_btSearchedAt = 0;
bool g_btSession = false;

void rescan() { g_library.scanAsync(); }  // on the library's thread; the page sees "scanning"
void scheduleRescan() {  // several files usually arrive in a row: scan once they stop
  uint64_t token = ++g_rescanToken;
  postToMain([token] {
    if (token == g_rescanToken) rescan();
  }, 3000);
}

// --- the page ---

std::string readFile(const std::string& path, bool& ok) {
  std::string out;
  ok = false;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  char b[16384];
  size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
  fclose(f);
  ok = true;
  return out;
}

Response Server::index() {
  bool ok;
  std::string html = readFile(assets_ + "/dashboard/index.html", ok);
  if (!ok) return error(404, "not found");
  // the script and styles stamped with this build: a browser that cached an older app.js fetches the new one
  struct stat st;
  std::string stamp = kVersion;
  time_t newest = 0;  // the later of the two: either can change alone
  for (const char* f : {"/dashboard/app.js", "/dashboard/app.css"})
    if (stat((assets_ + f).c_str(), &st) == 0 && st.st_mtime > newest) newest = st.st_mtime;
  if (newest) {
    char b[24];
    snprintf(b, sizeof(b), "%llx", (unsigned long long)newest);
    stamp = b;
  }
  auto stampIn = [&](const std::string& from, const std::string& to) {
    size_t at = html.find(from);
    if (at != std::string::npos) html.replace(at, from.size(), to);
  };
  stampIn("href=\"app.css\"", "href=\"app.css?v=" + stamp + "\"");
  stampIn("src=\"app.js\"", "src=\"app.js?v=" + stamp + "\"");
  Response res;
  res.type = "text/html; charset=utf-8";
  res.body = std::move(html);
  res.headers.push_back({"Cache-Control", "no-cache"});
  return res;
}

Response Server::file(const std::string& path) {
  if (path.find("..") != std::string::npos) return error(404, "not found");
  bool ok;
  std::string body = readFile(assets_ + "/" + path, ok);
  if (!ok) return error(404, "not found");
  std::string e = extOf(path);
  Response res;
  res.type = e == "html" ? "text/html; charset=utf-8"
             : e == "css" ? "text/css; charset=utf-8"
             : e == "js" || e == "mjs" ? "application/javascript; charset=utf-8"
             : e == "svg" ? "image/svg+xml"
             : e == "ttf" ? "font/ttf"
             : e == "png" ? "image/png"
             : e == "json" ? "application/json"
                           : "application/octet-stream";
  res.body = std::move(body);
  res.headers.push_back({"Cache-Control", e == "html" ? "no-cache" : "max-age=3600"});
  return res;
}

Response Server::serve(Request& r) {
  const std::string& u = r.path;
  if (u.compare(0, 5, "/api/") == 0) return api(r, u.substr(4));
  if (u.compare(0, 7, "/fonts/") == 0) return file("fonts/" + u.substr(7));
  if (u == "/" || u.empty() || u == "/index.html") return index();
  return file("dashboard" + u);
}

void Server::connection(int fd) {
  timeval tv = {5, 0};  // NanoHTTPD's 5 s socket timeout
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  std::string carry;
  while (!stopping_) {
    Request r;
    if (!readRequest(fd, carry, r)) break;
    Response res = serve(r);
    // what's left of a body the route didn't read would be taken for the next request: skipped when it
    // has all arrived, else the connection closes
    int64_t unread = std::max<int64_t>(0, r.length()) - r.bodyRead;
    if (unread > 0 && int64_t(r.rest.size()) >= unread) r.rest.erase(0, size_t(unread)), unread = 0;
    if (unread > 0) res.close = true;
    carry = r.rest;
    auto conn = r.headers.find("connection");
    if (conn != r.headers.end() && strcasecmp(conn->second.c_str(), "close") == 0) res.close = true;
    char head[256];
    snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\n", res.status, reason(res.status),
             res.type.c_str(), (long long)(res.file.empty() ? int64_t(res.body.size()) : res.fileBytes));
    std::string h = head;
    for (auto& kv : res.headers) h += kv.first + ": " + kv.second + "\r\n";
    h += res.close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
    if (!sendAll(fd, h.data(), h.size())) break;
    if (!res.file.empty() && r.method != "HEAD") {
      int in = open(res.file.c_str(), O_RDONLY | O_CLOEXEC);
      int64_t left = res.fileBytes;
      std::vector<char> chunk(64 * 1024);
      while (in >= 0 && left > 0) {
        ssize_t n = read(in, chunk.data(), size_t(std::min<int64_t>(left, int64_t(chunk.size()))));
        if (n <= 0 || !sendAll(fd, chunk.data(), size_t(n))) break;
        left -= n;
      }
      if (in >= 0) close(in);
      if (left > 0) break;  // cut short: the connection goes
    } else if (r.method != "HEAD" && !sendAll(fd, res.body.data(), res.body.size())) {
      break;
    }
    if (res.close) break;
  }
  close(fd);
}

void Server::run() {
  for (;;) {
    sockaddr_in from;
    socklen_t len = sizeof(from);
    int fd = accept(listen_, reinterpret_cast<sockaddr*>(&from), &len);
    if (fd < 0) {
      if (stopping_) break;
      if (errno == EINTR || errno == ECONNABORTED) continue;
      usleep(100000);
      continue;
    }
    if (connections.load() >= 16) {  // a browser opens six at most
      close(fd);
      continue;
    }
    struct Job {
      std::shared_ptr<Server> self;
      int fd;
    };
    Job* j = new Job{g_running, fd};
    connections++;
    pthread_t t;
    if (pthread_create(&t, nullptr, [](void* p) -> void* {
          Job* job = static_cast<Job*>(p);
          job->self->connection(job->fd);
          job->self->connections--;
          delete job;
          return nullptr;
        }, j) == 0) {
      pthread_detach(t);
    } else {
      connections--;
      close(fd);
      delete j;
    }
  }
  close(listen_);
}

// --- API ---

std::string cookie(const std::string& header, const std::string& name) {
  size_t at = 0;
  while (at < header.size()) {
    size_t semi = header.find(';', at);
    std::string part = header.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
    while (!part.empty() && part[0] == ' ') part.erase(0, 1);
    if (part.compare(0, name.size() + 1, name + "=") == 0) return part.substr(name.size() + 1);
    if (semi == std::string::npos) break;
    at = semi + 1;
  }
  return "";
}

Response pauseFor(int64_t ms) {
  Response res;
  res.status = 429;
  res.body = obj({{"error", str("wait")}, {"retryAfterSec", num((ms + 999) / 1000)}});
  return res;
}

// five wrong guesses: a new code on the device and a 30 s pause for everyone (at most 10 guesses a
// minute against a code of a million)
Response Server::pair(Request& r) {
  Body b(smallBody(r));
  std::string sent;
  b.text("code", sent);
  while (!sent.empty() && sent.back() == ' ') sent.pop_back();
  while (!sent.empty() && sent[0] == ' ') sent.erase(0, 1);
  pthread_mutex_lock(&mu_);
  int64_t now = monoNs(), pause = pairBlockedUntil_ - now;
  if (pause > 0) {
    pthread_mutex_unlock(&mu_);
    return pauseFor(pause / 1000000);
  }
  // compared in constant time, as MessageDigest.isEqual
  bool same = sent.size() == code_.size();
  unsigned diff = 0;
  for (size_t i = 0; i < code_.size(); i++) diff |= unsigned(uint8_t(i < sent.size() ? sent[i] : 0) ^ uint8_t(code_[i]));
  if (!same || diff) {
    if (++wrongCodes_ >= 5) {
      code_ = newCode();
      wrongCodes_ = 0;
      pairBlockedUntil_ = now + 30 * 1000000000LL;
      pthread_mutex_unlock(&mu_);
      fprintf(stderr, "rondo: dashboard: five wrong codes, new code and a pause\n");
      return pauseFor(30000);
    }
    pthread_mutex_unlock(&mu_);
    return error(403, "code");
  }
  wrongCodes_ = 0;
  std::string token = newToken();
  sessions_.insert(token);
  seen_[token] = now;
  pthread_mutex_unlock(&mu_);
  last_ = now;
  Response res = json(obj({{"session", str(token)}}));
  res.headers.push_back({"Set-Cookie", "rondo_session=" + token + "; Path=/; SameSite=Strict"});
  return res;
}

// rondo-player publishes a command's effect a moment later, where 1.0.x's in-process player had it at
// once: the answer to a command says what it asked for. queued: a song just handed to the player, as the
// one playing from its start; playing, positionMs, volume: >= 0 when just asked for.
struct Asked {
  int playing = -1, volume = -1;
  int64_t positionMs = -1;
};
std::string playerJson(const std::string& queued = "", Asked asked = {}) {  // UI thread
  PlayerState s = g_player.state();
  if (asked.playing >= 0) s.playing = asked.playing;
  if (asked.positionMs >= 0) s.positionMs = asked.positionMs, s.publishedNs = 0;
  if (asked.volume >= 0) s.volume = asked.volume;
  std::string path = queued.empty() ? std::string(s.path) : queued;
  if (queued.empty() && (s.index < 0 || path.empty())) return kNull;
  if (!queued.empty()) {
    const Track* t = g_library.find(queued);
    s.playing = 1, s.positionMs = 0, s.publishedNs = 0;
    s.durationMs = t && t->durationSec > 0 ? int64_t(t->durationSec) * 1000 : 0;
  }
  bool local = !isStream(path);  // streams are /s/<id>/<itag> in both modes: not a card file
  std::string title, subtitle, thumb;
  if (local) {
    SongInfo i = songInfo(path);
    title = i.title, subtitle = i.subtitle;
  } else if (const Item* it = streamItem(path)) {
    title = it->title, subtitle = it->subtitle, thumb = it->thumb;
  }
  int64_t position = s.positionMs;
  if (s.playing && s.publishedNs) position += (monoNs() - s.publishedNs) / 1000000;
  if (s.durationMs > 0) position = std::min(position, s.durationMs);
  return obj({{"title", str(title)},
              {"subtitle", str(subtitle)},
              {"path", local ? str(path) : kNull},
              {"art", local ? str("/api/art?path=" + encode(path) + "&size=300") : !thumb.empty() ? str("/api/thumb?path=" + encode(thumb)) : kNull},
              {"source", str(local ? "device" : "youtube")},
              {"playing", boolean(s.playing)},
              {"positionMs", num(position)},
              {"durationMs", num(s.durationMs)},
              {"volume", num(int64_t(s.volume))},
              {"maxVolume", num(int64_t(s.volumeMax > 0 ? s.volumeMax : 13))},
              {"volumeLimit", num(int64_t(s.volumeLimit))}});
}

std::string wifiCurrent(const CoreState& cs) {
  if (cs.wifiState != 4 || !cs.ip) return kNull;
  return obj({{"ssid", str(cs.ssid)}, {"signal", num(int64_t(signalLevel(cs.rssi)))}, {"ip", str(ipText(cs.ip))}});
}

Response Server::status() {
  power::Battery bat = power::battery();
  std::string storage = kNull, card = cardRoot();
  struct statfs fs;
  if (!card.empty() && statfs(card.c_str(), &fs) == 0)
    storage = obj({{"freeBytes", num(int64_t(fs.f_bavail) * int64_t(fs.f_bsize))}, {"totalBytes", num(int64_t(fs.f_blocks) * int64_t(fs.f_bsize))}});
  std::string device = obj({{"name", str("Rondo")},
                            {"version", str(kVersion)},
                            {"model", str("Innioasis Y1")},
                            {"power", "true"},
                            {"canUpdate", boolean(updatesAvailable())},  // the firmware's Rondo takes a bundle from the page
                            {"firmware", num(int64_t(firmwareLevel()))}});
  std::string battery = obj({{"level", num(int64_t(bat.level))}, {"charging", boolean(bat.plugged)}});
  auto player = std::make_shared<std::string>(kNull);
  auto wifi = std::make_shared<std::string>(kNull);
  auto about = std::make_shared<std::string>(kNull);
  if (!onMain([player, wifi, about] {
        *player = playerJson();
        *wifi = wifiCurrent(g_core.state());
        std::string mac = aboutMac();
        *about = obj({{"mac", mac.empty() ? kNull : str(mac)},
                      {"timeZone", str(aboutTimeZone())},
                      {"connectTo", str(directMode() ? "Directly to YouTube" : g_config.server.url.empty() ? "Not set up" : g_config.server.url)}});
      }))
    return error(500, "device busy");
  return json(obj({{"device", device}, {"battery", battery}, {"storage", storage}, {"wifi", *wifi}, {"about", *about}, {"player", *player}}));
}

// Connections to the music server (or direct mode's local one) for artwork, lent to one page request
// at a time: the page asks for many at once, and through rondo-core they waited behind its two workers
// and the UI thread (Nov 10: covers that never came while a list was loading).
struct ArtApi {
  rondo::Api api;
  std::string base, token;
};
pthread_mutex_t g_artApiMu = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::unique_ptr<ArtApi>> g_artApis;  // idle ones
std::unique_ptr<ArtApi> borrowArtApi(const std::string& base, const std::string& token) {
  std::unique_ptr<ArtApi> a;
  pthread_mutex_lock(&g_artApiMu);
  if (!g_artApis.empty()) a = std::move(g_artApis.back()), g_artApis.pop_back();
  pthread_mutex_unlock(&g_artApiMu);
  if (!a) a.reset(new ArtApi);
  if (a->base != base || a->token != token) a->base = base, a->token = token, a->api.configure(base, token);
  return a;
}
void returnArtApi(std::unique_ptr<ArtApi> a) {
  pthread_mutex_lock(&g_artApiMu);
  if (g_artApis.size() < 4) g_artApis.push_back(std::move(a));
  pthread_mutex_unlock(&g_artApiMu);
}

// YouTube Music and YouTube artwork through the music server, or direct mode's local one
Response Server::thumb(const std::string& path) {
  if (path.compare(0, 7, "/thumb?") != 0) return error(404, "not found");
  std::vector<uint8_t> bytes;
  if (!cachedArt(path, bytes)) {
    auto where = std::make_shared<std::pair<std::string, std::string>>();  // safe if it runs after 5 s
    if (!onMain([where] { where->first = apiBase(), where->second = apiToken(); }) || where->first.empty())
      return error(404, "no art");
    const std::string base = where->first, token = where->second;
    std::unique_ptr<ArtApi> a = borrowArtApi(base, token);
    std::string body;
    int status = 0;
    for (int attempt = 0; attempt < 2 && status != 200; attempt++) {  // a dropped connection: once more
      if (attempt) usleep(400 * 1000);
      body.clear();
      status = a->api.get(path + "&w=300", body);
      if (status == 404) break;
    }
    returnArtApi(std::move(a));
    if (status != 200 || body.empty()) return error(404, "no art");
    bytes.assign(body.begin(), body.end());
    keepArt(path, bytes);
  }
  Response res;
  res.type = "image/jpeg";
  res.body.assign(bytes.begin(), bytes.end());
  res.headers.push_back({"Cache-Control", "max-age=86400"});
  return res;
}

// A video's frame for the page, kept on disk beside the Y1's own thumbnails: a frame takes mediaserver a
// second or two, one at a time, so each video's is taken once. Named by the file, its time and size
// (a changed file gets a new one) and the width asked.
std::string webFramePath(const std::string& path, const struct stat& st, int size) {
  uint64_t h = 1469598103934665603ULL;
  for (char c : path) h = (h ^ uint8_t(c)) * 1099511628211ULL;
  char name[96];
  snprintf(name, sizeof(name), "/web-%016llx-%lld-%lld-%d.jpg", (unsigned long long)h, (long long)st.st_mtime,
           (long long)st.st_size, size);
  return g_prefsPath.substr(0, g_prefsPath.rfind('/')) + "/thumbs" + name;
}
bool readWhole(const std::string& file, std::vector<uint8_t>& out) {
  FILE* fp = fopen(file.c_str(), "rb");
  if (!fp) return false;
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  out.resize(n > 0 && n < (4 << 20) ? size_t(n) : 0);
  bool ok = !out.empty() && fread(out.data(), 1, out.size(), fp) == out.size();
  fclose(fp);
  return ok;
}
void writeWhole(const std::string& file, const std::vector<uint8_t>& bytes) {
  std::string tmp = file + ".part";
  FILE* fp = fopen(tmp.c_str(), "wb");
  if (!fp) return;
  bool ok = fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
  ok = fclose(fp) == 0 && ok;
  if (ok) rename(tmp.c_str(), file.c_str());
  else unlink(tmp.c_str());
}

// the embedded cover (or a video's frame at 10 s), `size` px wide, JPEG at 85
Response Server::art(const std::string& path, int size) {
  if (!inside(path)) return error(404, "not found");
  size = std::max(16, std::min(size, 1024));
  std::string key = path + "#" + std::to_string(size);
  std::vector<uint8_t> bytes;
  if (!cachedArt(key, bytes)) {
    ui::Pixels px;
    std::vector<uint8_t> cover;
    rondo::readCover(path.c_str(), cover);
    bool ok = false;
    if (!cover.empty()) {
      int w, h;
      if (ui::imageSize(cover.data(), cover.size(), w, h) && w > 0) {
        int tw = std::min(w, size), th = std::max(1, h * tw / w);
        ok = ui::decodeImage(cover.data(), cover.size(), tw, th, px);
      }
    } else {
      static const std::set<std::string> video = {"mp4", "m4v", "mkv", "3gp", "webm"};
      ui::Pixels frame;
      struct stat st;
      bool isVideo = video.count(extOf(path)) && stat(path.c_str(), &st) == 0;
      std::string kept = isVideo ? webFramePath(path, st, size) : "";
      if (isVideo && readWhole(kept, bytes)) {
        keepArt(key, bytes);
        Response res;
        res.type = "image/jpeg";
        res.body.assign(bytes.begin(), bytes.end());
        res.headers.push_back({"Cache-Control", "max-age=86400"});
        return res;
      }
      if (isVideo && ui::grabVideoFrameWithin(path, frame) && frame.w > 0) {
        int tw = std::min(frame.w, size), th = std::max(1, frame.h * tw / frame.w);
        ok = true;
        px.w = tw, px.h = th;
        px.rgba.resize(size_t(tw) * th * 4);
        for (int y = 0; y < th; y++)  // bilinear, as createScaledBitmap(…, true)
          for (int x = 0; x < tw; x++) {
            float sx = (x + 0.5f) * frame.w / tw - 0.5f, sy = (y + 0.5f) * frame.h / th - 0.5f;
            int x0 = std::max(0, int(sx)), y0 = std::max(0, int(sy));
            int x1 = std::min(frame.w - 1, x0 + 1), y1 = std::min(frame.h - 1, y0 + 1);
            float fx = std::max(0.f, sx - x0), fy = std::max(0.f, sy - y0);
            for (int c = 0; c < 4; c++) {
              auto at = [&](int qx, int qy) { return float(frame.rgba[(size_t(qy) * frame.w + qx) * 4 + c]); };
              float v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
              px.rgba[(size_t(y) * tw + x) * 4 + c] = uint8_t(std::min(255.f, v + 0.5f));
            }
          }
      }
    }
    if (!ok || !ui::encodeJpeg(px, 85, bytes)) return error(404, "no art");
    keepArt(key, bytes);
    if (cover.empty()) {  // a video's frame: on disk too
      struct stat st;
      if (stat(path.c_str(), &st) == 0) writeWhole(webFramePath(path, st, size), bytes);
    }
  }
  Response res;
  res.type = "image/jpeg";
  res.body.assign(bytes.begin(), bytes.end());
  res.headers.push_back({"Cache-Control", "max-age=86400"});
  return res;
}

// the raw file as the body: streamed straight to the card, no temporary copy
Response Server::upload(Request& r) {
  std::string name = r.param("name");
  size_t slash = name.rfind('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  while (!name.empty() && name.back() == ' ') name.pop_back();
  while (!name.empty() && name[0] == ' ') name.erase(0, 1);
  if (name.empty() || name[0] == '.') return error(400, "name");
  std::string ext = extOf(name);
  if (!kMediaExt.count(ext)) return error(415, "type");
  std::string root = cardRoot().empty() ? kRoots[0] : cardRoot();
  std::string folder = r.hasParam("folder") ? r.param("folder") : root + "/Music";
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  if (!inside(folder + "/x")) return error(403, "folder");
  int64_t length = r.length();
  if (length < 0) return error(411, "length");
  struct statfs fs;
  if (statfs(root.c_str(), &fs) == 0 && int64_t(fs.f_bavail) * int64_t(fs.f_bsize) < length + 16LL * 1024 * 1024)
    return error(507, "full");
  // mkdirs
  for (size_t at = 1; at <= folder.size(); at++)
    if (at == folder.size() || folder[at] == '/') mkdir(folder.substr(0, at).c_str(), 0775);
  std::string stem = name.substr(0, name.rfind('.')), target = folder + "/" + name;
  struct stat st;
  for (int n = 2; stat(target.c_str(), &st) == 0; n++) target = folder + "/" + stem + " (" + std::to_string(n) + ")." + ext;
  std::string partial = folder + "/." + target.substr(target.rfind('/') + 1) + ".part";
  int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
  if (fd < 0) return error(500, "cannot save");
  bool ok = streamBody(r, length, [fd](const char* p, size_t n) {
    while (n) {
      ssize_t w = write(fd, p, n);
      if (w <= 0) return false;
      p += w, n -= size_t(w);
    }
    return true;
  });
  ok = fsync(fd) == 0 && ok;
  close(fd);
  if (!ok || rename(partial.c_str(), target.c_str()) != 0) {
    unlink(partial.c_str());
    return error(500, ok ? "cannot save" : "upload cut short");
  }
  fprintf(stderr, "rondo: dashboard: uploaded %s (%lld KB)\n", target.substr(target.rfind('/') + 1).c_str(), (long long)(length / 1024));
  postToMain([] { scheduleRescan(); });
  return json(obj({{"path", str(target)}}));
}

// an update from the page: the native firmware's update bundle comes with N6; until then, as 1.0.x on
// a firmware without its installer
// PUT /api/update: a Rondo bundle (rondo-<version>.rondo, core/bundle.h) from the page, checked and put in
// place as Settings › Software update does; Rondo restarts into it once the answer is out
Response Server::update(Request& r) {
  if (!updatesAvailable()) return error(409, "firmware");
  int64_t length = r.length();
  if (length < 0) return error(411, "length");
  std::string dir = g_prefsPath.substr(0, g_prefsPath.rfind('/')), file = dir + "/update.rondo";
  struct statfs fs;
  if (statfs(dir.c_str(), &fs) == 0 && int64_t(fs.f_bavail) * int64_t(fs.f_bsize) < length * 3 + 16LL * 1024 * 1024)
    return error(507, "full");  // the file, then what it unpacks to
  int fd = open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return error(500, "cannot save");
  bool ok = streamBody(r, length, [fd](const char* p, size_t n) {
    while (n) {
      ssize_t w = write(fd, p, n);
      if (w <= 0) return false;
      p += w, n -= size_t(w);
    }
    return true;
  });
  ok = fsync(fd) == 0 && ok;
  close(fd);
  if (!ok) {
    unlink(file.c_str());
    return error(500, "upload cut short");
  }
  std::string version, why;
  if (!bundle::install(file, dir, version, why)) {
    unlink(file.c_str());
    return error(400, why);
  }
  fprintf(stderr, "rondo: dashboard: installed %s: restarting\n", version.c_str());
  postToMain([] {
    if (g_player.state().playing) g_player.pause();
    resumeSaveNow();
    requestRestart();
  }, 1500);  // the answer reaches the page first
  return json(obj({{"version", str(version)}}));
}

// the address, and either a connect code from the server's admin page or a token typed in
std::string serverInfoJson();  // below, with the routes' bodies

Response Server::putServer(const Body& b) {
  std::string url, code, token;
  bool hasUrl = b.text("url", url), hasToken = b.text("token", token);
  if (hasUrl) {
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) url.pop_back();
    while (!url.empty() && url[0] == ' ') url.erase(0, 1);
    if (!(url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0)) return error(400, "url");
  }
  std::string typed;
  if (b.text("code", typed))
    for (char c : typed)
      if (c >= '0' && c <= '9') code += c;
  auto serverJson = [] {
    auto out = std::make_shared<std::string>();
    onMain([out] { *out = serverInfoJson(); });
    return json(*out);
  };
  if (!code.empty()) {
    if (code.size() != 6) return error(400, "code");
    std::string target = url;
    if (target.empty()) onMain([&target] { target = g_config.server.url; });
    std::string body;
    int st = coreCall(true, target + "/device/pair", "{\"code\":\"" + code + "\",\"name\":\"Rondo\"}", body);
    if (st == 403) return error(403, "code");
    if (st == 0) return error(400, "unreachable");
    if (st < 200 || st >= 300) return error(400, "HTTP " + std::to_string(st));
    Body reply(body);
    std::string paired;
    if (!reply.text("token", paired) || paired.empty()) return error(403, "code");
    onMain([target, paired] {
      g_config.server.url = target, g_config.server.token = paired, g_config.server.direct = false;
      saveConfigNow();
      if (g_onServerChanged) g_onServerChanged();
    });
    fprintf(stderr, "rondo: dashboard: connected to a server with a connect code\n");
    return serverJson();
  }
  bool direct;
  if (b.flag("direct", direct) && !hasUrl && !hasToken)
    onMain([direct] {
      if (g_config.server.direct == direct) return;
      if (!direct && g_config.server.url.empty()) return;  // no server to use: direct it stays
      g_config.server.direct = direct;
      saveConfigNow();
      if (g_onServerChanged) g_onServerChanged();
    });
  if (hasUrl || hasToken) {
    while (!token.empty() && token.back() == ' ') token.pop_back();
    while (!token.empty() && token[0] == ' ') token.erase(0, 1);
    onMain([hasUrl, url, token] {
      if (hasUrl && !url.empty()) g_config.server.url = url;
      if (!token.empty()) g_config.server.token = token;
      g_config.server.direct = false;  // a server set up here is the one to use
      saveConfigNow();
      if (g_onServerChanged) g_onServerChanged();
    });
  }
  return serverJson();
}

// reachability from /healthz (no token), then an authenticated call to check the token
Response Server::testServer() {
  int64_t started = monoNs();
  std::string body;
  int st = coreCall(false, "/healthz", "", body);
  int64_t latency = (monoNs() - started) / 1000000;
  Health h;
  if (st == 200 && !parseHealth(body, h)) st = 0;
  if (st == 200) {
    std::string ignored;
    st = coreCall(false, "/time", "", ignored);
  }
  if (st == 200)
    return json(obj({{"ok", "true"},
                     {"latencyMs", num(latency)},
                     {"ytdlp", h.ytdlp.empty() ? kNull : str(h.ytdlp)},
                     {"playbackOk", h.lastResolveOk < 0 ? kNull : boolean(h.lastResolveOk == 1)},
                     {"signedIn", boolean(h.music || h.videos)}}));
  if (st > 0) return json(obj({{"ok", "false"}, {"error", str(st == 401 || st == 403 ? "unauthorized" : "HTTP " + std::to_string(st))}}));
  return json(obj({{"ok", "false"}, {"error", str("unreachable")}}));
}

// UI thread from here on: the routes' bodies

// Settings › Server: where YouTube comes from. direct: Rondo talks to YouTube itself (chosen, or no
// server set up); directChosen: the choice as made
std::string serverInfoJson() {
  return obj({{"url", str(g_config.server.url)},
              {"tokenSet", boolean(!g_config.server.token.empty())},
              {"direct", boolean(directMode())},
              {"directChosen", boolean(g_config.server.direct)}});
}

// Settings › Sound: the player's part (the amplifier, the volume limit) beside the outputs' sound
std::string soundPageJson(int askedAmp = -9, int askedLimit = -1) {
  PlayerState s = g_player.state();
  std::string j = soundJson();
  j.pop_back();
  int maxVol = s.volumeMax > 0 ? s.volumeMax : 13;
  int amp = askedAmp != -9 ? askedAmp : s.amplifier;
  int limit = askedLimit >= 0 ? askedLimit : std::min(maxVol, std::max(0, int(s.volumeLimit)));
  return j + ",\"amplifier\":" + std::to_string(amp) + ",\"volumeLimit\":" + std::to_string(limit) +
         ",\"maxVolume\":" + std::to_string(maxVol) + "}";
}

Response putSound(const Body& b) {
  std::string out, v;
  b.text("output", out);
  int64_t n;
  bool f;
  int askedAmp = -9, askedLimit = -1;
  std::array<float, 10> gains{};
  bool hasGains = false;
  if (yyjson_val* g = b.get("custom"))
    if (yyjson_is_arr(g) && yyjson_arr_size(g) == 10) {
      hasGains = true;
      size_t i, max;
      yyjson_val* e;
      yyjson_arr_foreach(g, i, max, e) gains[i] = yyjson_is_num(e) ? float(yyjson_get_num(e)) : 0.f;
    }
  if (b.text("savePreset", v)) {
    if (!hasGains) return error(400, "custom");
    std::string problem = soundSaveUserPreset(out, v, gains);
    if (!problem.empty()) return error(400, problem);
  } else if (hasGains && !soundSetCustom(out, gains)) {
    return error(400, "output");
  }
  if (b.text("preset", v) && !soundSetPreset(out, v)) return error(400, "preset");
  if (b.integer("boostDb", n) && !soundSetBoost(out, int(n))) return error(400, "boostDb");
  if (b.flag("leveling", f)) soundSetLeveling(f);
  if (b.text("deletePreset", v) && !soundDeleteUserPreset(v)) return error(404, "preset");
  if (b.integer("amplifier", n)) {
    if (n < 0 || n > 3 || g_player.state().amplifier < 0) return error(400, "amplifier");
    g_player.amplifier(int(n), g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1) + "amplifier-shipped.bin");
    askedAmp = int(n);
  }
  if (b.integer("volumeLimit", n)) {
    PlayerState s = g_player.state();
    askedLimit = int(std::max<int64_t>(0, std::min<int64_t>(s.volumeMax > 0 ? s.volumeMax : 13, n)));
    g_player.setVolumeLimit(askedLimit);
  }
  return json(soundPageJson(askedAmp, askedLimit));
}

Response putDateTime(const Body& b) {
  bool f;
  int autoTime = -1, autoZone = -1, clock24 = -1;
  if (b.flag("autoTime", f)) autoTime = f;
  if (b.flag("autoZone", f)) autoZone = f;
  if (b.flag("clock24", f)) clock24 = f;
  if (autoTime >= 0 || autoZone >= 0 || clock24 >= 0) dateTimeSetAuto(autoTime, autoZone, clock24);
  std::string zone, problem;
  if (b.text("zone", zone)) problem = dateTimeSetZone(zone);
  int64_t epoch;
  if (problem.empty() && b.integer("now", epoch)) problem = dateTimeSetClock(epoch);
  if (!problem.empty()) return error(400, problem);
  return json(dateTimeJson());
}

std::string settingsJson() {
  static const int timeouts[] = {15, 30, 60, 120, 300, 600};
  int sec = g_prefs.screenTimeoutMs / 1000, best = 60;
  for (int t : timeouts)
    if (abs(t - sec) < abs(best - sec)) best = t;
  return obj({{"videoQuality", g_prefs.videoQuality ? num(int64_t(g_prefs.videoQuality)) : kNull},
              {"brightness", num(int64_t((g_prefs.brightness * 100 + 127) / 255))},
              {"screenTimeoutSec", num(int64_t(best))},
              {"lyricsOnOpen", boolean(g_config.behavior.lyricsOnOpen)},
              {"wheelAcceleration", str(g_config.behavior.wheelAcceleration)},
              {"wheelVibration", str(g_config.behavior.wheelVibration)},
              {"clickSound", boolean(g_prefs.clickSound)}});
}

std::string putSettings(const Body& b) {
  int64_t v;
  if (b.has("videoQuality")) {
    g_prefs.videoQuality = b.integer("videoQuality", v) && (v == 360 || v == 480 || v == 720) ? int(v) : 0;
  }
  if (b.integer("brightness", v)) {
    g_prefs.brightness = int(std::max<int64_t>(10, std::min<int64_t>(100, v)) * 255 / 100);
    applyBrightness(g_prefs.brightness);
  }
  if (b.integer("screenTimeoutSec", v)) {
    static const int timeouts[] = {15, 30, 60, 120, 300, 600};
    int best = 60;
    for (int t : timeouts)
      if (abs(t - int(v)) < abs(best - int(v))) best = t;
    g_prefs.screenTimeoutMs = best * 1000;
  }
  bool f;
  if (b.flag("clickSound", f)) g_prefs.clickSound = f;
  savePrefsNow();
  bool changed = false;
  if (b.flag("lyricsOnOpen", f)) g_config.behavior.lyricsOnOpen = f, changed = true;
  std::string s;
  if (b.text("wheelAcceleration", s) && (s == "off" || s == "normal" || s == "fast")) {
    g_config.behavior.wheelAcceleration = s, changed = true;
    if (g_app->onAccelerationChanged) g_app->onAccelerationChanged();
  }
  if (b.text("wheelVibration", s) && (s == "off" || s == "light" || s == "normal" || s == "strong"))
    g_config.behavior.wheelVibration = s, changed = true;
  if (changed) saveConfigNow();
  return settingsJson();
}

std::string themeJson() {
  const Theme& t = g_config.theme;
  std::vector<std::string> menu, presets;
  for (const MenuEntry& m : t.menu) {
    std::vector<std::pair<std::string, std::string>> f = {{"id", str(m.id)}, {"label", str(menuLabel(m.id))}, {"visible", boolean(m.visible)}};
    if (m.id == "settings") f.push_back({"locked", "true"});
    menu.push_back(obj(f));
  }
  for (const AccentPreset& p : kAccentPresets) presets.push_back(obj({{"name", str(p.name)}, {"accent", str(p.color)}}));
  return obj({{"accent", str(t.accent)},
              {"appearance", str(t.appearance)},
              {"textScale", num(double(t.textScale))},
              {"menu", arr(menu)},
              {"presets", arr(presets)}});
}

std::string putTheme(const Body& b) {
  Theme t = g_config.theme;
  std::string s;
  if (b.text("accent", s)) t.accent = s;
  if (b.text("appearance", s)) t.appearance = s;
  double d;
  if (b.real("textScale", d)) t.textScale = float(d);
  yyjson_val* menu = b.get("menu");
  if (yyjson_is_arr(menu)) {
    std::vector<MenuEntry> entries;
    bool ok = true;
    size_t i, n;
    yyjson_val* e;
    yyjson_arr_foreach(menu, i, n, e) {
      yyjson_val* id = yyjson_obj_get(e, "id");
      if (!yyjson_is_str(id)) {
        ok = false;
        break;
      }
      yyjson_val* vis = yyjson_obj_get(e, "visible");
      entries.push_back({yyjson_get_str(id), yyjson_is_bool(vis) ? yyjson_get_bool(vis) : true});
    }
    if (ok) t.menu = entries;
  }
  sanitize(t);
  g_config.theme = t;
  saveConfigNow();
  themeChanged();
  return themeJson();
}

std::string librarySongJson(const Track& t) {
  return obj({{"path", str(t.path)},
              {"title", str(t.title)},
              {"artist", str(t.artist)},
              {"album", str(t.album)},
              {"track", num(int64_t(t.track))},
              {"disc", num(int64_t(t.disc))},
              {"year", num(int64_t(t.year))},
              {"albumArtist", str(t.albumArtist)},
              {"genre", str(t.genre)},
              {"plays", num(int64_t(t.plays))},
              {"lastPlayed", num(t.lastPlayed)},
              {"rating", num(int64_t(t.rating))},
              {"durationSec", t.durationSec < 0 ? kNull : num(int64_t(t.durationSec))},
              {"size", num(t.size)},
              {"modified", num(t.modified)},
              {"art", "true"}});
}

std::string libraryJson() {
  if (g_library.songs().empty() && !g_library.scanning()) rescan();
  // the songs' and videos' part is built again only when the library changed (800+ songs)
  static uint64_t builtFor = 0;
  static std::string songsPart, videosPart;
  if (builtFor != g_library.generation()) {
    builtFor = g_library.generation();
    std::vector<std::string> songs, videos;
    for (const Track* t : g_library.songs()) songs.push_back(librarySongJson(*t));
    for (const Track* t : g_library.videos())
      videos.push_back(obj({{"path", str(t->path)},
                            {"title", str(t->title)},
                            {"durationSec", t->durationSec < 0 ? kNull : num(int64_t(t->durationSec))},
                            {"size", num(t->size)},
                            {"modified", num(t->modified)}}));
    songsPart = arr(songs), videosPart = arr(videos);
  }
  std::vector<std::string> folders;
  for (const char* r : kRoots)
    if (isDir(r)) folders.push_back(str(std::string(r) + "/Music"));
  return obj({{"songs", songsPart}, {"videos", videosPart}, {"scanning", boolean(g_library.scanning())}, {"folders", arr(folders)}});
}

std::string playlistJson(const Playlist& p) {
  std::vector<std::string> paths;
  int missing = 0, duration = 0;
  for (const std::string& path : p.paths) {
    paths.push_back(str(path));
    const Track* t = g_library.find(path);
    if (!t || t->gone) missing++;
    else if (t->durationSec > 0) duration += t->durationSec;
  }
  return obj({{"name", str(p.name)}, {"paths", arr(paths)}, {"missing", num(int64_t(missing))}, {"durationSec", num(int64_t(duration))}});
}

// Smart playlists (core/smart.h): each with its rules and what they pick now
std::string smartJson(const SmartPlaylist& p, size_t paths = 0) {
  std::vector<const Track*> songs = SmartPlaylists::songs(p, g_library);
  int duration = 0;
  std::vector<std::string> list;
  for (const Track* t : songs) {
    if (t->durationSec > 0) duration += t->durationSec;
    if (list.size() < paths) list.push_back(str(t->path));
  }
  std::string j = SmartPlaylists::toJson(p);
  while (!j.empty() && (j.back() == '\n' || j.back() == '}')) j.pop_back();
  return j + ",\"name\":" + str(p.name) + ",\"count\":" + std::to_string(songs.size()) + ",\"durationSec\":" +
         std::to_string(duration) + (paths ? ",\"paths\":" + arr(list) : std::string()) + "}";
}

std::string smartListJson() {
  std::vector<std::string> all;
  for (const SmartPlaylist& p : g_smart.all()) all.push_back(smartJson(p));
  return obj({{"playlists", arr(all)}});
}

// the "playlist" object of a body, as a smart playlist
bool smartFrom(const Body& b, SmartPlaylist& p) {
  yyjson_val* v = b.get("playlist");
  if (!yyjson_is_obj(v)) return false;
  size_t len = 0;
  char* text = yyjson_val_write(v, 0, &len);
  bool ok = text && SmartPlaylists::fromJson(std::string(text, len), p);
  free(text);
  return ok;
}

// --- Files: the card as it is, for the dashboard's file manager ---

std::string kindOfFile(const std::string& name, bool dir) {
  if (dir) return "folder";
  std::string x = extOf(name);
  if (kSongExt.count(x)) return "song";
  static const std::set<std::string> video = {"mp4", "m4v", "mkv", "3gp", "webm"}, image = {"jpg", "jpeg", "png", "gif", "webp", "bmp"},
                                      text = {"lrc", "srt", "ass", "vtt", "txt", "m3u8", "m3u", "json"};
  if (video.count(x)) return "video";
  if (image.count(x)) return "image";
  if (text.count(x)) return "text";
  return "other";
}
// a name for one entry in a folder: no slashes, no dot files, nothing FAT can't hold
std::string entryName(const std::string& in) {
  std::string n = cardName(in);
  return n.empty() || n[0] == '.' || n == ".." ? std::string() : n;
}
bool isDirPath(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::string filesJson(const std::string& dir) {
  std::vector<std::string> entries;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;  // hidden, and the .part of an upload
      std::string p = dir + "/" + e->d_name;
      struct stat st;
      if (stat(p.c_str(), &st) != 0) continue;
      bool folder = S_ISDIR(st.st_mode);
      const Track* t = folder ? nullptr : g_library.find(p);
      std::vector<std::pair<std::string, std::string>> fields = {
          {"name", str(e->d_name)}, {"kind", str(kindOfFile(e->d_name, folder))}, {"size", num(int64_t(folder ? 0 : st.st_size))},
          {"modified", num(int64_t(st.st_mtime) * 1000)}};
      if (t) fields.push_back({"title", str(t->title)}), fields.push_back({"artist", str(t->artist)});
      entries.push_back(obj(fields));
    }
    closedir(d);
  }
  std::string parent;
  for (const char* r : kRoots)
    if (dir != r && inside(dir)) parent = dir.substr(0, dir.rfind('/'));
  std::vector<std::string> roots;
  for (const char* r : kRoots)
    if (isDirPath(r)) roots.push_back(obj({{"path", str(r)}, {"name", str(std::string(r) == kRoots[0] ? "Card" : "Internal storage")}}));
  return obj({{"path", str(dir)}, {"parent", parent.empty() ? kNull : str(parent)}, {"entries", arr(entries)}, {"roots", arr(roots)}});
}
bool isRoot(const std::string& p) {
  for (const char* r : kRoots)
    if (p == r) return true;
  return false;
}
// what's in a folder, files and folders, deepest first (a delete's order)
void walkDeep(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
  if (depth > 16) return;
  if (DIR* d = opendir(dir.c_str())) {
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
      if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    for (const std::string& n : names) {
      std::string p = dir + "/" + n;
      if (isDirPath(p)) walkDeep(p, out, depth + 1);
      out.push_back(p);
    }
  }
}

// Library › Organize the card: where everything would go, and how far the moves are
std::string organizeStatusJson() {
  OrganizeStatus s = organizeStatus();
  std::vector<std::string> errors;
  for (const std::string& e : s.errors) errors.push_back(str(e));
  return obj({{"running", boolean(s.running)}, {"finished", boolean(s.finished)}, {"done", num(int64_t(s.done))},
              {"total", num(int64_t(s.total))}, {"moved", num(int64_t(s.moved))}, {"errors", arr(errors)}});
}
std::string organizePlanJson() {
  static const char* const kinds[] = {"song", "video", "lyrics", "subtitles", "cover"};
  int songs = 0, videos = 0, sidecars = 0, covers = 0, there = 0;
  std::vector<std::string> moves;
  for (auto& p : organizePlans()) {
    const OrganizePlan& plan = p.second;
    songs += plan.songs, videos += plan.videos, sidecars += plan.sidecars, covers += plan.covers, there += plan.alreadyThere;
    size_t n = p.first.size() + 1;
    for (const OrganizeMove& m : plan.moves)
      moves.push_back(obj({{"kind", str(kinds[m.kind])}, {"from", str(m.from.substr(n))}, {"to", str(m.to.substr(n))}}));
  }
  return obj({{"songs", num(int64_t(songs))}, {"videos", num(int64_t(videos))}, {"sidecars", num(int64_t(sidecars))},
              {"covers", num(int64_t(covers))}, {"alreadyThere", num(int64_t(there))}, {"moves", arr(moves)},
              {"status", organizeStatusJson()}});
}

std::string playlistsJson() {
  std::vector<std::string> all;
  for (const Playlist& p : g_playlists.all()) all.push_back(playlistJson(p));
  return obj({{"playlists", arr(all)}});
}

// songs on the card only, in the order sent, once each
std::vector<std::string> songPaths(const Body& b, bool& ok) {
  std::vector<std::string> in = b.strings("paths", ok), out;
  std::set<std::string> seen;
  for (const std::string& p : in)
    if (inside(p) && kSongExt.count(extOf(p)) && seen.insert(p).second) out.push_back(p);
  return out;
}

// a Playlists problem: 409 for a name in use, else 400
Response playlistResult(const std::string& problem, const std::string& name) {
  if (!problem.empty()) return error(problem.compare(0, 15, "There's already") == 0 ? 409 : 400, problem);
  Playlist p;
  if (!name.empty() && g_playlists.get(name, p)) return json(playlistJson(p));
  return okBody();
}

std::string wifiJson() {
  CoreState cs = g_core.state();
  if (!g_joining.empty()) {
    if (cs.wifiState == 4 && g_joining == cs.ssid) g_joining.clear(), g_joinError.clear();
    else if (g_joining == cs.failedSsid) g_joinError = "auth";
  }
  std::set<std::string> savedNames;
  std::vector<std::string> saved, nearby;
  for (int i = 0; i < cs.savedCount; i++) {
    savedNames.insert(cs.saved[i].ssid);
    saved.push_back(obj({{"id", num(int64_t(cs.saved[i].id))}, {"ssid", str(cs.saved[i].ssid)}}));
  }
  static const char* const kSecurity[] = {"open", "wep", "psk", "eap"};
  for (int i = 0; i < cs.networkCount; i++) {  // strongest first, one per name
    const WifiNetwork& n = cs.networks[i];
    if (!n.ssid[0] || !strncmp(n.ssid, "NVRAM WARNING", 13)) continue;
    nearby.push_back(obj({{"ssid", str(n.ssid)},
                          {"signal", num(int64_t(signalLevel(n.rssi)))},
                          {"security", str(kSecurity[std::min<int>(n.security, 3)])},
                          {"saved", boolean(savedNames.count(n.ssid) != 0)}}));
  }
  g_core.wifiScan();  // fresher next time
  return obj({{"enabled", boolean(cs.wifiEnabled == 2)},
              {"current", wifiCurrent(cs)},
              {"joining", g_joining.empty() ? kNull : str(g_joining)},
              {"lastError", g_joinError.empty() ? kNull : str(g_joinError)},
              {"saved", arr(saved)},
              {"nearby", arr(nearby)}});
}

Response Server::joinWifi(const Body& b) {
  std::string ssid, password;
  if (!b.text("ssid", ssid) || ssid.find_first_not_of(' ') == std::string::npos) return error(400, "ssid");
  b.text("password", password);
  return onUi([ssid, password] {
    CoreState cs = g_core.state();
    int security = -1;
    for (int i = 0; i < cs.networkCount; i++)
      if (ssid == cs.networks[i].ssid) security = cs.networks[i].security;
    if (security < 0) security = int(password.empty() ? WifiSecurity::Open : WifiSecurity::Psk);
    if (security == int(WifiSecurity::Eap)) return error(400, "enterprise");
    if (security == int(WifiSecurity::Psk) && password.size() < 8) return error(400, "password");
    g_joining = ssid, g_joinError.clear();
    int savedId = -1;
    for (int i = 0; i < cs.savedCount; i++)
      if (ssid == cs.saved[i].ssid) savedId = cs.saved[i].id;
    // answer first: joining may cut this very connection
    postToMain([ssid, password, security, savedId] {
      if (savedId >= 0 && password.empty()) g_core.wifiConnect(savedId);
      else g_core.wifiJoin(ssid, WifiSecurity(security), password);
    }, 300);
    return okBody();
  });
}

std::string stationJson(int f, const std::string& name) {
  return obj({{"frequency", num(int64_t(f))}, {"name", str(name.empty() ? radioHeardName(f) : name)}});
}

std::string radioJson() {
  PlayerState s = g_player.state();
  const Radio& r = g_config.radio;
  int f = s.fmOn || s.fmStarting ? s.fmFrequency : r.frequency;
  std::string name = s.fmName;
  if (name.empty())
    for (const Preset& p : r.presets)
      if (p.frequency == f && !p.name.empty()) name = p.name;
  if (name.empty()) name = radioHeardName(f);
  std::vector<std::string> presets, found;
  for (const Preset& p : r.presets) presets.push_back(stationJson(p.frequency, p.name));
  for (int x : r.found)
    if (std::none_of(r.presets.begin(), r.presets.end(), [x](const Preset& p) { return p.frequency == x; })) found.push_back(stationJson(x, ""));
  std::vector<std::pair<std::string, std::string>> fields = {
      {"headphones", boolean(power::headset() != 0)},
      {"on", boolean(s.fmOn)},
      {"starting", boolean(s.fmStarting)},
      {"frequency", num(int64_t(f))},
      {"name", str(name)},
      {"text", str(s.fmText)},
      {"signal", num(int64_t(s.fmSignal))},
      {"stereo", boolean(s.fmStereo)},
      {"muted", boolean(s.fmMuted)},
      // the radio's own volume, not the music volume
      {"volume", num(int64_t(s.fmOn ? s.volume : std::max(0, g_prefs.fmVolume)))},
      {"maxVolume", num(int64_t(s.volumeMax > 0 ? s.volumeMax : 13))},  // the step count the player measured
      {"speaker", boolean(r.speaker)},
      {"seeking", boolean(s.fmSeeking)},
      {"scanning", boolean(s.fmScanning)}};
  if (s.fmError) fields.push_back({"error", str(s.fmError == 1 ? "headphones" : "chip")});
  fields.push_back({"band", str(r.band)});
  fields.push_back({"min", num(int64_t(radioMin()))});
  fields.push_back({"max", num(int64_t(radioMax()))});
  fields.push_back({"presets", arr(presets)});
  fields.push_back({"found", arr(found)});
  return obj(fields);
}

// {"on": bool, "frequency": 1005, "muted": bool, "speaker": bool, "band": "world" | "japan", "volume": n}
std::string putRadio(const Body& b) {
  std::string band;
  if (b.text("band", band) && (band == "world" || band == "japan")) radioSetBand(band);
  bool f;
  if (b.flag("speaker", f)) radioSetSpeaker(f);
  int64_t v;
  if (b.integer("frequency", v)) radioTune(int(v));
  if (b.flag("muted", f) && g_player.state().fmOn) g_player.fmMute(f);
  if (b.integer("volume", v)) {
    int target = int(std::max<int64_t>(0, std::min<int64_t>(13, v)));
    PlayerState s = g_player.state();
    if (s.fmOn) g_player.changeVolume(target - s.volume);
    g_prefs.fmVolume = target;
    savePrefsNow();
  }
  if (b.flag("on", f)) {
    if (f) radioStart();  // music pauses for the radio, as on the radio screen
    else g_player.fmStop();
  }
  return radioJson();
}

// the saved stations in the page's order and names
std::string putPresets(const Body& b) {
  yyjson_val* list = b.get("presets");
  if (!yyjson_is_arr(list)) return radioJson();
  std::vector<Preset> presets;
  size_t i, n;
  yyjson_val* e;
  yyjson_arr_foreach(list, i, n, e) {
    if (!yyjson_is_obj(e)) continue;
    yyjson_val* fv = yyjson_obj_get(e, "frequency");
    if (!yyjson_is_int(fv)) continue;
    int f = int(yyjson_get_sint(fv));
    if (f < 760 || f > 1080) continue;
    if (std::any_of(presets.begin(), presets.end(), [f](const Preset& p) { return p.frequency == f; })) continue;
    yyjson_val* nv = yyjson_obj_get(e, "name");
    std::string name = yyjson_is_str(nv) ? yyjson_get_str(nv) : "";
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name[0] == ' ') name.erase(0, 1);
    {  // take(40): forty characters
      size_t at = 0, count = 0;
      while (at < name.size() && count < 40) {
        at++;
        while (at < name.size() && (uint8_t(name[at]) & 0xc0) == 0x80) at++;
        count++;
      }
      name.resize(at);
    }
    presets.push_back({f, name});
  }
  g_config.radio.presets = presets;
  radioSaved();
  return radioJson();
}

std::string bluetoothJson() {
  CoreState s = g_core.state();
  if (s.btPower < 0) return obj({{"available", "false"}});
  // the page polls: searching while it does, and again every 20 s (DashboardServer.kt)
  if (!g_btSession || (!s.btSearching && monoNs() - g_btSearchedAt > 20 * 1000000000LL)) {
    g_btSession = true;
    g_btSearchedAt = monoNs();
    g_core.btSearch(true);
  }
  g_btPolledAt = monoNs();
  std::vector<std::pair<std::string, std::string>> f = {
      {"available", "true"},
      {"state", str(s.btPower == 1 ? "turning_on" : s.btPower == 3 ? "turning_off" : s.btPower == 2 ? "on" : "off")},
      {"searching", boolean(s.btSearching)}};
  if (s.btPower == 2) {
    std::vector<std::string> mine, others;
    for (int i = 0; i < s.btCount; i++) {
      const BtDevice& d = s.bt[i];
      if (d.paired)
        mine.push_back(obj({{"address", str(d.address)},
                            {"name", str(btName(d))},
                            {"kind", str(btKind(d.cod))},
                            {"audio", boolean(btIsAudio(d.cod))},
                            {"status", str(d.a2dp == 2 ? "connected" : d.a2dp == 1 ? "connecting" : "disconnected")}}));
      else
        others.push_back(obj({{"address", str(d.address)},
                              {"name", str(btName(d))},
                              {"kind", str(btKind(d.cod))},
                              {"audio", boolean(btIsAudio(d.cod))},
                              {"pairing", boolean(d.pairing)}}));
    }
    f.push_back({"mine", arr(mine)});
    f.push_back({"others", arr(others)});
  }
  return obj(f);
}

bool validAddress(const std::string& a) {  // BluetoothAdapter.checkBluetoothAddress
  if (a.size() != 17) return false;
  for (size_t i = 0; i < 17; i++) {
    if (i % 3 == 2) {
      if (a[i] != ':') return false;
    } else if (!isdigit((unsigned char)a[i]) && !(a[i] >= 'A' && a[i] <= 'F')) {
      return false;
    }
  }
  return true;
}

Response Server::api(Request& r, const std::string& path) {
  const std::string& m = r.method;
  if (path == "/pair" && m == "POST") return pair(r);
  std::string token = r.headers.count("x-rondo-session") ? r.headers["x-rondo-session"] : cookie(r.headers["cookie"], "rondo_session");
  pthread_mutex_lock(&mu_);
  bool known = !token.empty() && sessions_.count(token);
  if (known) seen_[token] = monoNs();
  pthread_mutex_unlock(&mu_);
  if (!known) {
    Response res = error(401, "pair");
    res.close = true;
    return res;
  }
  last_ = monoNs();
  // every body read now (except uploads): an unread body would be taken for the next request
  std::string text = (m == "POST" || m == "PUT") && path != "/library/upload" && path != "/update" ? smallBody(r) : "";
  auto b = std::make_shared<Body>(text);
  auto ui = [&](std::function<std::string()> make) {
    return onUi([make] { return json(make()); });
  };
  if (path == "/status" && m == "GET") return status();
  if (path == "/player" && m == "POST")
    return ui([b] {
      PlayerState s = g_player.state();
      std::string action;
      b->text("action", action);
      int64_t v;
      Asked asked;
      if (action == "toggle") {
        g_player.togglePlay();
        if (s.index >= 0) asked.playing = !s.playing;
      } else if (action == "next") {
        g_player.next();
      } else if (action == "previous") {
        g_player.previous();
      } else if (action == "seek" && b->integer("positionMs", v)) {
        g_player.seekTo(v);
        asked.positionMs = std::max<int64_t>(0, v);
      } else if (action == "volume" && b->integer("volume", v)) {
        int target = int(std::max<int64_t>(0, std::min<int64_t>(std::min(s.volumeMax > 0 ? s.volumeMax : 13, s.volumeLimit), v)));
        g_player.changeVolume(target - s.volume);
        asked.volume = target;
      }
      return playerJson("", asked);
    });
  if (path == "/player/play" && m == "POST")
    return ui([b] {
      std::string path;
      if (!b->text("path", path)) return kNull;
      bool hasQueue;
      std::vector<std::string> queue = b->strings("queue", hasQueue), songs;
      if (hasQueue) {
        for (const std::string& p : queue)
          if (const Track* t = g_library.find(p))
            if (!t->video && !t->gone) songs.push_back(p);
      } else {
        for (const Track* t : g_library.songs()) songs.push_back(t->path);
      }
      auto it = std::find(songs.begin(), songs.end(), path);
      // songs only: a video would open the video player over Settings › Dashboard and stop it
      if (it == songs.end()) return kNull;
      g_player.playQueue(songs, int(it - songs.begin()));
      return playerJson(path);
    });
  if (path == "/thumb" && m == "GET") return thumb(r.param("path"));
  if (path == "/art" && m == "GET") return art(r.param("path"), r.hasParam("size") ? atoi(r.param("size").c_str()) : 96);
  if (path == "/library" && m == "GET") return ui([] { return libraryJson(); });
  if (path == "/library/upload" && m == "PUT") {
    Response res = upload(r);
    if (res.status != 200) res.close = true;  // the body may be unread
    return res;
  }
  if (path == "/library" && m == "DELETE") {
    std::string p = r.param("path");
    if (!inside(p) || !kMediaExt.count(extOf(p))) return error(400, "path");
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return error(404, "not found");
    if (unlink(p.c_str()) != 0) return error(500, "cannot delete");
    fprintf(stderr, "rondo: dashboard: deleted %s\n", p.substr(p.rfind('/') + 1).c_str());
    postToMain([p] {
      g_playlists.forget(p);
      scheduleRescan();
    });
    return okBody();
  }
  if (path == "/library/rating" && m == "PUT")  // a song's stars, 0 to 5 (Now Playing's options on the Y1)
    return onUi([b] {
      std::string p;
      int64_t stars;
      if (!b->text("path", p) || !b->integer("rating", stars)) return error(400, "path, rating");
      if (!g_library.setRating(p, int(stars))) return error(404, "not found");
      const Track* t = g_library.find(p);
      return json(t ? librarySongJson(*t) : "{}");
    });
  if (path == "/library/rescan" && m == "POST") {
    postToMain([] { rescan(); });
    return okBody();
  }
  if (path == "/settings" && m == "GET") return ui([] { return settingsJson(); });
  if (path == "/settings" && m == "PUT") return ui([b] { return putSettings(*b); });
  if (path == "/sound" && m == "GET") return ui([] { return soundPageJson(); });
  if (path == "/sound" && m == "PUT") return onUi([b] { return putSound(*b); });
  if (path == "/sound/match" && m == "GET") {  // headphone models, from the server's AutoEq index
    std::string q = r.param("q"), body;
    if (q.empty()) return error(400, "q");
    int st = coreCall(false, "/eq/search?q=" + rondo::Api::escape(q), "", body, 60000);
    if (st == 404) return error(404, "This server can't look headphones up yet: update it");
    if (st != 200) return error(502, st ? "HTTP " + std::to_string(st) : "The server didn't answer");
    return json(body);
  }
  if (path == "/sound/match" && m == "POST") {  // one model's correction, for an output
    std::string output, id, name, source, body;
    b->text("output", output), b->text("id", id), b->text("name", name), b->text("source", source);
    if (id.empty() || output.empty()) return error(400, "id");
    int st = coreCall(false, "/eq/preset?id=" + rondo::Api::escape(id), "", body, 60000);
    if (st != 200) return error(502, st ? "HTTP " + std::to_string(st) : "The server didn't answer");
    return onUi([output, body, name, source] {
      if (!soundSetMatch(output, body, name, source)) return error(502, "That model's correction didn't come");
      return json(soundPageJson());
    });
  }
  if (path == "/datetime" && m == "GET") return ui([] { return dateTimeJson(); });
  if (path == "/datetime" && m == "PUT") return onUi([b] { return putDateTime(*b); });
  if (path == "/theme" && m == "GET") return ui([] { return themeJson(); });
  if (path == "/theme" && m == "PUT") return ui([b] { return putTheme(*b); });
  if (path == "/wifi" && m == "GET") return ui([] { return wifiJson(); });
  if (path == "/wifi/join" && m == "POST") return joinWifi(*b);
  if (path == "/wifi/saved" && m == "DELETE") {
    if (!r.hasParam("id")) return error(400, "id");
    int id = atoi(r.param("id").c_str());
    return onUi([id] {
      g_core.wifiForget(id);
      return okBody();
    });
  }
  if (path == "/radio" && m == "GET") return ui([] { return radioJson(); });
  if (path == "/radio" && m == "PUT") return ui([b] { return putRadio(*b); });
  if (path == "/radio/seek" && m == "POST")
    return ui([b] {
      bool up = true;
      b->flag("up", up);
      radioSeek(up);
      return radioJson();
    });
  if (path == "/radio/scan" && m == "POST")
    return ui([] {
      radioScan();
      return radioJson();
    });
  if (path == "/radio/presets" && m == "PUT") return ui([b] { return putPresets(*b); });
  if (path == "/bluetooth" && m == "GET") return ui([] { return bluetoothJson(); });
  if (path == "/bluetooth" && m == "PUT")
    return ui([b] {
      bool on;
      if (b->flag("enabled", on)) {
        g_core.btPower(on);
        g_prefs.bluetoothOn = on;
        savePrefsNow();
      }
      return bluetoothJson();
    });
  if (path == "/bluetooth/search" && m == "POST")
    return ui([] {
      std::string j = bluetoothJson();
      g_core.btSearch(true);
      return j;
    });
  if (path.compare(0, 11, "/bluetooth/") == 0 && m == "POST") {
    std::string action = path.substr(11), address;
    b->text("address", address);
    if (g_core.state().btPower < 0) return error(404, "no bluetooth");
    if (!validAddress(address)) return error(400, "address");
    if (action != "connect" && action != "disconnect" && action != "forget") return error(404, "not found");
    return onUi([action, address] {
      CoreState s = g_core.state();
      bool paired = false;
      for (int i = 0; i < s.btCount; i++)
        if (address == s.bt[i].address) paired = s.bt[i].paired;
      if (action == "connect") paired ? g_core.btConnect(address) : g_core.btPair(address);  // pairs first when needed
      else if (action == "disconnect") g_core.btDisconnect(address);
      else g_core.btForget(address);
      return json(bluetoothJson());
    });
  }
  if (path == "/server" && m == "GET")
    return ui([] { return serverInfoJson(); });
  if (path == "/server" && m == "PUT") return putServer(*b);
  if (path == "/server/test" && m == "POST") return testServer();
  if (path == "/power" && m == "POST") {
    std::string action;
    b->text("action", action);
    if (action != "shutdown" && action != "restart") return error(400, "action");
    fprintf(stderr, "rondo: dashboard: power %s\n", action.c_str());
    bool restart = action == "restart";
    postToMain([restart] { powerAction(restart); }, 800);  // the answer goes out first
    return okBody();
  }
  if (path == "/update" && m == "GET")
    return json(obj({{"version", str(kVersion)}, {"canInstall", boolean(updatesAvailable())}, {"firmware", num(int64_t(firmwareLevel()))},
                     {"problem", updatesAvailable() ? kNull : str("Updates need the Rondo firmware")}}));
  if (path == "/update" && m == "PUT") {
    Response res = update(r);
    if (res.status != 200) res.close = true;
    return res;
  }
  if (path == "/playlists" && m == "GET") return ui([] { return playlistsJson(); });
  if (path == "/smart" && m == "GET") return ui([] { return smartListJson(); });
  if (path == "/organize" && m == "GET") return ui([] { return organizePlanJson(); });
  if (path == "/files" && m == "GET") {
    std::string dir = r.hasParam("path") ? r.param("path") : cardRoot().empty() ? std::string(kRoots[0]) : cardRoot();
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (!(isRoot(dir) || inside(dir)) || !isDirPath(dir)) return error(404, "not found");
    return onUi([dir] { return json(filesJson(dir)); });  // the library's titles: UI thread
  }
  if (path == "/files/download" && m == "GET") {  // a file as it is, to save on the computer
    std::string p = r.param("path");
    struct stat st;
    if (!inside(p) || stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return error(404, "not found");
    Response res;
    res.type = "application/octet-stream";
    res.file = p, res.fileBytes = st.st_size;
    std::string name = p.substr(p.rfind('/') + 1), ascii;
    for (unsigned char c : name) ascii += c < 0x20 || c >= 0x7f || c == '"' || c == '\\' ? '_' : char(c);
    res.headers.push_back({"Content-Disposition", "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + Api::escape(name)});
    return res;
  }
  if (path == "/files/folder" && m == "POST")  // a new folder in one
    return onUi([b] {
      std::string dir, name;
      b->text("path", dir), b->text("name", name);
      name = entryName(name);
      if (name.empty()) return error(400, "Type a name");
      if (!(isRoot(dir) || inside(dir)) || !isDirPath(dir)) return error(404, "not found");
      std::string p = dir + "/" + name;
      struct stat st;
      if (stat(p.c_str(), &st) == 0) return error(409, "Something by that name is there already");
      if (mkdir(p.c_str(), 0775) != 0) return error(500, strerror(errno));
      return json(filesJson(dir));
    });
  if (path == "/files/rename" && m == "POST")  // a file or folder, in its folder
    return onUi([b] {
      std::string from, name;
      b->text("path", from), b->text("name", name);
      name = entryName(name);
      if (name.empty()) return error(400, "Type a name");
      if (!inside(from) || isRoot(from)) return error(404, "not found");
      std::string dir = from.substr(0, from.rfind('/')), to = dir + "/" + name;
      struct stat st;
      if (stat(from.c_str(), &st) != 0) return error(404, "not found");
      if (to != from && strcasecmp(to.c_str(), from.c_str()) != 0 && stat(to.c_str(), &st) == 0)
        return error(409, "Something by that name is there already");
      if (rename(from.c_str(), to.c_str()) != 0) return error(500, strerror(errno));
      filesMoved({{from, to}});
      return json(filesJson(dir));
    });
  if (path == "/files/move" && m == "POST")  // files and folders into another folder
    return onUi([b] {
      bool has;
      std::vector<std::string> paths = b->strings("paths", has);
      std::string to;
      b->text("to", to);
      if (!(isRoot(to) || inside(to)) || !isDirPath(to)) return error(404, "That folder isn't there");
      std::vector<std::pair<std::string, std::string>> moved;
      std::vector<std::string> errors;
      for (const std::string& from : paths) {
        if (!inside(from) || isRoot(from)) continue;
        std::string name = from.substr(from.rfind('/') + 1), target = to + "/" + name;
        if (target == from) continue;
        if (to.compare(0, from.size() + 1, from + "/") == 0 || to == from) {
          errors.push_back(name + ": can't go into itself");
          continue;
        }
        struct stat st;
        if (stat(target.c_str(), &st) == 0) {
          errors.push_back(name + ": something by that name is there already");
          continue;
        }
        if (rename(from.c_str(), target.c_str()) != 0) {
          errors.push_back(name + ": " + strerror(errno));
          continue;
        }
        moved.push_back({from, target});
      }
      filesMoved(moved);
      std::vector<std::string> errs;
      for (const std::string& e : errors) errs.push_back(str(e));
      return json(obj({{"moved", num(int64_t(moved.size()))}, {"errors", arr(errs)}}));
    });
  if (path == "/files/delete" && m == "POST")  // files, and folders with all they hold
    return onUi([b] {
      bool has;
      std::vector<std::string> paths = b->strings("paths", has);
      int deleted = 0;
      std::vector<std::string> errors;
      for (const std::string& p : paths) {
        if (!inside(p) || isRoot(p)) continue;
        std::vector<std::string> all;
        if (isDirPath(p)) walkDeep(p, all);
        all.push_back(p);
        for (const std::string& q : all) {
          bool ok;
          if (isDirPath(q)) {
            ok = rmdir(q.c_str()) == 0;
          } else if (const Track* t = g_library.find(q)) {  // a song or video: out of the library too
            ok = g_library.deleteFile(t->path);
            if (ok) g_playlists.forget(q);
          } else {
            struct stat st;
            ok = unlink(q.c_str()) == 0 || stat(q.c_str(), &st) != 0;  // an .lrc deleteFile took already
          }
          if (ok) deleted++;
          else errors.push_back(q.substr(q.rfind('/') + 1) + ": " + strerror(errno));
        }
      }
      std::vector<std::string> errs;
      for (const std::string& e : errors) errs.push_back(str(e));
      return json(obj({{"deleted", num(int64_t(deleted))}, {"errors", arr(errs)}}));
    });
  if (path == "/organize/status" && m == "GET") return ui([] { return organizeStatusJson(); });
  if (path == "/organize" && m == "POST")
    return onUi([] {
      std::string why = organizeStart();
      if (!why.empty()) return error(409, why);
      return json(organizeStatusJson());
    });
  if (path == "/smart/songs" && m == "GET") {  // one smart playlist's songs now, every path
    std::string name = r.param("name");
    return onUi([name] {
      SmartPlaylist p;
      if (!g_smart.get(name, p)) return error(404, "not found");
      return json(smartJson(p, 5000));
    });
  }
  if (path == "/smart/preview" && m == "POST")  // what rules not saved yet pick
    return onUi([b] {
      SmartPlaylist p;
      if (!smartFrom(*b, p)) return error(400, "playlist");
      std::string why = SmartPlaylists::problem(p);
      if (!why.empty()) return error(400, why);
      return json(smartJson(p, 50));
    });
  if (path == "/smart" && m == "PUT")  // saved (oldName: renamed from it)
    return onUi([b] {
      SmartPlaylist p;
      if (!smartFrom(*b, p)) return error(400, "playlist");
      std::string old;
      b->text("oldName", old);
      std::string why = g_smart.save(p, old);
      if (!why.empty()) return error(400, why);
      return json(smartListJson());
    });
  if (path == "/smart" && m == "DELETE") {
    std::string name = r.param("name");
    return onUi([name] {
      std::string why = g_smart.remove(name);
      if (!why.empty()) return error(404, why);
      return json(smartListJson());
    });
  }
  if (path == "/playlists" && (m == "POST" || m == "PUT" || m == "DELETE")) {
    std::string name = r.param("name");
    return onUi([b, m, name] {
      std::string newName, problem;
      bool hasPaths;
      std::vector<std::string> paths = songPaths(*b, hasPaths);
      if (m == "POST") {
        b->text("name", newName);
        problem = g_playlists.create(newName, hasPaths ? paths : std::vector<std::string>());
        return playlistResult(problem, Playlists::clean(newName));
      }
      if (m == "PUT") {
        bool renamed = b->text("name", newName);
        problem = g_playlists.update(name, renamed ? &newName : nullptr, hasPaths ? &paths : nullptr);
        return playlistResult(problem, renamed ? Playlists::clean(newName) : name);
      }
      problem = g_playlists.remove(name);
      return playlistResult(problem, "");
    });
  }
  return error(404, "not found");
}

}  // namespace

bool dashboardStart(const std::string& assetsDir) {
  if (g_running) return true;
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(kDashboardPort);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(fd, 16) != 0) {
    fprintf(stderr, "rondo: dashboard: cannot listen on %d: %s\n", kDashboardPort, strerror(errno));
    close(fd);
    return false;
  }
  g_running = std::make_shared<Server>(fd, assetsDir);
  g_server = g_running.get();
  g_btSession = false;
  pthread_t t;
  auto* keep = new std::shared_ptr<Server>(g_running);
  if (pthread_create(&t, nullptr, [](void* p) -> void* {
        auto* s = static_cast<std::shared_ptr<Server>*>(p);
        (*s)->run();
        delete s;
        return nullptr;
      }, keep) != 0) {
    delete keep;
    g_running.reset();
    g_server = nullptr;
    close(fd);
    return false;
  }
  pthread_detach(t);
  fprintf(stderr, "rondo: dashboard: listening on %d\n", kDashboardPort);
  return true;
}

void dashboardStop() {
  if (!g_running) return;
  g_running->stop();
  g_running.reset();  // the threads keep it until they end
  g_server = nullptr;
  if (g_btSession) {
    g_btSession = false;
    g_core.btSearch(false);
  }
  g_joining.clear(), g_joinError.clear();
  fprintf(stderr, "rondo: dashboard: stopped\n");
}

bool dashboardRunning() { return g_running != nullptr; }
std::string dashboardCode() { return g_running ? g_running->code() : ""; }
int dashboardBrowsers() { return g_running ? g_running->browsers() : 0; }
int64_t dashboardLastRequest() { return g_running ? g_running->last() : 0; }

// the Bluetooth search the page keeps going: 30 s without a GET stops it (from the Dashboard screen's tick)
void dashboardTick() {
  if (g_btSession && monoNs() - g_btPolledAt > 30 * 1000000000LL) {
    g_btSession = false;
    g_core.btSearch(false);
  }
}

}  // namespace rondo::shell
