// Settings › Dashboard (DashboardScreen.kt in 1.0.x), ported 1:1: runs the dashboard's server while
// this screen is open and shows how to reach it, the address, its QR code and the pairing code. The
// screen stays on while it runs (the CPU and Wi-Fi must be awake to answer; with the screen on, Wi-Fi's
// suspend optimizations, which 1.0.x's high-performance lock turned off, don't apply). Fifteen
// minutes without a request stop the server.
#include <math.h>
#include <stdio.h>

#include "../ui/gfx.h"
#include "../ui/qr.h"
#include "app.h"
#include "dashboard.h"
#include "screens.h"

namespace rondo::shell {

std::string g_dashboardAssets;

namespace {

constexpr int64_t kIdleNs = 15 * 60 * 1000000000LL;

std::string ipAddress() {
  CoreState s = g_core.state();
  if (s.wifiState != 4 || !s.ip) return "";
  char b[16];
  snprintf(b, sizeof(b), "%u.%u.%u.%u", s.ip & 0xff, s.ip >> 8 & 0xff, s.ip >> 16 & 0xff, s.ip >> 24);
  return b;
}

class DashboardScreen : public Screen {
 public:
  ~DashboardScreen() override {
    stop();
    destroyTexture(qr_);
  }
  std::string title() const override { return "Dashboard"; }
  void onShow() override { start(); }
  void onHide() override { stop(); }
  bool keepScreenOn() const override { return dashboardRunning(); }
  bool onAction(Action a, int) override {
    if (a != Action::Select) return false;
    if (ipAddress().empty()) g_app->push(makeWifi());
    else start();
    return true;
  }
  // 1.0.x's onTick, every 500 ms: the idle stop, a start once Wi-Fi is there, what the screen says
  bool tick(int64_t now) override {
    if (now < nextTick_) return false;
    nextTick_ = now + 500 * kMs;
    dashboardTick();
    if (dashboardRunning() && now_ns() - dashboardLastRequest() > kIdleNs) {
      stop();
      stoppedIdle_ = true;
    }
    if (!dashboardRunning() && !ipAddress().empty() && !stoppedIdle_) start();
    std::string sig = signature();
    if (sig == shown_) return false;
    shown_ = sig;
    return true;
  }
  int64_t wakeAt() const override { return nextTick_; }

  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    // the note at the bottom, on the header's color
    Label hint = label(e, "Keep this screen open while you use the dashboard \xc2\xb7 Menu stops it", 10, false, w);
    float hintH = float(hint.h + 2 * e.dp(5));
    e.c.fill({0, h - hintH, w, hintH}, p.header);
    e.c.layer(1);
    drawLabel(e, hint, centerText(0, w, hint.w), h - hintH + e.dp(5), p.muted);
    float area = h - hintH;
    std::string ip = ipAddress();
    if (!dashboardRunning() || ip.empty()) {
      std::string m = ip.empty() ? "Connect to Wi-Fi first.\nPress center for Settings \xe2\x80\xba Wi-Fi."
                      : stoppedIdle_ ? "The dashboard stopped after 15 minutes without use.\nPress center to start it again."
                                     : "The dashboard could not start.\nPress center to try again.";
      // a TextView of up to four lines, centered both ways, 20 dp in from the sides
      float tw = w - 2 * e.dp(20);
      std::vector<Label> lines;
      size_t at = 0;
      while (at <= m.size() && lines.size() < 4) {
        size_t nl = m.find('\n', at);
        for (Label& l : wrap(e, m.substr(at, nl == std::string::npos ? std::string::npos : nl - at), 14, false, tw, int(4 - lines.size())))
          lines.push_back(l);
        if (nl == std::string::npos) break;
        at = nl + 1;
      }
      int total = 0;
      for (const Label& l : lines) total += l.h;
      float y = float((int(area) - total) / 2);
      for (const Label& l : lines) {
        drawLabel(e, l, centerText(float(e.dp(20)), tw, l.w), y, p.text);
        y += l.h;
      }
      return;
    }
    std::string url = "http://" + ip + ":" + std::to_string(kDashboardPort);
    // the body: 12 dp in from the sides, 8 from the top and the note; the QR code and the column each
    // centered in its height (LinearLayout with gravity CENTER_VERTICAL)
    int padX = e.dp(12), padY = e.dp(8);
    int inner = int(area) - 2 * padY;
    int qrSize = e.dp(112), qrPad = e.dp(4);
    float qrY = float(padY + (inner - qrSize) / 2);
    e.c.layer(0);
    e.c.fill({float(padX), qrY, float(qrSize), float(qrSize)}, 0xffffffff);
    if (url != qrUrl_) makeQr(e, url);
    if (qr_.id) {
      e.c.layer(1);
      // ImageView FIT_CENTER: the dp(104) bitmap scaled into the box inside its padding, filtered
      e.c.image(qr_, {float(padX + qrPad), qrY + qrPad, float(qrSize - 2 * qrPad), float(qrSize - 2 * qrPad)}, {0, 0, 1, 1});
    }
    float cx = float(padX + qrSize + e.dp(14)), cw = w - padX - cx;
    Label open = label(e, "OPEN ON YOUR PHONE OR COMPUTER", 10, true, cw);
    Label address = label(e, url, 15, true, cw);
    Label pairing = label(e, "PAIRING CODE", 10, true, cw);
    // 4.2 has no letterSpacing: spaces spread the digits
    std::string c = dashboardCode(), spaced;
    for (size_t i = 0; i < c.size(); i++) spaced += std::string(i == 0 ? "" : i % 3 == 0 ? "  " : " ") + c[i];
    Label code = label(e, spaced, 24, true, cw);
    std::vector<Label> state = wrap(e, stateText(), 11, false, cw, 2);
    int stateH = 0;
    for (const Label& l : state) stateH += l.h;
    int colH = open.h + e.dp(3) + address.h + e.dp(12) + pairing.h + e.dp(2) + code.h + e.dp(10) + stateH;
    float y = float(padY + (inner - colH) / 2);
    e.c.layer(2);
    drawLabel(e, open, cx, y, p.muted);
    y += open.h + e.dp(3);
    drawLabel(e, address, cx, y, p.text);
    y += address.h + e.dp(12);
    drawLabel(e, pairing, cx, y, p.muted);
    y += pairing.h + e.dp(2);
    drawLabel(e, code, cx, y, p.accent);
    y += code.h + e.dp(10);
    for (const Label& l : state) {
      drawLabel(e, l, cx, y, p.muted);
      y += l.h;
    }
  }

 private:
  void start() {
    if (dashboardRunning()) return;
    if (ipAddress().empty()) return;
    stoppedIdle_ = false;
    if (!dashboardStart(g_dashboardAssets)) fprintf(stderr, "rondo: dashboard: cannot start\n");
  }
  void stop() { dashboardStop(); }
  std::string stateText() const {
    int n = dashboardBrowsers();
    return n == 0 ? "Waiting for a browser\xe2\x80\xa6" : n == 1 ? "1 browser connected" : std::to_string(n) + " browsers connected";
  }
  std::string signature() const {
    return ipAddress() + "|" + (dashboardRunning() ? "1" : "0") + (stoppedIdle_ ? "1" : "0") + "|" + dashboardCode() + "|" +
           std::to_string(dashboardBrowsers());
  }
  // QRCodeWriter at dp(104) pixels, as 1.0.x's qrCode(url, dp(104))
  void makeQr(Env& e, const std::string& url) {
    qrUrl_ = url;
    destroyTexture(qr_);
    ui::QrCode q;
    if (!ui::encodeQr(url, q)) return;
    int size = e.dp(104);
    std::vector<uint8_t> gray = ui::renderQr(q, size), rgba(size_t(size) * size * 4);
    for (size_t i = 0; i < gray.size(); i++) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = gray[i], rgba[i * 4 + 3] = 255;
    qr_ = createTexture(size, size, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data(), true);
  }
  Texture qr_;
  std::string qrUrl_, shown_;
  bool stoppedIdle_ = false;
  int64_t nextTick_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeDashboard() { return std::unique_ptr<Screen>(new DashboardScreen()); }

}  // namespace rondo::shell
