// N0 test S1: display and vsync without Java.
//
// Run with Android's Java layer stopped (`adb shell stop zygote`). It creates a SurfaceFlinger surface
// from this process, takes vsync from SurfaceFlinger's display events, draws an animated test scene
// with GLES2 for every refresh, reads the wheel from /dev/input, and prints statistics once a second
// and at the end: refreshes seen, frames drawn, refreshes missed, draw and swap time, and how long a
// wheel pulse takes to reach this process and to reach a finished frame.
//
//   s1 [seconds] [alpha]   default 60 s; "alpha": layer 99 % opaque; "buffers2": two buffers
//
// Written against Android 4.2.2's libgui as the Y1 ships it (native/sysroot, tools/native/sysroot.sh).

#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "../../platform/aosp_begin.h"
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <gui/DisplayEventReceiver.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayInfo.h>
#include <ui/PixelFormat.h>
#include <utils/String8.h>
#include "../../platform/aosp_end.h"

using namespace android;

#include "scene.h"

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 60;
  // "alpha": a 99 % opaque layer, which the hardware composer hands to GPU composition
  const bool alphaLayer = argc > 2 && !strcmp(argv[2], "alpha");
  // "buffers2": a two-buffer queue instead of the default three, for less latency
  const bool twoBuffers = argc > 2 && !strcmp(argv[2], "buffers2");

  sp<ProcessState> proc = ProcessState::self();
  proc->startThreadPool();

  // Objects whose constructor lives in the firmware get room to spare, in case MediaTek made the class
  // larger than AOSP's header says.
  sp<SurfaceComposerClient> client =
      new (::operator new(sizeof(SurfaceComposerClient) + 256)) SurfaceComposerClient();
  if (client->initCheck() != NO_ERROR) {
    fprintf(stderr, "s1: no SurfaceFlinger connection\n");
    return 1;
  }
  sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
  alignas(16) static unsigned char infoStore[256];
  DisplayInfo& info = *reinterpret_cast<DisplayInfo*>(infoStore);
  SurfaceComposerClient::getDisplayInfo(display, &info);
  printf("s1: display %ux%u, %.2f fps\n", info.w, info.h, info.fps);
  SurfaceComposerClient::unblankDisplay(display);

  // the backlight may have been switched off with the screen before the Java layer stopped
  int bl = open("/sys/class/leds/lcd-backlight/brightness", O_RDWR);
  if (bl >= 0) {
    char cur[8] = {0};
    read(bl, cur, sizeof(cur) - 1);
    if (atoi(cur) == 0) write(bl, "128", 3);
    close(bl);
  }

  const uint32_t W = info.w ? info.w : 480, H = info.h ? info.h : 360;
  sp<SurfaceControl> control =
      client->createSurface(String8("rondo-s1"), W, H, PIXEL_FORMAT_RGBX_8888, 0);
  if (control == nullptr) {
    fprintf(stderr, "s1: createSurface failed\n");
    return 1;
  }
  // With the Java layer stopped, no window manager gives the display a layer stack, so SurfaceFlinger
  // composes nothing onto it (dumpsys shows layerStack=4294967295, numLayers=0). Rondo is the window
  // manager now: layer stack 0 and the whole panel, upright.
  SurfaceComposerClient::openGlobalTransaction();
  SurfaceComposerClient::setDisplayLayerStack(display, 0);
  SurfaceComposerClient::setDisplayProjection(display, 0, Rect(W, H), Rect(W, H));
  control->setLayerStack(0);
  control->setLayer(0x7ffffff0);
  if (alphaLayer) control->setAlpha(0.99f);
  control->show();
  SurfaceComposerClient::closeGlobalTransaction();
  sp<Surface> surface = control->getSurface();
  ANativeWindow* window = surface.get();
  if (twoBuffers) native_window_set_buffer_count(window, 2);

  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(dpy, nullptr, nullptr);
  const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  EGLConfig configs[32];
  EGLint n = 0;
  eglChooseConfig(dpy, want, configs, 32, &n);
  EGLConfig config = nullptr;
  for (EGLint i = 0; i < n && !config; i++) {
    EGLint id = 0;
    eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (id == PIXEL_FORMAT_RGBX_8888) config = configs[i];
  }
  if (!config && n > 0) config = configs[0];
  if (!config) {
    fprintf(stderr, "s1: no EGL config\n");
    return 1;
  }
  EGLSurface eglSurface = eglCreateWindowSurface(dpy, config, window, nullptr);
  const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxAttr);
  if (eglSurface == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
      !eglMakeCurrent(dpy, eglSurface, eglSurface, ctx)) {
    fprintf(stderr, "s1: EGL setup failed (0x%x)\n", eglGetError());
    return 1;
  }
  eglSwapInterval(dpy, 1);
  printf("s1: GL %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

  GLuint prog = glCreateProgram();
  glAttachShader(prog, shader(GL_VERTEX_SHADER, kVertex));
  glAttachShader(prog, shader(GL_FRAGMENT_SHADER, kFragment));
  glBindAttribLocation(prog, 0, "a_pos");
  glBindAttribLocation(prog, 1, "a_color");
  glLinkProgram(prog);
  glUseProgram(prog);
  glUniform2f(glGetUniformLocation(prog, "u_scale"), 2.0f / W, 2.0f / H);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glViewport(0, 0, W, H);
  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);

  // vsync from SurfaceFlinger. The receiver is constructed in a roomy buffer in case the firmware's
  // class is larger than AOSP's.
  alignas(16) static unsigned char rxStore[256];
  DisplayEventReceiver* rx = new (rxStore) DisplayEventReceiver();
  if (rx->initCheck() != NO_ERROR) {
    fprintf(stderr, "s1: no display events\n");
    return 1;
  }
  rx->setVsyncRate(1);

  // the wheel and buttons (event2), Center and Power (event0), timestamped on the monotonic clock
  int inputs[2] = {open("/dev/input/event2", O_RDONLY | O_NONBLOCK),
                   open("/dev/input/event0", O_RDONLY | O_NONBLOCK)};
  for (int fd : inputs) {
    int clk = CLOCK_MONOTONIC;
    if (fd >= 0) ioctl(fd, EVIOCSCLOCKID, &clk);
  }

  Scene scene;
  std::vector<Vertex> verts;
  verts.reserve(4096);
  std::vector<int64_t> drawNs, swapNs, frameGapNs, inputReadNs, inputFrameNs, vsyncGapNs;
  int64_t start = now_ns(), lastReport = start, lastVsync = 0, lastFrameDone = 0;
  uint32_t lastCount = 0;
  int64_t vsyncs = 0, missed = 0, pendingInput = 0, wheelPulses = 0;
  int64_t secFrames = 0, secMissed = 0;

  while (now_ns() - start < int64_t(seconds) * 1000000000LL) {
    pollfd pfd[3] = {{rx->getFd(), POLLIN, 0}, {inputs[0], POLLIN, 0}, {inputs[1], POLLIN, 0}};
    if (poll(pfd, 3, 100) <= 0) continue;

    for (int k = 1; k < 3; k++) {
      if (!(pfd[k].revents & POLLIN)) continue;
      input_event ev[16];
      ssize_t got;
      while ((got = read(inputs[k - 1], ev, sizeof(ev))) > 0) {
        int64_t readAt = now_ns();
        for (size_t i = 0; i < size_t(got) / sizeof(input_event); i++) {
          if (ev[i].type != EV_KEY || ev[i].value != 1) continue;
          int64_t at = int64_t(ev[i].time.tv_sec) * 1000000000LL + ev[i].time.tv_usec * 1000LL;
          if (ev[i].code == KEY_DOWN) scene.selected = std::min(Scene::kRows - 1, scene.selected + 1);
          else if (ev[i].code == KEY_UP) scene.selected = std::max(0, scene.selected - 1);
          else continue;
          wheelPulses++;
          inputReadNs.push_back(readAt - at);
          if (!pendingInput) pendingInput = at;
        }
      }
    }

    if (!(pfd[0].revents & POLLIN)) continue;
    DisplayEventReceiver::Event events[8];
    ssize_t count = 0, got;
    int64_t vsyncAt = 0;
    while ((got = rx->getEvents(events, 8)) > 0) {
      for (ssize_t i = 0; i < got; i++) {
        if (events[i].header.type != DisplayEventReceiver::DISPLAY_EVENT_VSYNC) continue;
        count++;
        vsyncAt = events[i].header.timestamp;
        uint32_t c = events[i].vsync.count;
        if (lastCount && c > lastCount + 1) {
          missed += c - lastCount - 1;
          secMissed += c - lastCount - 1;
        }
        lastCount = c;
      }
    }
    if (!vsyncAt) continue;
    vsyncs += count;
    if (count > 1) {  // more than one refresh went by before we woke up: those got no new frame
      missed += count - 1;
      secMissed += count - 1;
    }
    if (lastVsync) vsyncGapNs.push_back(vsyncAt - lastVsync);
    double dt = lastVsync ? (vsyncAt - lastVsync) / 1e9 : 1.0 / 36.42;
    lastVsync = vsyncAt;

    int64_t t0 = now_ns();
    scene.step(dt);
    scene.build(verts, W, H);
    glClearColor(0.06f, 0.06f, 0.06f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(Vertex), verts.data(), GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, verts.size());
    int64_t t1 = now_ns();
    eglSwapBuffers(dpy, eglSurface);
    int64_t t2 = now_ns();
    scene.frames++;
    secFrames++;
    drawNs.push_back(t1 - t0);
    swapNs.push_back(t2 - t1);
    if (lastFrameDone) frameGapNs.push_back(t2 - lastFrameDone);
    lastFrameDone = t2;
    if (pendingInput) {
      inputFrameNs.push_back(t2 - pendingInput);
      pendingInput = 0;
    }

    if (t2 - lastReport >= 1000000000LL) {
      printf("s1: %3llds  frames %lld  missed %lld  draw p99 %.1f ms  swap p99 %.1f ms\n",
             (long long)((t2 - start) / 1000000000LL), (long long)secFrames, (long long)secMissed,
             pct(drawNs, 99), pct(swapNs, 99));
      fflush(stdout);
      lastReport = t2;
      secFrames = secMissed = 0;
    }
  }

  printf("s1: done: %lld refreshes, %lld frames, %lld missed refreshes\n", (long long)vsyncs,
         (long long)scene.frames, (long long)missed);
  printf("s1: refresh interval p50 %.2f ms, min %.2f, max %.2f\n", pct(vsyncGapNs, 50),
         pct(vsyncGapNs, 0), pct(vsyncGapNs, 100));
  printf("s1: frame interval p50 %.2f ms, p99 %.2f, max %.2f\n", pct(frameGapNs, 50),
         pct(frameGapNs, 99), pct(frameGapNs, 100));
  printf("s1: draw p50 %.2f ms, p99 %.2f, max %.2f; swap p50 %.2f ms, p99 %.2f, max %.2f\n",
         pct(drawNs, 50), pct(drawNs, 99), pct(drawNs, 100), pct(swapNs, 50), pct(swapNs, 99),
         pct(swapNs, 100));
  if (wheelPulses)
    printf("s1: %lld wheel pulses: event to read p50 %.2f ms, max %.2f; event to finished frame p50 "
           "%.2f ms, max %.2f\n",
           (long long)wheelPulses, pct(inputReadNs, 50), pct(inputReadNs, 100),
           pct(inputFrameNs, 50), pct(inputFrameNs, 100));
  fflush(stdout);

  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(dpy, eglSurface);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);
  rx->~DisplayEventReceiver();
  control.clear();
  client->dispose();
  IPCThreadState::self()->stopProcess();
  return 0;
}
