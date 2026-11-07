// grabVideoFrame (video_frame.h) on the Y1: Android's native MediaMetadataRetriever, which has
// mediaserver decode the frame (MediaTek's hardware decoder) and hands it back, as the JNI behind
// MediaMetadataRetriever.getFrameAtTime() takes it: the frame's pixels follow its VideoFrame header.
#include "../video_frame.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string.h>

#include <algorithm>

#include "../aosp_begin.h"
#include <binder/IMemory.h>
#include <media/mediametadataretriever.h>
#include <private/media/VideoFrame.h>
#include "../aosp_end.h"

namespace rondo {

namespace {
template <typename T, typename... A>
T* roomy(A&&... a) {  // room to spare for MediaTek's larger classes (as in s2)
  return new (::operator new(sizeof(T) + 256)) T(static_cast<A&&>(a)...);
}
}  // namespace

bool grabVideoFrame(const char* path, int64_t atUs, ui::Pixels& out) {
  using namespace android;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  fstat(fd, &st);
  sp<MediaMetadataRetriever> r = roomy<MediaMetadataRetriever>();
  bool ok = false;
  if (r->setDataSource(fd, 0, st.st_size) == OK) {
    sp<IMemory> mem = r->getFrameAtTime(atUs, 2 /* OPTION_CLOSEST_SYNC, getFrameAtTime(us)'s */);
    if (mem != nullptr && mem->size() >= sizeof(VideoFrame)) {
      const VideoFrame* f = static_cast<const VideoFrame*>(mem->pointer());
      const uint8_t* data = reinterpret_cast<const uint8_t*>(f) + sizeof(VideoFrame);
      int w = int(f->mWidth), h = int(f->mHeight);
      size_t n = size_t(w) * h;
      // stock Android hands RGB565; MediaTek's mediaserver hands 32-bit frames (Skia's ARGB_8888: R, G, B, A)
      int bpp = n ? int(f->mSize / n) : 0;
      if (w > 0 && h > 0 && (bpp == 2 || bpp == 4) && mem->size() >= sizeof(VideoFrame) + n * bpp) {
        out.w = w, out.h = h;
        out.rgba.resize(n * 4);
        if (bpp == 4) {
          memcpy(out.rgba.data(), data, n * 4);
          for (size_t i = 0; i < n; i++) out.rgba[i * 4 + 3] = 255;
        } else {
          const uint16_t* p = reinterpret_cast<const uint16_t*>(data);
          for (size_t i = 0; i < n; i++) {
            uint16_t c = p[i];
            uint8_t* o = &out.rgba[i * 4];
            o[0] = uint8_t(((c >> 11) & 0x1f) * 255 / 31);
            o[1] = uint8_t(((c >> 5) & 0x3f) * 255 / 63);
            o[2] = uint8_t((c & 0x1f) * 255 / 31);
            o[3] = 255;
          }
        }
        ok = true;
      }
    }
  }
  r->disconnect();
  close(fd);
  return ok;
}

}  // namespace rondo
