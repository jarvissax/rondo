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

  // Forks the decoding process: call before any other thread exists.
  bool start(int threads = 2);
  void request(const Request& r);
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
