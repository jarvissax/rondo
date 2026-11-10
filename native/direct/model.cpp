// model.h.
#include "model.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace rondo::direct {

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

std::string json(const Item& i) {
  std::string s = "{\"kind\":" + quote(i.kind) + ",\"id\":" + quote(i.id) + ",\"title\":" + quote(i.title) +
                  ",\"subtitle\":" + quote(i.subtitle);
  if (i.durationSec > 0) s += ",\"durationSec\":" + std::to_string(i.durationSec);
  if (!i.thumb.empty()) s += ",\"thumb\":" + quote(i.thumb);
  return s + "}";
}

std::string json(const std::vector<Item>& items) {
  std::string s = "[";
  for (size_t k = 0; k < items.size(); k++) s += (k ? "," : "") + json(items[k]);
  return s + "]";
}

std::string json(const Section& sec) { return "{\"title\":" + quote(sec.title) + ",\"items\":" + json(sec.items) + "}"; }

bool isVideoId(const std::string& id) {
  if (id.size() != 11) return false;
  for (char c : id)
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

std::string thumbLink(const std::string& raw) {
  std::string url = raw.compare(0, 2, "//") == 0 ? "https:" + raw : raw;
  if (url.compare(0, 8, "https://") != 0) return "";
  size_t end = url.find_first_of("/?#", 8);
  std::string host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
  bool ok = false;
  for (const char* h : {".ytimg.com", ".googleusercontent.com", ".ggpht.com", ".gstatic.com"}) {
    size_t l = strlen(h);
    if (host.size() > l && host.compare(host.size() - l, l, h) == 0) ok = true;
  }
  return ok ? "/thumb?u=" + uriComponent(url) : "";
}

namespace {
std::string randomToken() {
  unsigned char b[16] = {};
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    if (read(fd, b, sizeof(b)) != ssize_t(sizeof(b))) {}
    close(fd);
  }
  static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string s;
  for (unsigned char c : b) s += abc[c & 63];
  return s;
}
}  // namespace

Tokens& tokens() {
  static Tokens t;
  return t;
}

std::string Tokens::put(const std::string& scope, std::function<std::string()> load) {
  std::lock_guard<std::mutex> l(mu_);
  std::string token = randomToken();
  entries_[token] = {scope, std::move(load), nowMs() + 30 * 60000};
  order_.push_back(token);
  while (!order_.empty()) {
    auto it = entries_.find(order_.front());
    bool drop = it == entries_.end() || it->second.expires <= nowMs() || entries_.size() > 500;
    if (!drop) break;
    if (it != entries_.end()) entries_.erase(it);
    order_.erase(order_.begin());
  }
  return token;
}

bool Tokens::resume(const std::string& scope, const std::string& token, std::function<std::string()>& load) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = entries_.find(token);
  if (it == entries_.end() || it->second.expires <= nowMs() || it->second.scope != scope) return false;
  load = it->second.load;
  return true;
}

void Tokens::clear() {
  std::lock_guard<std::mutex> l(mu_);
  entries_.clear();
  order_.clear();
}

}  // namespace rondo::direct
