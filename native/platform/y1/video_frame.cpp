// grabVideoFrame (video_frame.h) on the Y1: Android's native MediaMetadataRetriever, which has
// mediaserver decode the frame (MediaTek's hardware decoder) and hands it back, as the JNI behind
// MediaMetadataRetriever.getFrameAtTime() takes it: the frame's pixels follow its VideoFrame header.
#include "../video_frame.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string.h>

#include <algorithm>

#include "../aosp_begin.h"
#include <binder/IMemory.h>
#include <binder/ProcessState.h>
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

void prepareVideoFrames() { android::ProcessState::self(); }

int grabVideoFrame(const char* path, int64_t atUs, ui::Pixels& out) {
  using namespace android;
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  struct stat st;
  fstat(fd, &st);
  sp<MediaMetadataRetriever> r = roomy<MediaMetadataRetriever>();
  int ok = 0;
  status_t set = r->setDataSource(fd, 0, st.st_size);
  if (set != OK) fprintf(stderr, "rondo-media: no frame of %s: setDataSource %d\n", path, int(set));
  if (set == OK) {
    sp<IMemory> mem = r->getFrameAtTime(atUs, 2 /* OPTION_CLOSEST_SYNC, getFrameAtTime(us)'s */);
    if (mem == nullptr && atUs > 0) {  // a clip shorter than that: a frame a third of the way in
      const char* d = r->extractMetadata(METADATA_KEY_DURATION);
      int64_t ms = d ? atoll(d) : 0;
      if (ms > 0 && ms * 1000 < atUs) mem = r->getFrameAtTime(ms * 1000 / 3, 2);
      else if (ms <= 0) mem = r->getFrameAtTime(0, 2);  // its length unknown (Matroska here): its first picture
    }
    if (mem == nullptr) fprintf(stderr, "rondo-media: no frame of %s at %lld us\n", path, (long long)atUs);
    // the frame's memory mapped into this process: it can fail (Nov 10: a null pointer, which crashed
    // rondo-media and the dashboard), and then the file isn't to blame
    const VideoFrame* f = mem != nullptr && mem->size() >= sizeof(VideoFrame) ? static_cast<const VideoFrame*>(mem->pointer()) : nullptr;
    if (mem != nullptr && mem->size() >= sizeof(VideoFrame) && !f) {
      fprintf(stderr, "rondo-media: the frame of %s couldn't be mapped: later\n", path);
      ok = -1;
    }
    if (f) {
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
        ok = 1;
      }
    }
  }
  r->disconnect();
  close(fd);
  return ok;
}

}  // namespace rondo
