// The Y1's display: GLES2 straight into the framebuffer (Android 4.2's FramebufferNativeWindow, the
// window SurfaceFlinger itself draws into), paced by MediaTek's hardware vsync. Needs SurfaceFlinger
// stopped. Measured in N0 (S1b): a finished frame is on screen at the next refresh.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <string>

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

// The panel at 60 Hz instead of the 42.14 it ships at (Nov 9). It is an ST7701 on two MIPI DSI lanes in
// video mode (the kernel's st7701_hvga_dsi_vdo_boe: 360 lines with 8 + 100 + 100 of sync and porches,
// 208 Mb/s a lane). The porches have to stay as the panel's own settings expect (a shorter front porch
// shifted the picture), so the link runs faster instead: the MIPI PLL's divider 4 -> 2 and its
// multiplier fractional, for about 300 Mb/s a lane, and the D-PHY timings worked out again with
// MediaTek's own formulas (DSI_PHY_TIMCONFIG) for that byte clock. The 2.0 firmware's kernel has these
// values in its panel driver (tools/firmware/BootImage.java), so the kernel itself sets them at every
// wake. Here they are set once at startup when the panel still has the bootloader's stock timing (and
// on 1.0.x's firmware, for development, where a deep sleep brings the stock timing back): through the
// display driver's debugfs (dispsys regw), only when every register holds this panel's stock or 60 Hz
// value, and measured after. The DSI clock moves from 104 MHz (inside the FM band) to 150 MHz; 100
// pings to the router lost nothing at either rate. RONDO_HZ=42 (development) keeps the stock timing.
namespace panel {

constexpr const char* kDebug = "/sys/kernel/debug/dispsys";
constexpr uint32_t kPllCon0 = 0xf0011050, kPllCon1 = 0xf0011054, kPllCon2 = 0xf0011058;
constexpr uint32_t kVbp = 0xf400c024, kVfp = 0xf400c028;
struct Reg {
  uint32_t addr, stock, fast;
};
// TIMCON0..3: LPX, HS_PREP, HS_ZERO, HS_TRAIL / TA_GO, TA_SURE, TA_GET, DA_HS_EXIT / CONT_DET, CLK_ZERO,
// CLK_TRAIL / CLK_HS_PREP, CLK_HS_POST, CLK_HS_EXIT, in byte clocks: 39 ns stock, and what the patched
// kernel works out for 60 Hz (a 28 ns cycle, 4 ns a bit)
constexpr Reg kTiming[] = {{0xf400c110, 0x0c040202, 0x0d050302},
                           {0xf400c114, 0x110a0308, 0x140a0308},
                           {0xf400c118, 0x0c0a0000, 0x0d0e0000},
                           {0xf400c11c, 0x00041101, 0x00041402}};
constexpr uint32_t kStockCon0 = 0x11, kStockCon2 = 0x3c000000;  // TXDIV0 2 (/4), fbk_div 15
constexpr uint32_t kFastCon0 = 0x09, kFastCon2 = 0x2b326100;    // TXDIV0 1 (/2), 43.197: 60.00 Hz (the patched kernel's word)

bool command(const char* c) {
  int fd = open(kDebug, O_WRONLY);
  if (fd < 0) return false;
  bool ok = write(fd, c, strlen(c)) == ssize_t(strlen(c));
  close(fd);
  return ok;
}
bool put(uint32_t addr, uint32_t v) {
  char c[48];
  snprintf(c, sizeof(c), "regw:%08x,%08x", addr, v);
  return command(c);
}
// "regr:<addr>", then the file says "regr: 0x<ADDR> = 0x<value>" after its usage text
bool get(uint32_t addr, uint32_t& v) {
  char c[32];
  snprintf(c, sizeof(c), "regr:%08x", addr);
  if (!command(c)) return false;
  int fd = open(kDebug, O_RDONLY);
  if (fd < 0) return false;
  std::string text;
  char buf[1024];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) text.append(buf, size_t(n));
  close(fd);
  char want[24];
  snprintf(want, sizeof(want), "0x%08X = 0x", addr);
  size_t at = text.rfind(want);
  if (at == std::string::npos) return false;
  v = uint32_t(strtoul(text.c_str() + at + strlen(want), nullptr, 16));
  return true;
}
// what the registers hold: 1 all fast, 0 each stock or fast (to be set: after a deep sleep the kernel
// sets the PLL up again but restores the DSI's own registers from what they held), -1 anything else
// (another panel: left alone)
int state() {
  uint32_t con0, con2, vbp, vfp;
  if (!get(kPllCon0, con0) || !get(kPllCon2, con2) || !get(kVbp, vbp) || !get(kVfp, vfp)) return -1;
  if (vbp != 100 || vfp != 100) return -1;
  bool pllStock = con0 == kStockCon0 && con2 == kStockCon2, pllFast = con0 == kFastCon0 && con2 == kFastCon2;
  if (!pllStock && !pllFast) return -1;
  bool fast = pllFast;
  for (const Reg& r : kTiming) {
    uint32_t v;
    if (!get(r.addr, v) || (v != r.stock && v != r.fast)) return -1;
    fast = fast && v == r.fast;
  }
  return fast ? 1 : 0;
}
// the PLL off, its new multiplier, on with the new divider, the multiplier taken (SDM_PCW_CHG), then
// the D-PHY timings; the multiplier only takes effect with the PLL switched on again
void set(bool fast) {
  put(kPllCon0, (fast ? kFastCon0 : kStockCon0) & ~1u);
  put(kPllCon2, fast ? kFastCon2 : kStockCon2);
  put(kPllCon0, fast ? kFastCon0 : kStockCon0);
  put(kPllCon1, 0x01b10003);
  put(kPllCon1, 0x01b10007);
  for (const Reg& r : kTiming) put(r.addr, fast ? r.fast : r.stock);
}

}  // namespace panel

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
    // the backlight: Android's power manager leaves it at 0 when it turned the screen off
    int bl = ::open("/sys/class/leds/lcd-backlight/brightness", O_RDWR);
    if (bl >= 0) {
      char level[8] = {0};
      if (read(bl, level, sizeof(level) - 1) > 0 && atoi(level) == 0) write(bl, "150", 3);
      close(bl);
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
    rate();
    return true;
  }

  int width() const override { return w_; }
  int height() const override { return h_; }
  int64_t period() const override { return period_; }
  int64_t waitVsync() override {
    ioctl(vsync_, kMtkVsyncWait);
    return now_ns();
  }
  void present() override { eglSwapBuffers(dpy_, surface_); }

 private:
  // the panel's rate: 60 Hz when its registers are this panel's stock ones, checked by measuring
  void rate() {
    // Only on 2.0's firmware (ro.rondo.firmware 2 and up), whose kernel sets 60 Hz itself at every wake.
    // 1.0.x's kernel would bring its PLL back over 60 Hz's timings after a deep sleep, which stalls the
    // link (Nov 9): development runs there stay at 42 Hz unless RONDO_HZ=60 asks for it (until the
    // first deep sleep), and RONDO_HZ=42 keeps the stock timing anywhere.
    char fw[PROP_VALUE_MAX] = "";
    __system_property_get("ro.rondo.firmware", fw);
    const char* hz = getenv("RONDO_HZ");
    bool want = hz ? strcmp(hz, "42") != 0 : atoi(fw) >= 2;
    if (access(panel::kDebug, F_OK) != 0) mount("none", "/sys/kernel/debug", "debugfs", 0, nullptr);
    // the display wakes in the background after "on" (vsync every 30 ms until it has): its registers
    // are read once it is up
    for (int i = 0; i < 10 && measure() > 26000000; i++) usleep(50000);
    int st = panel::state();
    if (st == 0 && want) panel::set(true), st = 1;
    if (st == 1 && !want) panel::set(false), st = 0;
    period_ = measure();
    fast_ = st == 1;
    if (fast_ && (period_ < 16200000 || period_ > 17200000)) {  // not what was measured on the Y1: back to stock
      fprintf(stderr, "display: the panel refreshed every %.2f ms at 60 Hz's settings: back to its own\n", period_ / 1e6);
      panel::set(false);
      fast_ = false;
      period_ = measure();
    }
    printf("display: %.2f Hz (%s)\n", 1e9 / double(period_), fast_ ? "60 Hz settings" : st == 0 ? "the panel's own" : "unknown panel, left alone");
  }
  // the median of four refreshes
  int64_t measure() {
    ioctl(vsync_, kMtkVsyncWait);
    int64_t last = now_ns(), d[4];
    for (int64_t& x : d) {
      ioctl(vsync_, kMtkVsyncWait);
      int64_t t = now_ns();
      x = t - last;
      last = t;
    }
    std::sort(d, d + 4);
    return (d[1] + d[2]) / 2;
  }
  int64_t period_ = 23730000;
  bool fast_ = false;
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
