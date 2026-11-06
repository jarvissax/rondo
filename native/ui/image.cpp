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
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <deque>
#include <turbojpeg.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

#include "../core/cover.h"

namespace rondo::ui {
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

enum MsgType : uint32_t { kRequest = 1, kCancel, kRelease, kResult, kPrefetch };

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
  char magic[4];  // "RTH1"
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
    if (read(fd, &h, sizeof(h)) == ssize_t(sizeof(h)) && !memcmp(h.magic, "RTH1", 4) && h.mtime == int64_t(st.st_mtime) &&
        h.size == int64_t(st.st_size)) {
      if (h.w == 0) {
        result = 0;
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
    CacheHeader h = {{'R', 'T', 'H', '1'}, px ? px->w : 0, px ? px->h : 0, r.blur, int64_t(st.st_mtime), int64_t(st.st_size)};
    bool ok = write(fd, &h, sizeof(h)) == ssize_t(sizeof(h));
    if (ok && px) ok = write(fd, px->rgba.data(), px->rgba.size()) == ssize_t(px->rgba.size());
    close(fd);
    if (ok) rename(tmp.c_str(), path.c_str());
    else unlink(tmp.c_str());
  }

  // blur < 0: 1.0.x's backdrop (ArtColor.blurred): the picture decoded at -blur pixels, then scaled to w x h
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

  bool decode(const Msg& r, std::vector<uint8_t>& bytes, Pixels& px) {
    bytes.clear();
    std::string path = r.path;
    bool file = path.size() > 4 && (path.compare(path.size() - 4, 4, ".jpg") == 0 ||
                                    path.compare(path.size() - 4, 4, ".png") == 0);
    if (file) {
      if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        bytes.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        fclose(f);
      }
    } else {
      rondo::readCover(path.c_str(), bytes);
    }
    if (r.blur < 0) {
      Pixels big;
      if (bytes.empty() || !decodeImage(bytes.data(), bytes.size(), -r.blur, -r.blur, big)) return false;
      bilinearDown(big, r.w, r.h, px);
      return true;
    }
    bool ok = !bytes.empty() && size_t(r.w) * r.h * 4 <= kSlotBytes && decodeImage(bytes.data(), bytes.size(), r.w, r.h, px);
    if (ok && r.blur > 0) blur(px, r.blur, 3);
    return ok;
  }

  bool quiet() const { return monoNs() - control->lastActivity > kQuietNs; }

  void decodeLoop() {
    std::vector<uint8_t> bytes;
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
          timespec until;
          clock_gettime(CLOCK_REALTIME, &until);
          until.tv_nsec += 200000000;
          if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
          pthread_cond_timedwait(&cv, &mu, &until);
        }
      }
      pthread_mutex_unlock(&mu);
      struct stat st;
      bool exists = stat(r.path, &st) == 0;
      if (r.type == kPrefetch) {
        if (exists && fromCache(r, st, nullptr) < 0) {
          Pixels px;
          bool ok = decode(r, bytes, px);
          toCache(r, st, ok ? &px : nullptr);
        }
        continue;
      }
      int cached = exists ? fromCache(r, st, slot(s)) : 0;
      if (cached == 1) {
        reply(r, true, false, s, r.w, r.h);
        continue;
      }
      Pixels px;
      bool ok = cached < 0 && exists && decode(r, bytes, px);
      if (exists && cached < 0) toCache(r, st, ok ? &px : nullptr);
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
    // SCHED_IDLE: runs only when nothing else wants the CPU, so the kernel's display and GPU threads (nice
    // 0) always go first. At nice 10 they still lost a refresh about once a minute (N1 log).
    sched_param sp = {};
    if (sched_setscheduler(0, 5 /* SCHED_IDLE */, &sp) != 0) setpriority(PRIO_PROCESS, 0, 19);
    for (int i = 0; i < kSlots; i++) freeSlots.push_back(i);
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
      }
      pthread_cond_broadcast(&cv);
      pthread_mutex_unlock(&mu);
    }
  }
};

}  // namespace

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
