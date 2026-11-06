// N0 test S1b: drawing straight to the framebuffer, without SurfaceFlinger.
//
// Run with the Java layer and SurfaceFlinger stopped (`stop zygote; stop surfaceflinger`). It draws
// the S1 scene with GLES2 into Android 4.2's framebuffer window (libui's android_createDisplaySurface,
// the window SurfaceFlinger itself used to draw into), paced by MediaTek's hardware vsync
// (/dev/mtkfb_vsync, ioctl 0x5601), and measures how far a finished frame is from the next vsync,
// which is when the panel starts showing it. Through SurfaceFlinger that path took ~68 ms (S1).
//
//   s1fb [seconds]      default 20

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "../../platform/aosp_begin.h"
#include <ui/FramebufferNativeWindow.h>
#include "../../platform/aosp_end.h"

#include "scene.h"

int main(int argc, char** argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 20;
  int vsyncFd = open("/dev/mtkfb_vsync", O_RDONLY);
  if (vsyncFd < 0) {
    perror("s1fb: /dev/mtkfb_vsync");
    return 1;
  }
  EGLNativeWindowType window = android_createDisplaySurface();
  if (!window) {
    fprintf(stderr, "s1fb: no framebuffer window (is SurfaceFlinger still running?)\n");
    return 1;
  }
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(dpy, nullptr, nullptr);
  const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                         EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_NONE};
  EGLConfig configs[64];
  EGLint n = 0;
  eglChooseConfig(dpy, want, configs, 64, &n);
  // the config whose visual matches the framebuffer's pixel format
  EGLint fbFormat = 0;
  window->query(window, NATIVE_WINDOW_FORMAT, &fbFormat);
  EGLConfig config = n > 0 ? configs[0] : nullptr;
  for (EGLint i = 0; i < n; i++) {
    EGLint id = 0;
    eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (id == fbFormat) {
      config = configs[i];
      break;
    }
  }
  EGLSurface surface = eglCreateWindowSurface(dpy, config, window, nullptr);
  const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxAttr);
  if (surface == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surface, surface, ctx)) {
    fprintf(stderr, "s1fb: EGL setup failed (0x%x)\n", eglGetError());
    return 1;
  }
  EGLint W = 0, H = 0;
  eglQuerySurface(dpy, surface, EGL_WIDTH, &W);
  eglQuerySurface(dpy, surface, EGL_HEIGHT, &H);
  // eglSwapBuffers would otherwise wait for the flip itself; pacing comes from the vsync wait below
  eglSwapInterval(dpy, 0);
  printf("s1fb: framebuffer %dx%d, format %d, GL %s\n", W, H, fbFormat, glGetString(GL_RENDERER));

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

  Scene scene;
  std::vector<Vertex> verts;
  verts.reserve(4096);
  std::vector<int64_t> vsyncGap, workNs, swapNs, toScreenNs;
  int64_t start = now_ns(), lastVsync = 0;
  int64_t pendingSwap = 0, late = 0;
  while (now_ns() - start < int64_t(seconds) * 1000000000LL) {
    ioctl(vsyncFd, 0x5601);
    int64_t v = now_ns();
    if (pendingSwap) {  // the frame finished before this vsync starts showing now
      toScreenNs.push_back(v - pendingSwap);
      pendingSwap = 0;
    }
    if (lastVsync) {
      vsyncGap.push_back(v - lastVsync);
      if (v - lastVsync > 30000000LL) late++;
    }
    double dt = lastVsync ? (v - lastVsync) / 1e9 : 1 / 42.14;
    lastVsync = v;
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
    eglSwapBuffers(dpy, surface);
    int64_t t2 = now_ns();
    scene.frames++;
    workNs.push_back(t1 - t0);
    swapNs.push_back(t2 - t1);
    pendingSwap = t2;
  }
  printf("s1fb: %lld frames, vsync interval p50 %.2f ms (max %.2f), vsyncs over 30 ms apart: %lld\n",
         (long long)scene.frames, pct(vsyncGap, 50), pct(vsyncGap, 100), (long long)late);
  printf("s1fb: draw p50 %.2f ms, p99 %.2f; swap p50 %.2f ms, p99 %.2f, max %.2f\n", pct(workNs, 50),
         pct(workNs, 99), pct(swapNs, 50), pct(swapNs, 99), pct(swapNs, 100));
  printf("s1fb: finished frame to next vsync p50 %.2f ms, p99 %.2f, max %.2f\n", pct(toScreenNs, 50),
         pct(toScreenNs, 99), pct(toScreenNs, 100));
  fflush(stdout);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglTerminate(dpy);
  return 0;
}
