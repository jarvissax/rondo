#include "theme.h"

#include <math.h>

#include <algorithm>

namespace rondo::ui {

Color lit(Color c) {
  float r = ((c >> 16) & 0xff) / 255.f, g = ((c >> 8) & 0xff) / 255.f, b = (c & 0xff) / 255.f;
  float mx = std::max({r, g, b}), mn = std::min({r, g, b});
  float l = (mx + mn) / 2, h = 0, s = 0;
  if (mx != mn) {
    float d = mx - mn;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    h = (mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4) * 60;
  }
  float h2 = fmodf(fmodf(h - 8, 360) + 360, 360);
  float s2 = std::clamp(s * 100 + 12, 0.f, 100.f) / 100;
  float l2 = std::clamp(l * 100 + std::min(20.f, std::max(0.f, 90 - l * 100) * 0.45f), 0.f, 100.f) / 100;
  float a = s2 * std::min(l2, 1 - l2);
  auto f = [&](int n) {
    float k = fmodf(n + h2 / 30, 12);
    return uint32_t(std::clamp((l2 - a * std::max(-1.f, std::min({k - 3, 9 - k, 1.f}))) * 255, 0.f, 255.f));
  };
  return rgb(f(0) << 16 | f(8) << 8 | f(4));
}

Palette paletteFor(const Theme& t) {
  Palette p;
  p.scale = t.textScale;
  p.darkShell = t.appearance == "dark";
  auto pick = [&](uint32_t light, uint32_t dark) { return rgb(p.darkShell ? dark : light); };
  p.shellBg = pick(0xffffff, 0x121214);
  p.barTop = pick(0xfbfbfb, 0x2c2c31);
  p.barBottom = pick(0xcfd1d6, 0x17171a);
  p.barLine = pick(0x9fa2a8, 0x2e2e33);
  p.rowLine = pick(0xe6e6e6, 0x232327);
  p.header = pick(0xf2f2f4, 0x1b1b1e);
  p.text = pick(0x111111, 0xf2f2f4);
  p.muted = pick(0x7a7a7f, 0x8e8e93);
  p.disabled = pick(0xb0b0b4, 0x4a4a4f);
  p.chevron = pick(0x9a9aa0, 0x6e6e73);
  p.icon = pick(0x3b3b40, 0xd0d0d4);
  p.titleShadow = p.darkShell ? 0 : rgb(0xffffff);
  unsigned accent = 0x1c5ccc;
  parseColor(t.accent, &accent);
  p.accent = rgb(accent);
  p.accentTop = lit(p.accent);
  return p;
}

}  // namespace rondo::ui
