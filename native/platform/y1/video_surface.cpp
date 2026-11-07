// The Y1's VideoSurface: a BufferQueue whose buffers come from SurfaceFlinger's allocator running in
// this process (SurfaceFlinger itself is stopped), a SurfaceTexture over it, and the queue published to
// rondo-player as "rondo.video". As in s2's fb mode.
#include "../video_surface.h"

#include <dlfcn.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "../aosp_begin.h"
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <gui/BufferQueue.h>
#include <gui/IGraphicBufferAlloc.h>
#include <gui/SurfaceTexture.h>
#include <ui/GraphicBuffer.h>
#include <utils/String16.h>
#include "../aosp_end.h"

// SurfaceFlinger's in-process buffer allocator, from libsurfaceflinger.so (AOSP keeps its header inside
// SurfaceFlinger). The library is loaded when the first video plays, not with Rondo: only its
// constructor is looked up, so nothing here links against it.
namespace android {
class GraphicBufferAlloc : public BnGraphicBufferAlloc {
 public:
  GraphicBufferAlloc();
  virtual ~GraphicBufferAlloc();
  virtual sp<GraphicBuffer> createGraphicBuffer(uint32_t w, uint32_t h, PixelFormat format, uint32_t usage,
                                                status_t* error);
};
}  // namespace android

namespace rondo {
namespace {

using android::sp;

// Objects whose constructor lives in the firmware get room to spare, in case MediaTek made the class
// larger than AOSP's header says (as in s2).
template <typename T, typename... A>
T* roomy(A&&... a) {
  return new (::operator new(sizeof(T) + 256)) T(static_cast<A&&>(a)...);
}

// Called on a binder thread when the decoder queues a picture: wakes the drawing thread.
class Listener : public android::ConsumerBase::FrameAvailableListener {
 public:
  explicit Listener(int fd) : fd_(fd) {}
  void onFrameAvailable() override {
    uint64_t one = 1;
    (void)!write(fd_, &one, sizeof(one));
  }

 private:
  int fd_;
};

class Y1VideoSurface : public VideoSurface {
 public:
  bool open() {
    fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd_ < 0) return false;
    void* lib = dlopen("libsurfaceflinger.so", RTLD_NOW);
    auto make = lib ? reinterpret_cast<void (*)(void*)>(dlsym(lib, "_ZN7android18GraphicBufferAllocC1Ev")) : nullptr;
    if (!make) {
      fprintf(stderr, "rondo: no GraphicBufferAlloc (%s)\n", dlerror());
      return false;
    }
    void* mem = ::operator new(sizeof(android::GraphicBufferAlloc) + 256);
    make(mem);
    sp<android::IGraphicBufferAlloc> alloc = static_cast<android::GraphicBufferAlloc*>(mem);
    glGenTextures(1, &tex_);
    queue_ = roomy<android::BufferQueue>(true, alloc);
    st_ = roomy<android::SurfaceTexture>(tex_, true, (GLenum)GL_TEXTURE_EXTERNAL_OES, true, queue_);
    listener_ = new Listener(fd_);
    st_->setFrameAvailableListener(listener_);
    // the decoder's buffer traffic arrives over binder: this process answers it on threads of its own
    android::ProcessState::self()->startThreadPool();
    if (android::defaultServiceManager()->addService(android::String16(kVideoService), queue_) != android::OK) {
      fprintf(stderr, "rondo: can't publish %s\n", kVideoService);
      return false;
    }
    for (int i = 0; i < 16; i++) m_[i] = i % 5 == 0 ? 1.f : 0.f;
    return true;
  }

  int fd() const override { return fd_; }
  bool latch() override {
    uint64_t n = 0;
    if (read(fd_, &n, sizeof(n)) != sizeof(n) || n == 0) return false;
    // each call takes the next picture in the queue; the last one stays on the texture
    android::GraphicBuffer* before = st_->getCurrentBuffer().get();
    for (uint64_t i = 0; i < n && i < 8; i++) st_->updateTexImage();
    sp<android::GraphicBuffer> b = st_->getCurrentBuffer();
    if (b == nullptr) return false;
    st_->getTransformMatrix(m_);
    android::Rect crop = st_->getCurrentCrop();
    w_ = crop.isEmpty() ? int(b->getWidth()) : crop.width();
    h_ = crop.isEmpty() ? int(b->getHeight()) : crop.height();
    bool changed = b.get() != before || !has_;
    has_ = true;
    return changed;
  }
  bool hasPicture() const override { return has_; }
  void clear() override { has_ = false; }
  unsigned texture() const override { return tex_; }
  const float* transform() const override { return m_; }
  int width() const override { return w_; }
  int height() const override { return h_; }

 private:
  int fd_ = -1;
  GLuint tex_ = 0;
  sp<android::BufferQueue> queue_;
  sp<android::SurfaceTexture> st_;
  sp<Listener> listener_;
  float m_[16];
  int w_ = 0, h_ = 0;
  bool has_ = false;
};

}  // namespace

namespace {
Y1VideoSurface* g_surface = nullptr;
}

VideoSurface* VideoSurface::get() {
  static bool tried = false;
  if (!tried) {
    tried = true;
    Y1VideoSurface* s = new Y1VideoSurface();
    if (s->open()) g_surface = s;
  }
  return g_surface;
}

VideoSurface* VideoSurface::made() { return g_surface; }

}  // namespace rondo
