// N0 test S2: hardware H.264 decoding without Java.
//
// Run with the Java layer stopped (`adb shell stop zygote`). It reads an MP4 with the native
// NuMediaExtractor and decodes its video with the native MediaCodec: the C++ classes Java's
// MediaExtractor and MediaCodec wrap, so the same MediaTek hardware decoder today's Rondo uses. Frames
// are shown at their presentation times, in one of two ways:
//
//   surface   the decoder renders into a SurfaceFlinger layer of its own
//   texture   the decoder renders into a SurfaceTexture in this process, and our own GL draws it as an
//             external texture (no copy) with a moving bar on top: the path for drawing video when
//             Rondo owns the display
//
// MediaTek's decoder is known to return timestamp 0 for every frame (player/TimestampFix.kt in the
// Kotlin app); this counts how often it does and, like that fix, takes presentation times from the
// samples it queued, smallest first.
//
//   fb        like texture, but with SurfaceFlinger stopped: our GL draws straight to the framebuffer
//             and the video buffers come from our own allocator (`stop surfaceflinger` first)
//
//   s2 <file.mp4> [surface|texture|fb] [seconds]

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "../../platform/aosp_begin.h"
#include <binder/ProcessState.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/SurfaceTexture.h>
#include <gui/SurfaceTextureClient.h>
#include <media/ICrypto.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <gui/BufferQueue.h>
#include <gui/IGraphicBufferAlloc.h>
#include <ui/DisplayInfo.h>
#include <ui/FramebufferNativeWindow.h>
#include <ui/PixelFormat.h>
#include <utils/String8.h>
#include "../../platform/aosp_end.h"

using namespace android;

// SurfaceFlinger's in-process buffer allocator, from libsurfaceflinger.so (MediaTek's own build, so its
// vtable is the firmware's). Declared here because AOSP keeps its header inside SurfaceFlinger.
namespace android {
class GraphicBufferAlloc : public BnGraphicBufferAlloc {
 public:
  GraphicBufferAlloc();
  virtual ~GraphicBufferAlloc();
  virtual sp<GraphicBuffer> createGraphicBuffer(uint32_t w, uint32_t h, PixelFormat format,
                                                uint32_t usage, status_t* error);
};
}  // namespace android

namespace {

int64_t now_ns() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

double pct(std::vector<int64_t> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(p / 100.0 * (v.size() - 1) + 0.5))] / 1e6;
}

// Objects whose constructor lives in the firmware get room to spare, in case MediaTek made the class
// larger than AOSP's header says.
template <typename T, typename... A>
T* roomy(A&&... a) {
  return new (::operator new(sizeof(T) + 256)) T(static_cast<A&&>(a)...);
}

const char* kVertex =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform mat4 u_tex;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "  v_uv = (u_tex * vec4(a_uv, 0.0, 1.0)).xy;\n"
    "}\n";
const char* kVideo =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES u_frame;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_frame, v_uv); }\n";
const char* kFlat =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

GLuint shader(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    fprintf(stderr, "s2: shader: %s\n", log);
    exit(1);
  }
  return s;
}

GLuint program(const char* fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, shader(GL_VERTEX_SHADER, kVertex));
  glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
  glBindAttribLocation(p, 0, "a_pos");
  glBindAttribLocation(p, 1, "a_uv");
  glLinkProgram(p);
  return p;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: s2 <file.mp4> [surface|texture] [seconds]\n");
    return 2;
  }
  const char* path = argv[1];
  const bool fb = argc > 2 && !strcmp(argv[2], "fb");
  const bool texture = fb || (argc > 2 && !strcmp(argv[2], "texture"));
  const int seconds = argc > 3 ? atoi(argv[3]) : 15;

  sp<ProcessState> proc = ProcessState::self();
  proc->startThreadPool();

  // the display, as in S1: layer stack, projection, one full-screen layer; or the framebuffer itself
  uint32_t W = 480, H = 360;
  sp<Surface> surface;
  EGLNativeWindowType fbWindow = nullptr;
  if (fb) {
    fbWindow = android_createDisplaySurface();
    if (!fbWindow) {
      fprintf(stderr, "s2: no framebuffer window (is SurfaceFlinger still running?)\n");
      return 1;
    }
  } else {
    sp<SurfaceComposerClient> client = roomy<SurfaceComposerClient>();
    sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
    alignas(16) static unsigned char infoStore[256];
    DisplayInfo& info = *reinterpret_cast<DisplayInfo*>(infoStore);
    SurfaceComposerClient::getDisplayInfo(display, &info);
    SurfaceComposerClient::unblankDisplay(display);
    W = info.w ? info.w : 480;
    H = info.h ? info.h : 360;
    sp<SurfaceControl> control =
        client->createSurface(String8("rondo-s2"), W, H, PIXEL_FORMAT_RGBX_8888, 0);
    if (control == nullptr) {
      fprintf(stderr, "s2: createSurface failed\n");
      return 1;
    }
    SurfaceComposerClient::openGlobalTransaction();
    SurfaceComposerClient::setDisplayLayerStack(display, 0);
    SurfaceComposerClient::setDisplayProjection(display, 0, Rect(W, H), Rect(W, H));
    control->setLayerStack(0);
    control->setLayer(0x7ffffff0);
    control->show();
    SurfaceComposerClient::closeGlobalTransaction();
    surface = control->getSurface();
    static sp<SurfaceComposerClient> keepClient = client;  // keep the layer alive until exit
    static sp<SurfaceControl> keepControl = control;
  }

  // the clip
  DataSource::RegisterDefaultSniffers();  // as the AOSP stagefright tool does, or no format is recognized
  sp<NuMediaExtractor> extractor = roomy<NuMediaExtractor>();
  if (extractor->setDataSource(path, nullptr) != OK) {
    fprintf(stderr, "s2: can't open %s\n", path);
    return 1;
  }
  sp<AMessage> format;
  AString mime;
  ssize_t track = -1;
  for (size_t i = 0; i < extractor->countTracks(); i++) {
    sp<AMessage> f;
    extractor->getTrackFormat(i, &f);
    AString m;
    if (f->findString("mime", &m) && !strncasecmp(m.c_str(), "video/", 6)) {
      track = i;
      format = f;
      mime = m;
      break;
    }
  }
  if (track < 0) {
    fprintf(stderr, "s2: no video track\n");
    return 1;
  }
  extractor->selectTrack(track);
  int32_t vw = 0, vh = 0;
  format->findInt32("width", &vw);
  format->findInt32("height", &vh);
  printf("s2: %s %dx%d, %s mode\n", mime.c_str(), vw, vh, fb ? "fb" : texture ? "texture" : "surface");

  // in texture mode the decoder renders into our SurfaceTexture, and our GL draws to the layer
  EGLDisplay dpy = EGL_NO_DISPLAY;
  EGLSurface eglSurface = EGL_NO_SURFACE;
  GLuint videoProg = 0, flatProg = 0, tex = 0;
  sp<SurfaceTexture> st;
  sp<SurfaceTextureClient> decoderWindow;
  if (texture) {
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(dpy, nullptr, nullptr);
    const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                           EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint n = 0;
    eglChooseConfig(dpy, want, &config, 1, &n);
    if (fb) {
      // the config whose visual matches the framebuffer's format (RGB565 here)
      EGLint fmt = 0, n2 = 0;
      fbWindow->query(fbWindow, NATIVE_WINDOW_FORMAT, &fmt);
      const EGLint any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
      EGLConfig all[64];
      eglChooseConfig(dpy, any, all, 64, &n2);
      for (EGLint i = 0; i < n2; i++) {
        EGLint id = 0;
        eglGetConfigAttrib(dpy, all[i], EGL_NATIVE_VISUAL_ID, &id);
        if (id == fmt) {
          config = all[i];
          break;
        }
      }
    }
    eglSurface = eglCreateWindowSurface(dpy, config, fb ? fbWindow : surface.get(), nullptr);
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctxAttr);
    if (!eglMakeCurrent(dpy, eglSurface, eglSurface, ctx)) {
      fprintf(stderr, "s2: EGL setup failed (0x%x)\n", eglGetError());
      return 1;
    }
    // the framebuffer's own swap waits for the flip; frames are paced by their timestamps instead
    eglSwapInterval(dpy, fb ? 0 : 1);
    videoProg = program(kVideo);
    flatProg = program(kFlat);
    glGenTextures(1, &tex);
    if (fb) {
      // no SurfaceFlinger to allocate the video buffers: a BufferQueue with SurfaceFlinger's allocator
      // running in this process
      sp<IGraphicBufferAlloc> alloc = roomy<GraphicBufferAlloc>();
      sp<BufferQueue> bq = roomy<BufferQueue>(true, alloc);
      st = roomy<SurfaceTexture>(tex, true, (GLenum)GL_TEXTURE_EXTERNAL_OES, true, bq);
    } else {
      st = roomy<SurfaceTexture>(tex, true, (GLenum)GL_TEXTURE_EXTERNAL_OES, true);
    }
    decoderWindow = roomy<SurfaceTextureClient>(st);
  } else {
    decoderWindow = surface;
  }

  sp<ALooper> looper = roomy<ALooper>();
  looper->setName("s2");
  looper->start();
  int64_t t0 = now_ns();
  sp<MediaCodec> codec = MediaCodec::CreateByType(looper, mime.c_str(), false);
  if (codec == nullptr || codec->configure(format, decoderWindow, nullptr, 0) != OK ||
      codec->start() != OK) {
    fprintf(stderr, "s2: can't start a %s decoder\n", mime.c_str());
    return 1;
  }
  int64_t t1 = now_ns();
  Vector<sp<ABuffer> > inputs, outputs;
  codec->getInputBuffers(&inputs);
  codec->getOutputBuffers(&outputs);
  printf("s2: decoder ready in %.1f ms, %zu input and %zu output buffers\n", (t1 - t0) / 1e6,
         inputs.size(), outputs.size());

  std::multiset<int64_t> queued;  // presentation times of the samples in the decoder, smallest first
  bool inputDone = false;
  int64_t firstPts = -1, startNs = 0, firstFrameNs = 0;
  int64_t frames = 0, zeroPts = 0, late = 0, inputs_ = 0;
  std::vector<int64_t> lateness, decodeGap;
  int64_t lastOut = 0;
  const int64_t frameNs = 1000000000LL / 30;
  float mtx[16];

  while (now_ns() - t1 < int64_t(seconds) * 1000000000LL) {
    // feed every free input buffer
    size_t index;
    while (!inputDone && codec->dequeueInputBuffer(&index, 0) == OK) {
      const sp<ABuffer>& buf = inputs.itemAt(index);
      if (extractor->readSampleData(buf) != OK) {
        codec->queueInputBuffer(index, 0, 0, 0, MediaCodec::BUFFER_FLAG_EOS);
        inputDone = true;
        break;
      }
      int64_t pts = 0;
      extractor->getSampleTime(&pts);
      codec->queueInputBuffer(index, buf->offset(), buf->size(), pts, 0);
      queued.insert(pts);
      inputs_++;
      extractor->advance();
    }

    size_t offset, size;
    int64_t pts;
    uint32_t flags;
    status_t r = codec->dequeueOutputBuffer(&index, &offset, &size, &pts, &flags, 5000);
    if (r == INFO_OUTPUT_BUFFERS_CHANGED) {
      codec->getOutputBuffers(&outputs);
      continue;
    }
    if (r == INFO_FORMAT_CHANGED) {
      sp<AMessage> of;
      codec->getOutputFormat(&of);
      int32_t ow = 0, oh = 0, color = 0;
      of->findInt32("width", &ow);
      of->findInt32("height", &oh);
      of->findInt32("color-format", &color);
      printf("s2: output %dx%d, color format 0x%x\n", ow, oh, color);
      continue;
    }
    if (r != OK) continue;
    if (flags & MediaCodec::BUFFER_FLAG_EOS) break;

    if (pts == 0) zeroPts++;
    int64_t fixed = queued.empty() ? pts : *queued.begin();
    if (!queued.empty()) queued.erase(queued.begin());
    int64_t now = now_ns();
    if (lastOut) decodeGap.push_back(now - lastOut);
    lastOut = now;
    if (firstPts < 0) {
      firstPts = fixed;
      startNs = now;
      firstFrameNs = now - t1;
    }
    int64_t due = startNs + (fixed - firstPts) * 1000;
    if (due > now) usleep(useconds_t((due - now) / 1000));
    int64_t shown = now_ns();
    lateness.push_back(shown - due);
    if (shown - due > frameNs) late++;
    codec->renderOutputBufferAndRelease(index);
    frames++;

    if (texture) {
      // wait for the frame to reach our SurfaceTexture, then draw it letterboxed with a bar on top
      // (timestamps can't tell frames apart: MediaTek's decoder sends 0 for all of them, so watch
      // for the latched buffer to change instead)
      GraphicBuffer* before = st->getCurrentBuffer().get();
      int64_t waitUntil = now_ns() + 30000000LL;
      do {
        st->updateTexImage();
      } while (st->getCurrentBuffer().get() == before && now_ns() < waitUntil && (usleep(500), true));
      st->getTransformMatrix(mtx);
      glViewport(0, 0, W, H);
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      float vh2 = float(W) * vh / vw / H;  // the video's height in clip space, fit to the width
      const float quad[] = {-1, -vh2, 0, 0, 1, -vh2, 1, 0, -1, vh2, 0, 1, 1, vh2, 1, 1};
      glUseProgram(videoProg);
      glUniformMatrix4fv(glGetUniformLocation(videoProg, "u_tex"), 1, GL_FALSE, mtx);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
      glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
      glEnableVertexAttribArray(0);
      glEnableVertexAttribArray(1);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      float x = -1 + 2.0f * (frames % 60) / 60;
      const float bar[] = {x, -1, 0, 0, x + 0.2f, -1, 0, 0, x, -0.9f, 0, 0, x + 0.2f, -0.9f, 0, 0};
      glUseProgram(flatProg);
      glUniform4f(glGetUniformLocation(flatProg, "u_color"), 1, 0.8f, 0.1f, 1);
      glUniformMatrix4fv(glGetUniformLocation(flatProg, "u_tex"), 1, GL_FALSE, mtx);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, bar);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      eglSwapBuffers(dpy, eglSurface);
    }
  }
  int64_t elapsed = lastOut - startNs;
  printf("s2: %lld frames shown in %.1f s (%.1f fps), first frame %.0f ms after start\n",
         (long long)frames, elapsed / 1e9, frames / (elapsed / 1e9), firstFrameNs / 1e6);
  printf("s2: %lld of %lld frames came back with timestamp 0 (the MediaTek decoder bug)\n",
         (long long)zeroPts, (long long)frames);
  printf("s2: shown vs due p50 %.2f ms, p99 %.2f, max %.2f; over one frame late: %lld\n",
         pct(lateness, 50), pct(lateness, 99), pct(lateness, 100), (long long)late);
  printf("s2: decoder output interval p50 %.2f ms, p99 %.2f ms\n", pct(decodeGap, 50),
         pct(decodeGap, 99));
  fflush(stdout);
  codec->stop();
  codec->release();
  looper->stop();
  return 0;
}
