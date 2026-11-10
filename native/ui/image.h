// Pictures: JPEG (libjpeg-turbo, decoded straight at 1/2 to 1/8 scale when that still covers the
// target) and PNG, scaled with an area filter to cover the target size, center-cropped, as RGBA.
//
// Decoding runs in a helper process with its own address space. Measured in N1: page faults and large
// allocations on threads of the render process hold its memory-map lock, which the Mali driver needs at
// the start of every frame, and frames then waited 50 ms to 2 s. In another process the same work costs
// the render process nothing. The pixels come back through shared memory mapped once at startup.
#pragma once
#include <stdint.h>
#include <sys/types.h>

#include <string>
#include <vector>

namespace rondo::ui {

struct Pixels {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

bool decodeImage(const uint8_t* data, size_t size, int w, int h, Pixels& out);
// A JPEG's or PNG's size without decoding it.
bool imageSize(const uint8_t* data, size_t size, int& w, int& h);
// RGBA as a JPEG (the dashboard's artwork; 1.0.x compressed at 85).
bool encodeJpeg(const Pixels& px, int quality, std::vector<uint8_t>& out);
// A still from a video file at atUs, as RGBA at its own size: the platform's (on the Y1, Android's
// MediaMetadataRetriever, as 1.0.x's ImageLoader used it). Set before the loader starts.
// 1: a frame, 0: none, -1: not this time (ask again later)
using FrameGrabber = int (*)(const char* path, int64_t atUs, Pixels& out);
// prepare: run on the decoding process's main thread as it starts (prepareVideoFrames on the Y1)
void setFrameGrabber(FrameGrabber grabber, void (*prepare)() = nullptr);
// a video's frame at 10 s through that grabber, given up after 8 s (mediaserver's retriever can hang);
// *later: it failed for now (a hung or unreadable answer), not for good
bool grabVideoFrameWithin(const std::string& path, Pixels& out, bool* later = nullptr);
// A box blur repeated `passes` times (3 is close to a Gaussian), in place.
void blur(Pixels& p, int radius, int passes);

// The newest request is served first: on a list that is what is on screen.
class ImageLoader {
 public:
  static constexpr int kMaxSide = 202;  // a result's w x h x 4 fits one shared slot (160 KB)

  struct Request {
    uint64_t id;
    std::string path;  // a music file (its embedded cover) or a .jpg/.png file
    int w, h;          // at most kMaxSide x kMaxSide... in pixels: w * h <= kMaxSide^2
    int blurRadius;    // 0: none
  };
  struct Result {
    uint64_t id;
    bool ok;
    bool dropped;  // never decoded (the queue was full or cancelled): ask again when still wanted
    int w, h;
    int slot;             // hand back with release() once uploaded
    const uint8_t* rgba;  // in shared memory, valid until release()
  };

  // Forks the decoding process: call before any other thread exists. cacheDir holds the thumbnail cache
  // (null: none).
  bool start(int threads, const char* cacheDir);
  bool alive();  // false once rondo-media has died
  // False when the helper's queue is full right now: ask again later.
  bool request(const Request& r);
  // Decodes into the cache only, at low priority and only while the UI is still (no reply).
  bool prefetch(const Request& r);
  // The server artwork ("/thumb?…" paths) comes from: its base URL and the device's token.
  void configure(const std::string& baseUrl, const std::string& token);
  // Tells the helper the UI moved at `now`: prefetching waits until it has been still 1.5 s.
  void active(int64_t now);
  void cancelAll();
  bool poll(Result& out);
  void release(const Result& r);
  size_t pending() const { return pending_; }
  // Readable when a result is waiting (for poll() while idle).
  int waitFd() const { return sock_; }
  void acknowledge() {}

 private:
  int sock_ = -1;
  uint8_t* shm_ = nullptr;
  size_t pending_ = 0;
  pid_t child_ = -1;
};

}  // namespace rondo::ui
