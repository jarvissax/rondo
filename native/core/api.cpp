#include "api.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <curl/curl.h>
#include <yyjson.h>

#include "clock.h"
#include "net.h"

namespace rondo {

namespace {

size_t collect(char* p, size_t size, size_t n, void* user) {
  static_cast<std::string*>(user)->append(p, size * n);
  return size * n;
}

std::string str(yyjson_val* o, const char* k) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
}

Item item(yyjson_val* o) {
  Item it;
  it.kind = str(o, "kind"), it.id = str(o, "id"), it.title = str(o, "title");
  it.subtitle = str(o, "subtitle"), it.thumb = str(o, "thumb");
  yyjson_val* d = yyjson_obj_get(o, "durationSec");
  if (yyjson_is_num(d)) it.durationSec = int(yyjson_get_num(d));
  return it;
}

void items(yyjson_val* arr, std::vector<Item>& out) {
  size_t i, n;
  yyjson_val* e;
  yyjson_arr_foreach(arr, i, n, e) if (yyjson_is_obj(e)) out.push_back(item(e));
}

}  // namespace

bool parseSections(const std::string& json, SectionsPage& out) {
  yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
  if (!doc) return false;
  yyjson_val* root = yyjson_doc_get_root(doc);
  size_t i, n;
  yyjson_val* s;
  yyjson_arr_foreach(yyjson_obj_get(root, "sections"), i, n, s) {
    Section sec;
    sec.title = str(s, "title");
    items(yyjson_obj_get(s, "items"), sec.items);
    out.sections.push_back(std::move(sec));
  }
  out.next = str(root, "next");
  yyjson_doc_free(doc);
  return true;
}

bool parseItems(const std::string& json, ItemsPage& out) {
  yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
  if (!doc) return false;
  yyjson_val* root = yyjson_doc_get_root(doc);
  items(yyjson_obj_get(root, "items"), out.items);
  out.next = str(root, "next");
  yyjson_doc_free(doc);
  return true;
}

Api::Api() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl_ = curl_easy_init();
}

Api::~Api() { curl_easy_cleanup(curl_); }

void Api::configure(const std::string& baseUrl, const std::string& token) {
  base_ = baseUrl;
  while (!base_.empty() && base_.back() == '/') base_.pop_back();
  auth_ = "Authorization: Bearer " + token;
  cache_.clear();
}

std::string Api::escape(const std::string& s) {
  char* e = curl_easy_escape(nullptr, s.c_str(), int(s.size()));
  std::string r = e ? e : "";
  curl_free(e);
  return r;
}

std::string Api::urlFor(const std::string& path, std::string& auth) const {
  if (path.compare(0, strlen(kLocal), kLocal) == 0) {
    auth = "Authorization: Bearer direct";
    return path;
  }
  auth = auth_;
  return base_ + path;
}

int Api::get(const std::string& path, std::string& body) {
  std::string auth, url = urlFor(path, auth);
  if (!configured() && url == base_ + path) return 0;
  curl_slist* headers = curl_slist_append(nullptr, auth.c_str());
  bool wide = false;  // the system's roots too, once the bundled two didn't verify the server
  long status = 0;
  int64_t t0 = now_ns();
  for (int attempt = 0; attempt < 20; attempt++) {
    body.clear();
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, net::userAgent());
    net::trust(curl_, wide);
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "");  // gzip from the server
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 1L);  // 15 s without data (OkHttp's read timeout)
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl_);
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
      break;
    }
    if (rc == CURLE_PEER_FAILED_VERIFICATION && !wide) {
      wide = true;
      continue;
    }
    if (!net::retryable(rc)) break;  // a refused connect or a reset is worth asking again at once
    usleep(250000);
  }
  curl_slist_free_all(headers);
  lastMs_ = int((now_ns() - t0) / 1000000);
  return int(status);
}

namespace {
struct Sink {
  FILE* f;
  int64_t done, total;
  const std::function<void(int64_t, int64_t)>* progress;
  int64_t reportedAt;
  const std::atomic<bool>* cancel = nullptr;
  CURL* h = nullptr;
  int64_t base = 0;      // where this attempt's answer starts in the file (a resume's Range)
  bool checked = false;  // the answer's status looked at, on its first bytes
  bool refused = false;  // an error answer: its body isn't the file's
};
size_t toFile(char* data, size_t size, size_t n, void* user) {
  Sink* s = static_cast<Sink*>(user);
  size_t len = size * n;
  if (!s->checked) {
    s->checked = true;
    long code = 0;
    curl_easy_getinfo(s->h, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200 && code != 206) {
      s->refused = true;
    } else if (code == 200 && s->base > 0) {  // the whole file again (the Range not taken): it starts over
      if (fseek(s->f, 0, SEEK_SET) != 0 || ftruncate(fileno(s->f), 0) != 0) return 0;
      s->base = s->done = s->reportedAt = 0;
    }
  }
  if (s->refused) return len;  // read and dropped: the status says what went wrong
  if (fwrite(data, 1, len, s->f) != len) return 0;
  s->done += int64_t(len);
  if (s->done - s->reportedAt >= 64 * 1024 || s->done == s->total) {
    s->reportedAt = s->done;
    (*s->progress)(s->done, s->total);
  }
  return len;
}
int progressInfo(void* user, curl_off_t total, curl_off_t, curl_off_t, curl_off_t) {
  Sink* s = static_cast<Sink*>(user);
  if (total > 0) s->total = s->base + int64_t(total);
  return s->cancel && s->cancel->load() ? 1 : 0;  // non-zero: curl stops
}
// a transfer worth taking up again where it stopped: the connection, the network or the server failing
// on the way (a Wi-Fi gone for a moment, the stream's source giving up on a piece)
bool transferRetryable(CURLcode rc) {
  switch (rc) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_GOT_NOTHING:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM: return true;
    default: return false;
  }
}
}  // namespace

// A transfer that breaks on the way is taken up again from the bytes already in the file (Range: a 705 MB
// video lost near its end started over from nothing), after 2, 4, 8, then 15 s without progress (1 s
// after some); progress starts the count again, so only 12 tries in a row with nothing new (2.5 minutes)
// end it. A server that answers the whole file instead starts it over.
int Api::download(const std::string& path, const std::string& file, const std::function<void(int64_t, int64_t)>& progress,
                  const std::atomic<bool>* cancel) {
  std::string auth, url = urlFor(path, auth);
  if (!configured() && url == base_ + path) return 0;
  std::string part = file + ".part";
  FILE* f = fopen(part.c_str(), "wb");
  if (!f) return 0;
  Sink sink{f, 0, -1, &progress, 0, cancel};
  sink.h = curl_;
  long status = 0;
  bool wide = false, complete = false;
  int64_t known = -1;  // the file's size, once an answer said it
  int misses = 0;
  for (int attempt = 0; attempt < 400 && misses < 12; attempt++) {
    int64_t have = sink.done;
    if (fflush(f) != 0 || fseek(f, have, SEEK_SET) != 0) break;
    sink.base = sink.reportedAt = have;
    sink.checked = sink.refused = false;
    curl_slist* headers = curl_slist_append(nullptr, auth.c_str());
    if (have > 0) headers = curl_slist_append(headers, ("Range: bytes=" + std::to_string(have) + "-").c_str());
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, net::userAgent());
    net::trust(curl_, wide);
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, toFile);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, progressInfo);
    curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, &sink);
    CURLcode rc = curl_easy_perform(curl_);
    curl_slist_free_all(headers);
    status = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
    if (sink.total > 0) known = sink.total;
    if (cancel && cancel->load()) break;
    if (rc == CURLE_OK && (status == 200 || status == 206) && (known < 0 || sink.done == known)) {
      complete = true;
      break;
    }
    if (rc == CURLE_OK && status == 416 && have > 0 && have == known) {  // it was whole already
      complete = true;
      break;
    }
    if (rc == CURLE_PEER_FAILED_VERIFICATION && !wide) {
      wide = true;
      continue;
    }
    bool again = rc == CURLE_OK ? status >= 500 || status == 408 || status == 429 || status == 206 : transferRetryable(rc);
    if (!again) break;
    misses = sink.done > have ? 0 : misses + 1;
    int waitMs = misses == 0 ? 1000 : std::min(15000, 1000 << misses);
    for (int w = 0; w < waitMs / 250 && !(cancel && cancel->load()); w++) usleep(250000);
  }
  curl_easy_reset(curl_);  // the next request sets what it needs again
  if (cancel && cancel->load()) complete = false;
  status = complete ? 200 : status == 200 || status == 206 ? 0 : status;
  bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
  fclose(f);
  if (status != 200 || !ok || rename(part.c_str(), file.c_str()) != 0) {
    unlink(part.c_str());
    return status == 200 ? 0 : int(status);
  }
  return 200;
}

int Api::post(const std::string& url, const std::string& json, std::string& body) {
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  bool wide = false;  // the system's roots too, once the bundled two didn't verify the server
  long status = 0;
  for (int attempt = 0; attempt < 20; attempt++) {
    body.clear();
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, net::userAgent());
    net::trust(curl_, wide);
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 1L);  // 15 s without data (OkHttp's read timeout)
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, long(json.size()));
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl_);
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
      break;
    }
    if (rc == CURLE_PEER_FAILED_VERIFICATION && !wide) {
      wide = true;
      continue;
    }
    if (!net::retryable(rc)) break;
    usleep(250000);
  }
  curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);  // the handle's next requests are GETs again
  curl_slist_free_all(headers);
  return int(status);
}

int Api::cached(const std::string& path, std::string& body, int64_t ttlMs) {
  int64_t now = now_ns() / 1000000;
  for (auto it = cache_.begin(); it != cache_.end(); ++it) {
    if (it->path != path) continue;
    if (now - it->at < ttlMs) {
      body = it->body;
      lastMs_ = 0;
      return 200;
    }
    cache_.erase(it);
    break;
  }
  int status = get(path, body);
  if (status == 200) {
    if (cache_.size() >= 40) cache_.erase(cache_.begin());
    cache_.push_back({path, body, now});
  }
  return status;
}

bool parseHealth(const std::string& json, Health& out) {
  yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
  if (!doc) return false;
  yyjson_val* r = yyjson_doc_get_root(doc);
  out.ok = yyjson_get_bool(yyjson_obj_get(r, "ok"));
  out.cookiesOk = yyjson_get_bool(yyjson_obj_get(r, "cookiesOk"));
  out.ytdlp = str(r, "ytdlp");
  out.version = str(r, "version");
  yyjson_val* lr = yyjson_obj_get(r, "lastResolveOk");
  if (yyjson_is_bool(lr)) out.lastResolveOk = yyjson_get_bool(lr) ? 1 : 0;
  yyjson_val* a = yyjson_obj_get(r, "account");
  if (yyjson_is_obj(a)) {
    yyjson_val* m = yyjson_obj_get(a, "music");
    yyjson_val* v = yyjson_obj_get(a, "videos");
    if (yyjson_is_bool(m)) out.music = yyjson_get_bool(m);
    if (yyjson_is_bool(v)) out.videos = yyjson_get_bool(v);
  }
  yyjson_doc_free(doc);
  return true;
}

bool parseShortInfo(const std::string& json, ShortInfo& out) {
  yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
  if (!doc) return false;
  yyjson_val* r = yyjson_doc_get_root(doc);
  out.id = str(r, "id"), out.title = str(r, "title"), out.channel = str(r, "channel"), out.subtitle = str(r, "subtitle");
  yyjson_val* d = yyjson_obj_get(r, "durationSec");
  out.durationSec = yyjson_is_num(d) ? int(yyjson_get_num(d)) : -1;
  yyjson_doc_free(doc);
  return true;
}

bool parseCaptions(const std::string& json, std::vector<CaptionTrack>& out) {
  yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
  if (!doc) return false;
  yyjson_val* tracks = yyjson_obj_get(yyjson_doc_get_root(doc), "tracks");
  size_t i, n;
  yyjson_val* t;
  yyjson_arr_foreach(tracks, i, n, t) {
    CaptionTrack c;
    c.id = str(t, "id"), c.lang = str(t, "lang"), c.name = str(t, "name");
    c.autoGen = yyjson_get_bool(yyjson_obj_get(t, "auto"));
    if (!c.id.empty()) out.push_back(std::move(c));
  }
  yyjson_doc_free(doc);
  return yyjson_is_arr(tracks);
}

int Api::health(Health& out) {
  std::string body;
  int status = get("/healthz", body);
  if (status == 200) parseHealth(body, out);
  return status;
}

int Api::musicHome(SectionsPage& out) {
  std::string body;
  int status = cached("/music/home", body);
  if (status == 200 && !parseSections(body, out)) return -1;
  return status;
}

int Api::musicSearch(const std::string& q, ItemsPage& out, const std::string& type) {
  std::string body;
  int status = get("/music/search?type=" + type + "&q=" + escape(q), body);
  if (status == 200 && !parseItems(body, out)) return -1;
  return status;
}

int Api::videoSearch(const std::string& q, ItemsPage& out) {
  std::string body;
  int status = get("/video/search?q=" + escape(q), body);
  if (status == 200 && !parseItems(body, out)) return -1;
  return status;
}

int Api::more(const std::string& path, const std::string& next, ItemsPage& out) {
  std::string body;
  int status = get(path.substr(0, path.find('?')) + "?next=" + escape(next), body);
  if (status == 200 && !parseItems(body, out)) return -1;
  return status;
}

}  // namespace rondo
