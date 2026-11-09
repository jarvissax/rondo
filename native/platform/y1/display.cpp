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
// values in its panel driver (tools/firmware/BootImage.java): it sets the PLL itself at boot and at
// every wake, and leaves the D-PHY timing registers as they held (the bootloader's stock ones at boot).
// Here, at startup, whatever is still stock is set: through the display driver's debugfs (dispsys regw),
// only when every register holds this panel's stock or 60 Hz value, and measured after. The PLL is
// switched only where the kernel hasn't set it (1.0.x's firmware, for development, where a deep sleep
// brings the stock timing back): on 2.0's firmware the kernel brings its PLL back at the next wake
// whatever is done here, so nothing is put back there either (Nov 9's first boot put the stock values
// back after a wrong measurement, and the next wake left the kernel's PLL over the stock timings, at
// 60.30 Hz). At a cold boot the PLL is switched again (rate()). The DSI clock moves from 104 MHz (inside the FM band) to
// 150 MHz; 100 pings to the router lost nothing at either rate. RONDO_HZ=42 (development) keeps the
// stock timing.
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
// what the registers hold: the PLL 0 (stock), 1 (60 Hz) or -1 (anything else), and the D-PHY timings
// 1 (all 60 Hz), 0 (each stock or 60 Hz: to be set; after a deep sleep the kernel sets the PLL up again
// but restores the DSI's own registers from what they held) or -1 (anything else); false when they
// can't be read or the porches aren't this panel's (another panel: left alone)
bool state(int& pll, int& timings) {
  uint32_t con0, con2, vbp, vfp;
  if (!get(kPllCon0, con0) || !get(kPllCon2, con2) || !get(kVbp, vbp) || !get(kVfp, vfp)) return false;
  if (vbp != 100 || vfp != 100) return false;
  pll = con0 == kStockCon0 && con2 == kStockCon2 ? 0 : con0 == kFastCon0 && con2 == kFastCon2 ? 1 : -1;
  timings = 1;
  for (const Reg& r : kTiming) {
    uint32_t v;
    if (!get(r.addr, v)) return false;
    if (v != r.stock && v != r.fast) {
      timings = -1;
      break;
    }
    if (v != r.fast) timings = 0;
  }
  return true;
}
// the PLL off, its new multiplier, on with the new divider, the multiplier taken (SDM_PCW_CHG); the
// multiplier only takes effect with the PLL switched on again
void setPll(bool fast) {
  put(kPllCon0, (fast ? kFastCon0 : kStockCon0) & ~1u);
  put(kPllCon2, fast ? kFastCon2 : kStockCon2);
  put(kPllCon0, fast ? kFastCon0 : kStockCon0);
  put(kPllCon1, 0x01b10003);
  put(kPllCon1, 0x01b10007);
}
void setTimings(bool fast) {
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
    // Only on 2.0's firmware (ro.rondo.firmware 2 and up), whose kernel sets its PLL to 60 Hz itself at
    // boot and at every wake. 1.0.x's kernel would bring its PLL back over 60 Hz's timings after a deep
    // sleep, which stalls the link (Nov 9): development runs there stay at 42 Hz unless RONDO_HZ=60 asks
    // for it (until the first deep sleep), and RONDO_HZ=42 keeps the stock timing anywhere.
    char fw[PROP_VALUE_MAX] = "";
    __system_property_get("ro.rondo.firmware", fw);
    const bool kernel = atoi(fw) >= 2;
    const char* hz = getenv("RONDO_HZ");
    bool want = hz ? strcmp(hz, "42") != 0 : kernel;
    if (access(panel::kDebug, F_OK) != 0) mount("none", "/sys/kernel/debug", "debugfs", 0, nullptr);
    // the display wakes in the background after "on" (vsync every 30 ms until it has): its registers
    // are read once it is up
    for (int i = 0; i < 10 && measure(2) > 26000000; i++) usleep(50000);
    int pll = -1, timings = -1;
    bool known = panel::state(pll, timings) && pll >= 0 && timings >= 0;
    int st = !known ? -1 : pll == 1 && timings == 1 ? 1 : 0;
    const char* found = !known                      ? "unknown"
                        : pll == 1 && timings == 1  ? "60 Hz"
                        : pll == 0 && timings == 0  ? "stock"
                        : pll == 1                  ? "the kernel's PLL over stock timings"
                                                    : "the stock PLL over 60 Hz timings";
    if (st == 0 && want) {  // the PLL only where the kernel hasn't set it
      if (pll != 1) panel::setPll(true);
      if (timings != 1) panel::setTimings(true);
      st = 1;
    }
    if (st == 1 && !want) panel::setPll(false), panel::setTimings(false), st = 0;
    period_ = measure();
    fast_ = st == 1;
    auto off = [](int64_t p) { return p < 16200000 || p > 17200000; };  // not what was measured on the Y1
    if (fast_ && off(period_)) {
      // At a cold boot of 2.0's firmware the registers hold the kernel's 60 Hz words, yet the panel
      // refreshes every 13.79 ms (72.5 Hz; Nov 9's first two boots), and at 60.00 after the first
      // wake with the same words: the multiplier only takes effect when the PLL is switched on again,
      // which the kernel does at a wake but not when it takes the panel over from the bootloader. The
      // PLL switched again here, as at a wake.
      fprintf(stderr, "display: the panel refreshed every %.2f ms at 60 Hz's settings: the PLL switched again\n", period_ / 1e6);
      panel::setPll(true);
      panel::setTimings(true);
      usleep(100000);
      period_ = measure();
      if (off(period_) && !kernel) {  // back to stock, which 1.0.x's kernel brings back at its next wake anyway
        fprintf(stderr, "display: the panel refreshed every %.2f ms at 60 Hz's settings: back to its own\n", period_ / 1e6);
        panel::setPll(false);
        panel::setTimings(false);
        fast_ = false, st = 0;
        period_ = measure();
      } else if (off(period_)) {  // kept: the kernel brings its PLL back at the next wake whatever is done here
        fprintf(stderr, "display: still every %.2f ms (16.67 expected): kept, the kernel holds the settings\n", period_ / 1e6);
      }
    }
    printf("display: %.2f Hz (%s; found %s)\n", 1e9 / double(period_), fast_ ? "60 Hz settings" : st == 0 ? "the panel's own" : "unknown panel, left alone",
           found);
  }
  // the median of n refreshes (six: one disturbed interval, and the short one after it, don't count)
  int64_t measure(int n = 6) {
    ioctl(vsync_, kMtkVsyncWait);
    int64_t last = now_ns(), d[8];
    n = std::min(std::max(n, 2), 8);
    for (int i = 0; i < n; i++) {
      ioctl(vsync_, kMtkVsyncWait);
      int64_t t = now_ns();
      d[i] = t - last;
      last = t;
    }
    std::sort(d, d + n);
    return (d[(n - 1) / 2] + d[n / 2]) / 2;
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
