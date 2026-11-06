#include "covers.h"

#include <GLES2/gl2.h>

#include "motion.h"

namespace rondo::ui {

uint64_t Covers::keyOf(const std::string& path, int w, int h, int blur) {
  uint64_t k = 1469598103934665603ULL;
  for (char c : path) k = (k ^ uint8_t(c)) * 1099511628211ULL;
  return k ^ (uint64_t(w) << 40) ^ (uint64_t(h) << 24) ^ uint64_t(blur) * 0x9e3779b97f4a7c15ULL;
}

const Texture* Covers::get(const std::string& path, int w, int h, int blur, int64_t now, float* alpha) {
  uint64_t key = keyOf(path, w, h, blur);
  Entry& e = map_[key];
  e.used = ++clock_;
  if (e.state == kNone) {
    e.path = path, e.w = w, e.h = h, e.blur = blur;
    if (loader_.request({key, path, w, h, blur})) e.state = kLoading;
  }
  if (e.state != kReady) return nullptr;
  float a = clamp01(float(now - e.readyAt) / 150e6f);
  if (a < 1) fading_ = true;
  if (alpha) *alpha = a;
  return &e.tex;
}

void Covers::prefetch(const std::string& path, int w, int h, int blur) { prefetch_.push_back({path, w, h, blur}); }

bool Covers::sendPrefetches() {
  for (int n = 0; n < 64 && sent_ < prefetch_.size(); n++, sent_++) {
    const Ask& a = prefetch_[sent_];
    if (!loader_.prefetch({keyOf(a.path, a.w, a.h, a.blur), a.path, a.w, a.h, a.blur})) break;
  }
  return sent_ < prefetch_.size();
}

Texture Covers::take(int w, int h) {
  auto& p = pool_[uint32_t(w) << 16 | uint32_t(h)];
  if (p.empty()) return createTexture(w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, true);
  Texture t = p.back();
  p.pop_back();
  return t;
}

void Covers::reserve(int w, int h, int n) {
  auto& p = pool_[uint32_t(w) << 16 | uint32_t(h)];
  while (int(p.size()) < n) p.push_back(createTexture(w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, true));
}

bool Covers::pump(int64_t now, int max) {
  ImageLoader::Result r;
  int uploaded = 0;
  while (uploaded < max && loader_.poll(r)) {
    auto it = map_.find(r.id);
    if (it == map_.end()) {
      loader_.release(r);
      continue;
    }
    Entry& e = it->second;
    if (r.dropped) {
      e.state = kNone;
      continue;
    }
    if (!r.ok) {
      e.state = kMissing;
      continue;
    }
    e.tex = take(r.w, r.h);
    glBindTexture(GL_TEXTURE_2D, e.tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, r.rgba);
    loader_.release(r);
    e.bytes = size_t(r.w) * r.h * 4;
    bytes_ += e.bytes;
    e.state = kReady;
    e.readyAt = now;
    uploaded++;
  }
  evict();
  bool more = fading_ || uploaded == max;
  fading_ = false;
  uploadedLast_ = uploaded;
  return more;
}

void Covers::evict() {
  const size_t kBudget = 20u << 20;  // of the 24 MB texture budget
  while (bytes_ > kBudget) {
    auto oldest = map_.end();
    for (auto it = map_.begin(); it != map_.end(); ++it)
      if (it->second.state == kReady && (oldest == map_.end() || it->second.used < oldest->second.used)) oldest = it;
    if (oldest == map_.end()) break;
    bytes_ -= oldest->second.bytes;
    pool_[uint32_t(oldest->second.tex.w) << 16 | uint32_t(oldest->second.tex.h)].push_back(oldest->second.tex);
    map_.erase(oldest);
  }
}

}  // namespace rondo::ui
