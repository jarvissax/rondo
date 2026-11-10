#include "image.h"

#include <png.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <atomic>
#include <deque>
#include <string>
#include <memory>
#include <turbojpeg.h>
#include <fcntl.h>
#include <unistd.h>

#include "../core/clock.h"

#include <algorithm>

#include "../core/api.h"
#include "../core/cover.h"

namespace rondo::ui {

namespace {
FrameGrabber g_grabber = nullptr;
void (*g_grabberPrepare)() = nullptr;
}
void setFrameGrabber(FrameGrabber grabber, void (*prepare)()) { g_grabber = grabber, g_grabberPrepare = prepare; }

namespace {

// A card video's frame, from a thread of its own with a time limit. *timedOut: no answer (it hung, or the last
// one still hangs), which says nothing about the file: not to be remembered as "no picture". mediaserver's retriever can keep
// a call waiting for good (the 2.0 firmware, Nov 9: both decode workers sat in it with two videos
// open, and no artwork came for anything after); a grab that doesn't answer in 8 s is given up, and
// while it hangs no other is started, so the workers keep decoding everything else. Grabs take turns:
// one waits up to 8 s for the one before it (Nov 10: three videos' frames given up at once while the
// first waited for drmserver to start).
struct GrabJob {
  std::string path;
  Pixels frame;
  bool ok = false, later = false, done = false, abandoned = false;
};
pthread_mutex_t g_grabMu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_grabCv = PTHREAD_COND_INITIALIZER;
bool g_grabBusy = false;  // a grab is running (perhaps hung)

bool grabWithin(const std::string& path, Pixels& out, bool* timedOut = nullptr) {
  if (timedOut) *timedOut = false;
  pthread_mutex_lock(&g_grabMu);
  int64_t turn = now_ns() + 8000 * kMs;
  while (g_grabBusy && now_ns() < turn) waitUntil(&g_grabCv, &g_grabMu, turn);
  if (g_grabBusy) {  // the one before still hasn't come back: no frame for now
    pthread_mutex_unlock(&g_grabMu);
    if (timedOut) *timedOut = true;
    return false;
  }
  g_grabBusy = true;
  pthread_mutex_unlock(&g_grabMu);
  GrabJob* job = new GrabJob;
  job->path = path;
  pthread_t t;
  pthread_create(&t, nullptr, [](void* p) -> void* {
    GrabJob* j = static_cast<GrabJob*>(p);
    int got = g_grabber(j->path.c_str(), 10000000, j->frame);
    pthread_mutex_lock(&g_grabMu);
    j->ok = got > 0, j->later = got < 0, j->done = true;
    g_grabBusy = false;
    bool gone = j->abandoned;
    pthread_cond_broadcast(&g_grabCv);
    pthread_mutex_unlock(&g_grabMu);
    if (gone) delete j;
    return nullptr;
  }, job);
  pthread_detach(t);
  pthread_mutex_lock(&g_grabMu);
  int64_t until = now_ns() + 8000 * kMs;
  while (!job->done && now_ns() < until) waitUntil(&g_grabCv, &g_grabMu, until);
  bool ok = job->done && job->ok;
  if ((!job->done || job->later) && timedOut) *timedOut = true;
  if (ok) out = std::move(job->frame);
  if (job->done) delete job;
  else job->abandoned = true;  // its thread frees it, if it ever returns
  pthread_mutex_unlock(&g_grabMu);
  return ok;
}

}  // namespace

bool grabVideoFrameWithin(const std::string& path, Pixels& out, bool* later) {
  if (later) *later = false;
  return g_grabber && grabWithin(path, out, later);
}

namespace {

// Area-filtered resample of the crop (cx, cy, cw, ch) of src into dst, 4 channels: each output pixel
// is the exact average of the source area it covers.
void resample(const uint8_t* src, int sw, int sh, float cx, float cy, float cw, float ch, uint8_t* dst, int dw,
              int dh) {
  std::vector<float> tmp(size_t(dw) * sh * 4, 0.0f);
  float sx = cw / dw;
  for (int x = 0; x < dw; x++) {
    float a = cx + x * sx, b = a + sx;
    int i0 = int(a), i1 = std::min(sw - 1, int(b - 1e-4f));
    for (int i = i0; i <= i1; i++) {
      float w = std::min(b, float(i + 1)) - std::max(a, float(i));
      if (w <= 0) continue;
      for (int y = 0; y < sh; y++) {
        const uint8_t* s = src + (size_t(y) * sw + i) * 4;
        float* t = &tmp[(size_t(y) * dw + x) * 4];
        t[0] += s[0] * w, t[1] += s[1] * w, t[2] += s[2] * w, t[3] += s[3] * w;
      }
    }
  }
  float sy = ch / dh;
  float norm = 1.0f / (sx * sy);
  for (int y = 0; y < dh; y++) {
    float a = cy + y * sy, b = a + sy;
    int j0 = int(a), j1 = std::min(sh - 1, int(b - 1e-4f));
    for (int x = 0; x < dw; x++) {
      float acc[4] = {0, 0, 0, 0};
      for (int j = j0; j <= j1; j++) {
        float w = std::min(b, float(j + 1)) - std::max(a, float(j));
        if (w <= 0) continue;
        const float* t = &tmp[(size_t(j) * dw + x) * 4];
        for (int c = 0; c < 4; c++) acc[c] += t[c] * w;
      }
      uint8_t* d = dst + (size_t(y) * dw + x) * 4;
      for (int c = 0; c < 4; c++) d[c] = uint8_t(std::min(255.0f, acc[c] * norm + 0.5f));
    }
  }
}

bool fit(const uint8_t* src, int sw, int sh, int w, int h, Pixels& out) {
  // cover: the largest centered crop with the target's aspect ratio
  float aspect = float(w) / h;
  float cw = float(sw), ch = float(sh);
  if (cw / ch > aspect) cw = ch * aspect;
  else ch = cw / aspect;
  out.w = w;
  out.h = h;
  out.rgba.resize(size_t(w) * h * 4);
  resample(src, sw, sh, (sw - cw) / 2, (sh - ch) / 2, cw, ch, out.rgba.data(), w, h);
  return true;
}

bool jpeg(const uint8_t* data, size_t size, int w, int h, Pixels& out) {
  tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
  if (!tj) return false;
  bool ok = false;
  if (tj3DecompressHeader(tj, data, size) == 0) {
    int iw = tj3Get(tj, TJPARAM_JPEGWIDTH), ih = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    int n = 0;
    tjscalingfactor* f = tj3GetScalingFactors(&n);
    tjscalingfactor best = {1, 1};
    for (int i = 0; i < n; i++) {  // the smallest decode that still covers the target
      int sw = TJSCALED(iw, f[i]), sh = TJSCALED(ih, f[i]);
      if (sw >= w && sh >= h && sw * sh < TJSCALED(iw, best) * TJSCALED(ih, best)) best = f[i];
    }
    tj3SetScalingFactor(tj, best);
    int sw = TJSCALED(iw, best), sh = TJSCALED(ih, best);
    std::vector<uint8_t> px(size_t(sw) * sh * 4);
    if (tj3Decompress8(tj, data, size, px.data(), 0, TJPF_RGBA) == 0) ok = fit(px.data(), sw, sh, w, h, out);
  }
  tj3Destroy(tj);
  return ok;
}

bool png(const uint8_t* data, size_t size, int w, int h, Pixels& out) {
  png_image img;
  memset(&img, 0, sizeof(img));
  img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&img, data, size)) return false;
  img.format = PNG_FORMAT_RGBA;
  std::vector<uint8_t> px(PNG_IMAGE_SIZE(img));
  if (!png_image_finish_read(&img, nullptr, px.data(), 0, nullptr)) return false;
  return fit(px.data(), int(img.width), int(img.height), w, h, out);
}

}  // namespace

bool imageSize(const uint8_t* data, size_t size, int& w, int& h) {
  if (size > 3 && data[0] == 0xff && data[1] == 0xd8) {
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    bool ok = tj && tj3DecompressHeader(tj, data, size) == 0;
    if (ok) w = tj3Get(tj, TJPARAM_JPEGWIDTH), h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    tj3Destroy(tj);
    return ok;
  }
  if (size > 24 && !memcmp(data, "\x89PNG", 4)) {  // IHDR: width and height, big-endian, at 16
    w = int(uint32_t(data[16]) << 24 | data[17] << 16 | data[18] << 8 | data[19]);
    h = int(uint32_t(data[20]) << 24 | data[21] << 16 | data[22] << 8 | data[23]);
    return w > 0 && h > 0;
  }
  return false;
}

bool encodeJpeg(const Pixels& px, int quality, std::vector<uint8_t>& out) {
  tjhandle tj = tj3Init(TJINIT_COMPRESS);
  if (!tj) return false;
  tj3Set(tj, TJPARAM_QUALITY, quality);
  tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);
  unsigned char* buf = nullptr;
  size_t len = 0;
  bool ok = tj3Compress8(tj, px.rgba.data(), px.w, 0, px.h, TJPF_RGBA, &buf, &len) == 0;
  if (ok) out.assign(buf, buf + len);
  tj3Free(buf);
  tj3Destroy(tj);
  return ok;
}

bool decodeImage(const uint8_t* data, size_t size, int w, int h, Pixels& out) {
  if (size > 3 && data[0] == 0xff && data[1] == 0xd8) return jpeg(data, size, w, h, out);
  if (size > 8 && !memcmp(data, "\x89PNG", 4)) return png(data, size, w, h, out);
  return false;
}

void blur(Pixels& p, int r, int passes) {
  std::vector<uint8_t> tmp(p.rgba.size());
  int w = p.w, h = p.h;
  auto pass = [&](const uint8_t* src, uint8_t* dst, int len, int count, int step, int stride) {
    for (int k = 0; k < count; k++) {
      const uint8_t* s = src + size_t(k) * stride;
      uint8_t* d = dst + size_t(k) * stride;
      for (int c = 0; c < 4; c++) {
        int acc = 0;
        for (int i = -r; i <= r; i++) acc += s[std::clamp(i, 0, len - 1) * step + c];
        for (int i = 0; i < len; i++) {
          d[i * step + c] = uint8_t(acc / (2 * r + 1));
          acc += s[std::min(i + r + 1, len - 1) * step + c] - s[std::max(i - r, 0) * step + c];
        }
      }
    }
  };
  for (int i = 0; i < passes; i++) {
    pass(p.rgba.data(), tmp.data(), w, h, 4, w * 4);  // rows
    pass(tmp.data(), p.rgba.data(), h, w, w * 4, 4);  // columns
  }
}

namespace {

constexpr int kSlots = 32;
constexpr size_t kSlotBytes = 160 << 10;
constexpr size_t kControlBytes = 4096;  // the shared block before the slots

enum MsgType : uint32_t { kRequest = 1, kCancel, kRelease, kResult, kPrefetch, kConfigure };

struct Msg {
  uint32_t type;
  int32_t slot;
  uint64_t id;
  int32_t w, h, blur;
  int32_t ok, dropped;
  char path[512];
};

// Written by the UI process, read by the helper.
struct Control {
  volatile int64_t lastActivity;  // CLOCK_MONOTONIC ns of the last frame that moved
};

int64_t monoNs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

uint64_t fnv64(const char* s) {
  uint64_t h = 1469598103934665603ULL;
  for (; *s; s++) h = (h ^ uint8_t(*s)) * 1099511628211ULL;
  return h;
}

// A cached thumbnail: this header, then w * h * 4 bytes of RGBA. w = h = 0 records a file with no
// picture, so it is not read again.
struct CacheHeader {
  char magic[4];  // "RTH2" (2: video frames and folder pictures, Nov 7); "RTM3": no picture (Nov 10)
  int32_t w, h, blur;
  int64_t mtime, size;  // of the source file, to notice a changed file
};

// The decoding process: a receiver thread queues requests, workers decode into free shared slots.
// Prefetches only fill the thumbnail cache, and only while the UI has been still for a while, so a
// full-size decode never competes with interaction.
struct Worker {
  int sock;
  uint8_t* shm;
  const Control* control;
  std::string cacheDir;
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
  std::deque<Msg> queue, prefetch;
  std::vector<int> freeSlots;
  // the server's /thumb artwork: one API handle per worker (libcurl's setup isn't thread-safe, so they
  // are made before the threads), configured by kConfigure
  std::unique_ptr<rondo::Api> apis[4];
  std::atomic<int> nextApi{0};
  std::string base, token;
  int generation = 0;

  static constexpr int64_t kQuietNs = 1500000000LL;  // still this long before prefetching

  uint8_t* slot(int i) { return shm + kControlBytes + size_t(i) * kSlotBytes; }

  void reply(const Msg& req, bool ok, bool dropped, int slot, int w, int h) {
    Msg m = {};
    m.type = kResult;
    m.id = req.id;
    m.ok = ok;
    m.dropped = dropped;
    m.slot = slot;
    m.w = w;
    m.h = h;
    send(sock, &m, offsetof(Msg, path), 0);
  }

  std::string cachePath(const Msg& r) {
    char name[64];
    snprintf(name, sizeof(name), "/%016llx-%dx%d-%d.rgba", (unsigned long long)fnv64(r.path), r.w, r.h, r.blur);
    return cacheDir + name;
  }

  // From the cache into dst (null: only check it is there). -1: not cached, 0: cached as missing, 1: ok.
  int fromCache(const Msg& r, const struct stat& st, uint8_t* dst) {
    if (cacheDir.empty()) return -1;
    int fd = open(cachePath(r).c_str(), O_RDONLY);
    if (fd < 0) return -1;
    CacheHeader h;
    int result = -1;
    bool marker = false;
    if (read(fd, &h, sizeof(h)) == ssize_t(sizeof(h)) && (!memcmp(h.magic, "RTH2", 4) || (marker = !memcmp(h.magic, "RTM3", 4))) &&
        h.mtime == int64_t(st.st_mtime) && h.size == int64_t(st.st_size)) {
      if (h.w == 0) {
        // an older "no picture" for a video: most were frames that never came (drmserver stopped, Nov 9)
        result = marker || !isVideo(r.path) ? 0 : -1;
      } else if (h.w == r.w && h.h == r.h) {
        size_t n = size_t(h.w) * h.h * 4;
        result = !dst || read(fd, dst, n) == ssize_t(n) ? 1 : -1;
      }
    }
    close(fd);
    return result;
  }

  void toCache(const Msg& r, const struct stat& st, const Pixels* px) {
    if (cacheDir.empty()) return;
    std::string path = cachePath(r), tmp = path + ".part";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    CacheHeader h = {{'R', 'T', px ? 'H' : 'M', px ? '2' : '3'}, px ? px->w : 0, px ? px->h : 0, r.blur, int64_t(st.st_mtime), int64_t(st.st_size)};
    bool ok = write(fd, &h, sizeof(h)) == ssize_t(sizeof(h));
    if (ok && px) ok = write(fd, px->rgba.data(), px->rgba.size()) == ssize_t(px->rgba.size());
    close(fd);
    if (ok) rename(tmp.c_str(), path.c_str());
    else unlink(tmp.c_str());
  }

  // blur < 0: the picture decoded at a size of its own, then scaled to w x h: -blur is that size, square,
  // or width | height << 12 (a video row's 104 x 58: its color is taken from the row's own picture)
  static int srcW(const Msg& r) { return r.blur < 0 ? (-r.blur) & 0xfff : r.w; }
  static int srcH(const Msg& r) { return r.blur < 0 ? ((-r.blur) >> 12 ? (-r.blur) >> 12 : (-r.blur) & 0xfff) : r.h; }
  // 1.0.x's backdrop (ArtColor.blurred): the picture decoded at -blur pixels, then scaled to w x h
  // with bilinear filtering, as Bitmap.createScaledBitmap(bitmap, 6, 6, true) does
  static void bilinearDown(const Pixels& src, int w, int h, Pixels& out) {
    out.w = w, out.h = h;
    out.rgba.assign(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        float sx = (x + 0.5f) * src.w / w - 0.5f, sy = (y + 0.5f) * src.h / h - 0.5f;
        int x0 = std::max(0, int(sx)), y0 = std::max(0, int(sy));
        int x1 = std::min(src.w - 1, x0 + 1), y1 = std::min(src.h - 1, y0 + 1);
        float fx = sx - x0, fy = sy - y0;
        for (int c = 0; c < 4; c++) {
          auto at = [&](int px, int py) { return float(src.rgba[(size_t(py) * src.w + px) * 4 + c]); };
          float v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
          out.rgba[(size_t(y) * w + x) * 4 + c] = uint8_t(std::min(255.f, v + 0.5f));
        }
      }
  }

  static bool remote(const char* path) { return !strncmp(path, "/thumb", 6); }

  // a thumbnail from the server: <base><thumb>&w=<width>&h=<height>, as 1.0.x's ImageLoader asked
  bool fetch(rondo::Api& api, int& apiGeneration, const Msg& r, std::vector<uint8_t>& bytes) {
    pthread_mutex_lock(&mu);
    if (apiGeneration != generation) apiGeneration = generation, api.configure(base, token);
    pthread_mutex_unlock(&mu);
    int w = srcW(r), h = srcH(r);
    std::string body;
    std::string path = std::string(r.path) + "&w=" + std::to_string(w) + "&h=" + std::to_string(h);
    if (api.get(path, body) != 200 || body.empty()) return false;
    bytes.assign(body.begin(), body.end());
    return true;
  }

  // *transient: it failed for now (a video's frame that didn't answer), not for good
  bool decode(const Msg& r, std::vector<uint8_t>& bytes, Pixels& px, rondo::Api* api = nullptr, int* apiGeneration = nullptr,
              bool* transient = nullptr) {
    bytes.clear();
    if (transient) *transient = false;
    std::string path = r.path;
    bool file = path.size() > 4 && (path.compare(path.size() - 4, 4, ".jpg") == 0 ||
                                    path.compare(path.size() - 4, 4, ".png") == 0);
    if (remote(r.path)) {
      if (!api || !fetch(*api, *apiGeneration, r, bytes)) return false;
    } else if (file) {
      if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        bytes.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        fclose(f);
      }
    } else {
      // 1.0.x's order (ImageLoader.local): the embedded picture, a video's frame at 10 s, then a picture
      // next to the file
      rondo::readCover(path.c_str(), bytes);
      if (bytes.empty() && isVideo(path) && g_grabber) {
        Pixels frame;
        bool timedOut = false;
        if (grabWithin(path, frame, &timedOut) && frame.w > 0 && frame.h > 0) return fit(frame, r, px);
        if (timedOut) {
          if (transient) *transient = true;
          return false;
        }
      }
      if (bytes.empty()) folderPicture(path, bytes);
    }
    if (r.blur < 0) {
      Pixels big;
      if (bytes.empty() || !decodeImage(bytes.data(), bytes.size(), srcW(r), srcH(r), big)) return false;
      bilinearDown(big, r.w, r.h, px);
      return true;
    }
    bool ok = !bytes.empty() && size_t(r.w) * r.h * 4 <= kSlotBytes && decodeImage(bytes.data(), bytes.size(), r.w, r.h, px);
    if (ok && r.blur > 0) blur(px, r.blur, 3);
    return ok;
  }

  static bool isVideo(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string x = path.substr(dot + 1);
    for (char& c : x) c = char(tolower((unsigned char)c));
    return x == "mp4" || x == "m4v" || x == "mkv" || x == "3gp" || x == "webm";
  }
  // cover.jpg, folder.jpg (either case) or front.jpg beside the file
  static void folderPicture(const std::string& path, std::vector<uint8_t>& bytes) {
    std::string dir = path.substr(0, path.rfind('/') + 1);
    for (const char* name : {"cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg", "front.jpg"}) {
      FILE* f = fopen((dir + name).c_str(), "rb");
      if (!f) continue;
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      bytes.resize(n > 0 && n < (16 << 20) ? size_t(n) : 0);
      if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
      fclose(f);
      if (!bytes.empty()) return;
    }
  }
  // a video's still as 1.0.x showed it (ImageLoader.local): Bitmap.createScaledBitmap to twice the asked
  // width (bilinear), then the ImageView's center crop, drawn with bilinear filtering. The backdrops
  // (blur) take it the way they take a picture.
  static bool fit(const Pixels& src, const Msg& r, Pixels& px) {
    int w = srcW(r), h = srcH(r);
    if (w <= 0 || h <= 0 || size_t(w) * h * 4 > kSlotBytes * 4) return false;
    if (r.blur != 0) {
      float aspect = float(w) / h, cw = float(src.w), ch = float(src.h);
      if (cw / ch > aspect) cw = ch * aspect;
      else ch = cw / aspect;
      Pixels out;
      out.w = w, out.h = h;
      out.rgba.resize(size_t(w) * h * 4);
      resample(src.rgba.data(), src.w, src.h, (src.w - cw) / 2, (src.h - ch) / 2, cw, ch, out.rgba.data(), w, h);
      if (r.blur < 0) bilinearDown(out, r.w, r.h, px);
      else px = std::move(out), blur(px, r.blur, 3);
      return true;
    }
    if (size_t(w) * h * 4 > kSlotBytes) return false;
    Pixels scaled;
    const Pixels* a = &src;
    if (src.w > w * 2) {
      bilinearDown(src, w * 2, src.h * w * 2 / src.w, scaled);
      a = &scaled;
    }
    // ImageView.CENTER_CROP: the larger scale, the offset rounded to whole pixels
    float s = std::max(float(w) / a->w, float(h) / a->h);
    int dx = int((w - a->w * s) * 0.5f + 0.5f), dy = int((h - a->h * s) * 0.5f + 0.5f);
    px.w = w, px.h = h;
    px.rgba.assign(size_t(w) * h * 4, 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        float sx = (x + 0.5f - dx) / s - 0.5f, sy = (y + 0.5f - dy) / s - 0.5f;
        sx = std::max(0.f, std::min(float(a->w - 1), sx)), sy = std::max(0.f, std::min(float(a->h - 1), sy));
        int x0 = int(sx), y0 = int(sy), x1 = std::min(a->w - 1, x0 + 1), y1 = std::min(a->h - 1, y0 + 1);
        float fx = sx - x0, fy = sy - y0;
        for (int c = 0; c < 4; c++) {
          auto at = [&](int qx, int qy) { return float(a->rgba[(size_t(qy) * a->w + qx) * 4 + c]); };
          float v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
          px.rgba[(size_t(y) * w + x) * 4 + c] = uint8_t(std::min(255.f, v + 0.5f));
        }
      }
    return true;
  }

  bool quiet() const { return monoNs() - control->lastActivity > kQuietNs; }

  void decodeLoop() {
    std::vector<uint8_t> bytes;
    rondo::Api& api = *apis[nextApi++];
    int apiGeneration = -1;
    for (;;) {
      pthread_mutex_lock(&mu);
      Msg r;
      int s = -1;
      for (;;) {
        if (!queue.empty() && !freeSlots.empty()) {
          r = queue.back();  // newest first
          queue.pop_back();
          s = freeSlots.back();
          freeSlots.pop_back();
          break;
        }
        if (!prefetch.empty() && quiet()) {
          r = prefetch.front();
          prefetch.pop_front();
          break;
        }
        if (prefetch.empty()) {
          pthread_cond_wait(&cv, &mu);
        } else {  // check again for quiet in a while
          waitUntil(&cv, &mu, now_ns() + 200 * kMs);
        }
      }
      pthread_mutex_unlock(&mu);
      struct stat st;
      bool exists = stat(r.path, &st) == 0;
      if (remote(r.path)) {
        // the server's artwork: cached on disk once fetched (it doesn't change); a failed fetch (Wi-Fi)
        // is not recorded as "no picture", so it is asked again later
        memset(&st, 0, sizeof(st));
        int cached = r.type == kPrefetch ? fromCache(r, st, nullptr) : fromCache(r, st, slot(s));
        if (cached == 1) {
          if (r.type != kPrefetch) reply(r, true, false, s, r.w, r.h);
          continue;
        }
        Pixels px;
        bool ok = decode(r, bytes, px, &api, &apiGeneration);
        if (ok) toCache(r, st, &px);
        if (r.type == kPrefetch) continue;
        if (ok) {
          memcpy(slot(s), px.rgba.data(), px.rgba.size());
          reply(r, true, false, s, px.w, px.h);
        } else {
          pthread_mutex_lock(&mu);
          freeSlots.push_back(s);
          pthread_mutex_unlock(&mu);
          reply(r, false, false, -1, 0, 0);
        }
        continue;
      }
      if (r.type == kPrefetch) {
        if (exists && fromCache(r, st, nullptr) < 0) {
          Pixels px;
          bool transient = false;
          bool ok = decode(r, bytes, px, nullptr, nullptr, &transient);
          if (ok || !transient) toCache(r, st, ok ? &px : nullptr);
        }
        continue;
      }
      int cached = exists ? fromCache(r, st, slot(s)) : 0;
      if (cached == 1) {
        reply(r, true, false, s, r.w, r.h);
        continue;
      }
      Pixels px;
      bool transient = false;
      bool ok = cached < 0 && exists && decode(r, bytes, px, nullptr, nullptr, &transient);
      if (exists && cached < 0 && (ok || !transient)) toCache(r, st, ok ? &px : nullptr);
      if (ok) {
        memcpy(slot(s), px.rgba.data(), px.rgba.size());
        reply(r, true, false, s, px.w, px.h);
      } else {
        pthread_mutex_lock(&mu);
        freeSlots.push_back(s);
        pthread_mutex_unlock(&mu);
        reply(r, false, false, -1, 0, 0);
      }
    }
  }

  static void* run(void* self) {
    static_cast<Worker*>(self)->decodeLoop();
    return nullptr;
  }

  [[noreturn]] void main(int threads) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    prctl(PR_SET_NAME, "rondo-media");
    if (g_grabberPrepare) g_grabberPrepare();  // on this thread, which lives as long as the process
    // SCHED_IDLE: runs only when nothing else wants the CPU, so the kernel's display and GPU threads (nice
    // 0) always go first. At nice 10 they still lost a refresh about once a minute (N1 log).
    sched_param sp = {};
    if (sched_setscheduler(0, 5 /* SCHED_IDLE */, &sp) != 0) setpriority(PRIO_PROCESS, 0, 19);
    for (int i = 0; i < kSlots; i++) freeSlots.push_back(i);
    threads = std::min(threads, 4);
    for (int i = 0; i < threads; i++) apis[i].reset(new rondo::Api());
    for (int i = 0; i < threads; i++) {
      pthread_t t;
      pthread_create(&t, nullptr, &Worker::run, this);
    }
    Msg m;
    for (;;) {
      ssize_t n = recv(sock, &m, sizeof(m), 0);
      if (n <= 0) _exit(0);  // the render process is gone
      pthread_mutex_lock(&mu);
      m.path[sizeof(m.path) - 1] = 0;
      if (m.type == kRequest) {
        // a fast spin asks for more than the workers decode: the oldest asks are rows long gone
        while (queue.size() >= 48) {
          reply(queue.front(), false, true, -1, 0, 0);
          queue.pop_front();
        }
        queue.push_back(m);
      } else if (m.type == kPrefetch) {
        prefetch.push_back(m);
      } else if (m.type == kCancel) {
        for (const Msg& q : queue) reply(q, false, true, -1, 0, 0);
        queue.clear();
      } else if (m.type == kRelease && m.slot >= 0 && m.slot < kSlots) {
        freeSlots.push_back(m.slot);
      } else if (m.type == kConfigure) {
        std::string s = m.path;
        size_t nl = s.find('\n');
        base = s.substr(0, nl);
        token = nl == std::string::npos ? "" : s.substr(nl + 1);
        generation++;
      }
      pthread_cond_broadcast(&cv);
      pthread_mutex_unlock(&mu);
    }
  }
};

}  // namespace

bool ImageLoader::alive() {
  if (child_ <= 0) return true;
  int status;
  return waitpid(child_, &status, WNOHANG) != child_;
}

bool ImageLoader::start(int threads, const char* cacheDir) {
  size_t bytes = kControlBytes + size_t(kSlots) * kSlotBytes;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return false;
  shm_ = static_cast<uint8_t*>(p);
  memset(shm_, 0, bytes);  // fault every page in now, never during a frame
  if (cacheDir) mkdir(cacheDir, 0700);
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return false;
  int buf = 256 << 10;
  setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  child_ = fork();
  if (child_ < 0) return false;
  if (child_ == 0) {
    close(sv[0]);
    Worker* w = new Worker;
    w->sock = sv[1];
    w->shm = shm_;
    w->control = reinterpret_cast<const Control*>(shm_);
    if (cacheDir) w->cacheDir = cacheDir;
    w->main(threads);
  }
  close(sv[1]);
  sock_ = sv[0];
  fcntl(sock_, F_SETFL, O_NONBLOCK);
  return true;
}

void ImageLoader::active(int64_t now) { reinterpret_cast<Control*>(shm_)->lastActivity = now; }

namespace {
bool sendRequest(int sock, uint32_t type, const ImageLoader::Request& r) {
  Msg m = {};
  m.type = type;
  m.id = r.id;
  m.w = r.w;
  m.h = r.h;
  m.blur = r.blurRadius;
  m.slot = -1;
  strncpy(m.path, r.path.c_str(), sizeof(m.path) - 1);
  size_t len = offsetof(Msg, path) + strlen(m.path) + 1;
  return send(sock, &m, len, MSG_DONTWAIT) == ssize_t(len);
}
}  // namespace

bool ImageLoader::request(const Request& r) {
  if (!sendRequest(sock_, kRequest, r)) return false;
  pending_++;
  return true;
}

bool ImageLoader::prefetch(const Request& r) { return sendRequest(sock_, kPrefetch, r); }

void ImageLoader::configure(const std::string& baseUrl, const std::string& token) {
  Request r{0, baseUrl + "\n" + token, 0, 0, 0};
  sendRequest(sock_, kConfigure, r);
}

void ImageLoader::cancelAll() {
  Msg m = {};
  m.type = kCancel;
  send(sock_, &m, offsetof(Msg, path), MSG_DONTWAIT);
}

bool ImageLoader::poll(Result& out) {
  Msg m;
  ssize_t n = recv(sock_, &m, sizeof(m), MSG_DONTWAIT);
  if (n < ssize_t(offsetof(Msg, path)) || m.type != kResult) return false;
  if (pending_) pending_--;
  out.id = m.id;
  out.ok = m.ok != 0;
  out.dropped = m.dropped != 0;
  out.w = m.w;
  out.h = m.h;
  out.slot = m.slot;
  out.rgba = m.slot >= 0 ? shm_ + kControlBytes + size_t(m.slot) * kSlotBytes : nullptr;
  return true;
}

void ImageLoader::release(const Result& r) {
  if (r.slot < 0) return;
  Msg m = {};
  m.type = kRelease;
  m.slot = r.slot;
  send(sock_, &m, offsetof(Msg, path), MSG_DONTWAIT);
}

}  // namespace rondo::ui
