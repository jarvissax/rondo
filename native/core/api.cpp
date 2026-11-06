#include "api.h"

#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <yyjson.h>

#include "clock.h"

namespace rondo {

extern const char kRootCerts[];  // generated from app/app/src/main/res/raw/*.pem (native/CMakeLists.txt)

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

int Api::get(const std::string& path, std::string& body) {
  if (!configured()) return 0;
  curl_slist* headers = curl_slist_append(nullptr, auth_.c_str());
  std::string url = base_ + path;
  curl_blob roots = {(void*)kRootCerts, strlen(kRootCerts), CURL_BLOB_NOCOPY};
  long status = 0;
  int64_t t0 = now_ns();
  for (int attempt = 0; attempt < 20; attempt++) {
    body.clear();
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, "Rondo/2.0-dev (Innioasis Y1; native)");
    curl_easy_setopt(curl_, CURLOPT_CAINFO_BLOB, &roots);
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "");  // gzip from the server
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 30000L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(curl_);
    if (rc == CURLE_OK) {
      curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
      break;
    }
    if (rc != CURLE_COULDNT_CONNECT) break;  // only a refused connect is worth asking again at once
    usleep(250000);
  }
  curl_slist_free_all(headers);
  lastMs_ = int((now_ns() - t0) / 1000000);
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

int Api::health(Health& out) {
  std::string body;
  int status = get("/healthz", body);
  yyjson_doc* doc = status == 200 ? yyjson_read(body.data(), body.size(), 0) : nullptr;
  if (doc) {
    yyjson_val* r = yyjson_doc_get_root(doc);
    out.ok = yyjson_get_bool(yyjson_obj_get(r, "ok"));
    out.cookiesOk = yyjson_get_bool(yyjson_obj_get(r, "cookiesOk"));
    out.ytdlp = str(r, "ytdlp");
    yyjson_val* a = yyjson_obj_get(r, "account");
    if (yyjson_is_obj(a)) {
      yyjson_val* m = yyjson_obj_get(a, "music");
      yyjson_val* v = yyjson_obj_get(a, "videos");
      if (yyjson_is_bool(m)) out.music = yyjson_get_bool(m);
      if (yyjson_is_bool(v)) out.videos = yyjson_get_bool(v);
    }
    yyjson_doc_free(doc);
  }
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
