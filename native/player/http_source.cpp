// HttpSource; see http_source.h.
#include "http_source.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <unistd.h>

#include <curl/curl.h>

#include "../core/net.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace rondo {


namespace {

constexpr int kChunk = 256 << 10;

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

struct Transfer {
  std::vector<uint8_t>* out;
  int64_t total = -1;  // from Content-Range: bytes a-b/total
  const std::function<bool()>* stopped;
};

size_t onBody(char* p, size_t size, size_t n, void* user) {
  auto* t = static_cast<Transfer*>(user);
  t->out->insert(t->out->end(), p, p + size * n);
  return size * n;
}

size_t onHeader(char* p, size_t size, size_t n, void* user) {
  auto* t = static_cast<Transfer*>(user);
  std::string h(p, size * n);
  if (strncasecmp(h.c_str(), "content-range:", 14) == 0) {
    size_t slash = h.find('/');
    if (slash != std::string::npos && h[slash + 1] != '*') t->total = strtoll(h.c_str() + slash + 1, nullptr, 10);
  }
  return size * n;
}

// a seek or a skip makes the transfer stop at once instead of finishing a chunk nobody wants
int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* t = static_cast<Transfer*>(user);
  return (*t->stopped)() ? 1 : 0;
}

}  // namespace

HttpSource::HttpSource(CURL* curl, std::string url, const std::string& token, std::function<bool()> stopped)
    : curl_(curl), url_(std::move(url)), stopped_(std::move(stopped)) {
  headers_ = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
  uint8_t* buf = static_cast<uint8_t*>(av_malloc(64 << 10));
  avio_ = avio_alloc_context(buf, 64 << 10, 0, this, readPacket, nullptr, seekTo);
}

HttpSource::~HttpSource() {
  if (avio_) {
    av_freep(&avio_->buffer);
    avio_context_free(&avio_);
  }
  curl_slist_free_all(headers_);
}

bool HttpSource::fill(int64_t at) {
  int64_t giveUp = nowMs() + 20000;
  for (int attempt = 0;; attempt++) {
    if (stopped_()) return false;
    std::vector<uint8_t> body;
    body.reserve(kChunk);
    Transfer t{&body, -1, &stopped_};
    char range[64];
    snprintf(range, sizeof(range), "%lld-%lld", (long long)at, (long long)(at + kChunk - 1));
    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
    curl_easy_setopt(curl_, CURLOPT_RANGE, range);
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, net::userAgent());
    net::trust(curl_, wideRoots_);
    curl_easy_setopt(curl_, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 1L);  // a transfer stalled for 15 s is given up
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 15L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(curl_);
    long status = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
    if (rc == CURLE_OK && (status == 206 || status == 200)) {
      if (t.total >= 0) size_ = t.total;
      else if (status == 200) size_ = int64_t(body.size());  // the whole file at once
      if (status == 200 && at > 0) {  // no ranges: the part asked for, out of the whole
        if (at >= int64_t(body.size())) return false;
        body.erase(body.begin(), body.begin() + at);
      }
      chunk_ = std::move(body);
      chunkAt_ = at;
      return true;
    }
    if (rc == CURLE_OK && status == 416) {  // past the end
      chunk_.clear();
      chunkAt_ = at;
      if (size_ < 0) size_ = at;
      return true;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK || stopped_()) return false;
    if (rc == CURLE_PEER_FAILED_VERIFICATION && !wideRoots_) wideRoots_ = true;  // the system's roots too
    // 4xx other than an expired link (the server answers 403 once and resolves the song again): no use
    if (rc == CURLE_OK && status >= 400 && status < 500 && status != 403 && status != 408 && status != 429) return false;
    if (nowMs() > giveUp) return false;
    usleep(net::retryable(rc) ? 250000 : useconds_t(std::min(2000, 300 * (attempt + 1))) * 1000);
  }
}

int HttpSource::readPacket(void* self, uint8_t* buf, int size) {
  auto* s = static_cast<HttpSource*>(self);
  if (s->size_ >= 0 && s->pos_ >= s->size_) return AVERROR_EOF;
  if (s->chunkAt_ < 0 || s->pos_ < s->chunkAt_ || s->pos_ >= s->chunkAt_ + int64_t(s->chunk_.size())) {
    if (!s->fill(s->pos_)) return s->stopped_() ? AVERROR_EXIT : AVERROR(EIO);
    if (s->chunk_.empty()) return AVERROR_EOF;
  }
  int64_t off = s->pos_ - s->chunkAt_;
  int n = int(std::min<int64_t>(size, int64_t(s->chunk_.size()) - off));
  memcpy(buf, s->chunk_.data() + off, size_t(n));
  s->pos_ += n;
  return n;
}

int64_t HttpSource::seekTo(void* self, int64_t offset, int whence) {
  auto* s = static_cast<HttpSource*>(self);
  if (whence & AVSEEK_SIZE) {
    if (s->size_ < 0 && !s->fill(0)) return -1;
    return s->size_;
  }
  whence &= ~AVSEEK_FORCE;
  int64_t target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? s->pos_ + offset : s->size_ >= 0 ? s->size_ + offset : -1;
  if (target < 0) return -1;
  s->pos_ = target;
  return target;
}

}  // namespace rondo
