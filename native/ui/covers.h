// Pictures on the GPU by file path and size: the embedded cover of a music file or a .jpg/.png, asked
// of rondo-media (image.h) when first drawn, faded in over 150 ms when it arrives, kept in an LRU
// texture budget, and prefetched into the thumbnail cache while nothing moves.
#pragma once
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "image.h"

namespace rondo::ui {

class Covers {
 public:
  explicit Covers(ImageLoader& loader) : loader_(loader) {}

  // The picture at w x h (blurRadius > 0: blurred), or null while it loads (or the file has none).
  // alpha: its fade-in. The server's artwork that didn't come is asked again after retryNs.
  const Texture* get(const std::string& path, int w, int h, int blurRadius, int64_t now, float* alpha,
                     int64_t retryNs = 15 * 1000000000LL);
  // A small picture kept on the CPU instead (w * h <= 256: a color sample), RGBA, or null while it loads.
  const uint8_t* pixels(const std::string& path, int w, int h, int blurRadius);
  // Queues the picture for the thumbnail cache, filled while the UI is still.
  void prefetch(const std::string& path, int w, int h, int blurRadius);
  // Hands queued prefetches to the helper as its queue has room; true while some are left.
  bool sendPrefetches();
  // Uploads up to `max` decoded pictures; true when more wait or one is fading in.
  bool pump(int64_t now, int max);
  // Makes textures of this size ahead of need (creating one in a frame costs the driver an allocation).
  void reserve(int w, int h, int n);
  // Tops the reserved sizes back up, one texture a call (while idle): true while some are short.
  bool topUp();
  size_t bytes() const { return bytes_; }
  int uploadedLast() const { return uploadedLast_; }

 private:
  enum State : uint8_t { kNone, kLoading, kReady, kMissing };
  struct Entry {
    std::string path;
    int w = 0, h = 0, blur = 0;
    State state = kNone;
    Texture tex;
    int64_t readyAt = 0;
    uint32_t used = 0;
    size_t bytes = 0;
    bool cpuOnly = false;
    uint8_t tries = 0;  // remote artwork that didn't come: asked again sooner at first
    std::vector<uint8_t> cpu;
  };
  struct Ask {
    std::string path;
    int w, h, blur;
  };
  static uint64_t keyOf(const std::string& path, int w, int h, int blur);
  Texture take(int w, int h);
  void evict();

  ImageLoader& loader_;
  std::unordered_map<uint64_t, Entry> map_;
  std::unordered_map<uint32_t, std::vector<Texture>> pool_;
  std::unordered_map<uint32_t, int> reserved_;  // the size's spare count to keep
  std::vector<Ask> prefetch_;
  size_t sent_ = 0;
  uint32_t clock_ = 0;
  size_t bytes_ = 0;
  bool fading_ = false;
  int uploadedLast_ = 0;
};

}  // namespace rondo::ui
