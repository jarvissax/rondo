// The Y1's display: GLES2 straight into the framebuffer (Android 4.2's FramebufferNativeWindow, the
// window SurfaceFlinger itself draws into), paced by MediaTek's hardware vsync. Needs SurfaceFlinger
// stopped. Measured in N0 (S1b): a finished frame is on screen at the next refresh.
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "../aosp_begin.h"
#include <ui/FramebufferNativeWindow.h>
#include "../aosp_end.h"

#include "../../core/clock.h"
#include "../platform.h"

namespace rondo {
namespace {

constexpr unsigned long kMtkVsyncWait = 0x5601;  // /dev/mtkfb_vsync: returns at the next vsync

class FbDisplay : public Display {
 public:
  ~FbDisplay() override {
    if (dpy_ != EGL_NO_DISPLAY) {
      eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglTerminate(dpy_);
    }
    if (vsync_ >= 0) close(vsync_);
  }

  bool open() {
    // The display driver stays in early suspend (vsync every ~100 ms) until told the screen is on.
    int st = ::open("/sys/power/state", O_WRONLY);
    if (st >= 0) {
      write(st, "on", 2);
      close(st);
    }
    vsync_ = ::open("/dev/mtkfb_vsync", O_RDONLY);
    if (vsync_ < 0) {
      perror("display: /dev/mtkfb_vsync");
      return false;
    }
    ANativeWindow* window = android_createDisplaySurface();
    if (!window) {
      fprintf(stderr, "display: no framebuffer window (is SurfaceFlinger still running?)\n");
      return false;
    }
    dpy_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy_, nullptr, nullptr);
    const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                           EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_NONE};
    EGLConfig configs[64];
    EGLint n = 0;
    eglChooseConfig(dpy_, want, configs, 64, &n);
    EGLint fbFormat = 0;
    window->query(window, NATIVE_WINDOW_FORMAT, &fbFormat);
    EGLConfig config = n > 0 ? configs[0] : nullptr;
    for (EGLint i = 0; i < n; i++) {  // the config whose visual is the framebuffer's pixel format
      EGLint id = 0;
      eglGetConfigAttrib(dpy_, configs[i], EGL_NATIVE_VISUAL_ID, &id);
      if (id == fbFormat) {
        config = configs[i];
        break;
      }
    }
    surface_ = eglCreateWindowSurface(dpy_, config, window, nullptr);
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy_, config, EGL_NO_CONTEXT, ctxAttr);
    if (surface_ == EGL_NO_SURFACE || !eglMakeCurrent(dpy_, surface_, surface_, ctx)) {
      fprintf(stderr, "display: EGL setup failed (0x%x)\n", eglGetError());
      return false;
    }
    eglQuerySurface(dpy_, surface_, EGL_WIDTH, &w_);
    eglQuerySurface(dpy_, surface_, EGL_HEIGHT, &h_);
    // pacing comes from waitVsync(); a swap that also waited would cost a refresh
    eglSwapInterval(dpy_, 0);
    return true;
  }

  int width() const override { return w_; }
  int height() const override { return h_; }
  int64_t period() const override { return 23730000; }
  int64_t waitVsync() override {
    ioctl(vsync_, kMtkVsyncWait);
    return now_ns();
  }
  void present() override { eglSwapBuffers(dpy_, surface_); }

 private:
  int vsync_ = -1;
  EGLDisplay dpy_ = EGL_NO_DISPLAY;
  EGLSurface surface_ = EGL_NO_SURFACE;
  EGLint w_ = 0, h_ = 0;
};

}  // namespace

std::unique_ptr<Display> openDisplay() {
  std::unique_ptr<FbDisplay> d(new FbDisplay);
  if (!d->open()) return nullptr;
  return d;
}

}  // namespace rondo
