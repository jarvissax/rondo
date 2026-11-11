// Library › Cover Flow: the albums' covers side by side in perspective, the one in the middle facing
// you and the others turned toward it, each above its reflection, as iTunes and the iPod showed them.
// The wheel glides through them, Next and Prev jump ten, center opens the album's songs.
#include <math.h>

#include <algorithm>
#include <memory>

#include "app.h"
#include "screens.h"

namespace rondo::shell {

namespace {

constexpr ui::Color white = 0xffffffff;
inline ui::Color whiteA(int a) { return uint32_t(a) << 24 | 0xffffff; }

class CoverFlowScreen : public Screen {
 public:
  CoverFlowScreen() : albums_(g_library.albums()) {}
  std::string title() const override { return "Cover Flow"; }

  bool onAction(Action a, int steps) override {
    int n = int(albums_.size());
    if (!n) return false;
    switch (a) {
      case Action::Up: target_ = std::max(0, target_ - std::max(1, steps)); return true;
      case Action::Down: target_ = std::min(n - 1, target_ + std::max(1, steps)); return true;
      case Action::Prev: target_ = std::max(0, target_ - 10); return true;
      case Action::Next: target_ = std::min(n - 1, target_ + 10); return true;
      case Action::Select: {
        const Library::Group& g = albums_[size_t(target_)];
        g_app->push(makeSongList(g.first, g.second));
        return true;
      }
      default: return false;
    }
  }

  // a glide toward the album picked: fast at first, settling (critically damped)
  bool step(float dt) override {
    float d = float(target_) - pos_;
    if (std::fabs(d) < 0.002f && std::fabs(vel_) < 0.002f) {
      pos_ = float(target_), vel_ = 0;
      return false;
    }
    const float k = 120.f, c = 2.f * sqrtf(k);
    vel_ += (k * d - c * vel_) * dt;
    pos_ += vel_ * dt;
    return true;
  }

  void draw(Env& e, float w, float h) override {
    e.c.fill({0, 0, w, h}, rgb(0x000000));
    e.c.fillGradient({0, 0, w, h * 0.62f}, rgb(0x15161a), rgb(0x000000));
    if (albums_.empty()) {
      Label l = label(e, "No albums on the card", 15, false, w);
      drawLabel(e, l, (w - l.w) / 2, (h - l.h) / 2, whiteA(180));
      return;
    }
    float size = float(e.dp(140));        // the cover in the middle, forward
    float cx = w / 2, cy = float(e.dp(12)) + size / 2;
    // the middle's neighbors well clear of it, then each further one a little apart: a wide stage
    float gapMid = size * 0.8f, spacing = size * 0.33f;
    // the stage: a soft light behind the middle cover, the floor dark under the reflections
    e.c.layer(1);
    for (int k = 0; k < 6; k++) {
      float r = size * (1.25f - k * 0.16f);
      e.c.shape({cx - r, cy - r * 0.8f, 2 * r, 1.6f * r}, r, ui::Color(uint32_t(3 + k) << 24 | 0x8090b0));
    }
    int n = int(albums_.size()), at = int(floorf(pos_ + 0.5f));
    // back to front: the furthest first, the middle one last
    std::vector<int> order;
    for (int i = std::max(0, at - 7); i <= std::min(n - 1, at + 7); i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [this](int a, int b) { return std::fabs(a - pos_) > std::fabs(b - pos_); });
    e.c.layer(2);  // one layer, back to front: a layer keeps its quads' order
    for (int i : order) drawCover(e, i, cx, cy, size, gapMid, spacing, w);
    // the album in the middle
    e.c.layer(6);
    const Library::Group& g = albums_[size_t(std::max(0, std::min(n - 1, target_)))];
    const Track* t = g.second.empty() ? nullptr : g.second.front();
    std::string by = t ? (!t->albumArtist.empty() ? t->albumArtist : t->artist) : "";
    int year = Library::yearOf(g.second);
    if (year) by += (by.empty() ? "" : " \xc2\xb7 ") + std::to_string(year);
    by += (by.empty() ? "" : "   \xc2\xb7   ") + std::to_string(std::max(0, std::min(n - 1, target_)) + 1) + " of " + std::to_string(n);
    Label title = label(e, g.first, 15, true, w - e.dp(24));
    Label sub = label(e, by, 12, false, w - e.dp(24));
    float ty = h - float(e.dp(10)) - title.h - sub.h - e.dp(2);
    drawLabel(e, title, (w - title.w) / 2, ty, white);
    drawLabel(e, sub, (w - sub.w) / 2, ty + title.h + e.dp(2), whiteA(170));
  }

 private:
  // One cover: turned toward the middle by up to 64 degrees and set back from it, in perspective, as
  // vertical strips (the canvas draws rectangles: strips each their own height make the trapezoid), and
  // its reflection below, upside down and dimmer.
  void drawCover(Env& e, int i, float cx, float cy, float size, float gapMid, float spacing, float w) {
    float d = float(i) - pos_, ad = std::fabs(d), side = d < 0 ? -1.f : 1.f;
    float turn = std::min(1.f, ad) * 1.117f;  // 64 degrees
    float back = std::min(1.f, ad) * size * 0.55f;  // the sides stand behind the middle one
    float off = ad < 1 ? ad * gapMid : gapMid + (ad - 1) * spacing;
    float mx = cx + side * off;
    if (mx + size < 0 || mx - size > w) return;
    const Track* t = albums_[size_t(i)].second.empty() ? nullptr : albums_[size_t(i)].second.front();
    float alpha = 1;
    const ui::Texture* tex = t ? e.covers.get(t->path, 200, 200, 0, e.now, &alpha) : nullptr;
    float focal = size * 2.0f, half = size / 2;
    const int strips = 24;  // narrow enough that the edges look straight
    // what's further away dims a little: a gray tint (the canvas premultiplies)
    float shade = 1.f - std::min(0.5f, ad * 0.11f);
    auto tint = [](float gray, float a) {
      uint32_t g = uint32_t(std::max(0.f, std::min(1.f, gray)) * 255.f + 0.5f), al = uint32_t(std::max(0.f, std::min(1.f, a)) * 255.f + 0.5f);
      return ui::Color(al << 24 | g << 16 | g << 8 | g);
    };
    for (int s = 0; s < strips; s++) {
      float x0 = -half + size * s / strips, x1 = -half + size * (s + 1) / strips;
      // a right-hand cover's inner (left) edge turns away from you, its outer edge toward you
      float z0 = -side * x0 * sinf(turn) + back, z1 = -side * x1 * sinf(turn) + back;
      float s0 = focal / (focal + z0), s1 = focal / (focal + z1);
      float px0 = mx + x0 * cosf(turn) * s0, px1 = mx + x1 * cosf(turn) * s1;
      float hh = half * (s0 + s1) / 2;
      if (px1 <= px0) continue;
      float sw = px1 - px0 + 0.6f;  // a hair wider: no seams between strips
      ui::Rect dst{px0, cy - hh, sw, hh * 2};
      ui::Rect uv{float(s) / strips, 0, 1.f / strips, 1};
      if (tex) e.c.image(*tex, dst, uv, 0, tint(shade, alpha));
      else e.c.fill(dst, tint(0.24f * shade, 1));
      // the reflection: the cover's lower half upside down, fading out in four bands
      static const float kFade[4] = {0.30f, 0.17f, 0.08f, 0.03f};
      for (int b = 0; b < 4; b++) {
        float bh = hh / 4;  // half the cover's height in all
        ui::Rect r{px0, cy + hh + b * bh, sw, bh};
        // a band of the flipped picture: rows from the bottom up
        ui::Rect fuv{float(s) / strips, 1.f - b * 0.125f, 1.f / strips, -0.125f};
        if (tex) e.c.image(*tex, r, fuv, 0, tint(shade * kFade[b], alpha * kFade[b]));
        else e.c.fill(r, tint(0.24f * shade * kFade[b], kFade[b]));
      }
    }
  }

  std::vector<Library::Group> albums_;
  int target_ = 0;
  float pos_ = 0, vel_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeCoverFlow() { return std::unique_ptr<Screen>(new CoverFlowScreen()); }

}  // namespace rondo::shell
