#include "icons.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H

namespace rondo::ui {
namespace {

struct Contour {
  std::vector<float> xy;  // in pixels, y down
  bool closed;
};

// One icon's mask, built shape by shape like Android's Canvas: each shape is rasterized on its own and
// laid over the others (source-over in one color).
class Pen {
 public:
  Pen(FT_Library ft, int w, int h, float s, float ox, float oy) : ft_(ft), w_(w), h_(h), s_(s), ox_(ox), oy_(oy) {
    mask.assign(size_t(w) * h, 0);
  }
  std::vector<uint8_t> mask;

  // shapes in the icon's own units (24-unit box, or the logos' grids), mapped by s and the offset
  Contour poly(std::initializer_list<float> pts, bool closed) {
    Contour c{{}, closed};
    for (auto it = pts.begin(); it != pts.end(); it += 2) add(c, it[0], it[1]);
    return c;
  }
  Contour circle(float cx, float cy, float r) {
    Contour c{{}, true};
    int n = std::max(32, int(r * s_ * 4));
    for (int i = 0; i < n; i++) {
      float a = float(i) / n * 2 * float(M_PI);
      add(c, cx + r * cosf(a), cy + r * sinf(a));
    }
    return c;
  }
  // Canvas.drawArc: degrees, 0 at 3 o'clock, clockwise (y down)
  Contour arc(float l, float t, float r, float b, float start, float sweep, bool center = false) {
    Contour c{{}, center};
    float cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
    if (center) add(c, cx, cy);
    int n = std::max(8, int(fabsf(sweep) / 4));
    for (int i = 0; i <= n; i++) {
      float a = (start + sweep * i / n) * float(M_PI) / 180;
      add(c, cx + rx * cosf(a), cy + ry * sinf(a));
    }
    return c;
  }
  Contour rect(float l, float t, float r, float b) { return poly({l, t, r, t, r, b, l, b}, true); }
  Contour roundRect(float l, float t, float r, float b, float rad) {
    Contour c{{}, true};
    auto corner = [&](float cx, float cy, float from) {
      for (int i = 0; i <= 8; i++) {
        float a = (from + 90.f * i / 8) * float(M_PI) / 180;
        add(c, cx + rad * cosf(a), cy + rad * sinf(a));
      }
    };
    corner(r - rad, t + rad, -90);
    corner(r - rad, b - rad, 0);
    corner(l + rad, b - rad, 90);
    corner(l + rad, t + rad, 180);
    return c;
  }
  // cubic Bézier segments appended to c: (x1 y1 x2 y2 x3 y3) from c's last point
  void cubic(Contour& c, float x1, float y1, float x2, float y2, float x3, float y3) {
    float x0 = (c.xy[c.xy.size() - 2] - ox_) / s_, y0 = (c.xy.back() - oy_) / s_;
    for (int i = 1; i <= 12; i++) {
      float t = i / 12.f, u = 1 - t;
      add(c, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
          u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
  }
  void moveTo(Contour& c, float x, float y) { add(c, x, y); }
  void lineTo(Contour& c, float x, float y) { add(c, x, y); }

  void fill(std::vector<Contour> shapes) { over(raster(shapes, false, 0, true)); }
  // width in icon units (scaled), or in pixels with px = true
  void stroke(std::vector<Contour> shapes, float width, bool roundCap = true, bool px = false) {
    std::vector<Contour> open, closed;
    for (auto& c : shapes) (c.closed ? closed : open).push_back(c);
    float w = px ? width : width * s_;
    if (!open.empty()) over(raster(open, true, w, roundCap));
    if (!closed.empty()) over(raster(closed, true, w, roundCap));
  }

 private:
  void add(Contour& c, float x, float y) {
    c.xy.push_back(ox_ + x * s_);
    c.xy.push_back(oy_ + y * s_);
  }
  void over(const std::vector<uint8_t>& m) {
    for (size_t i = 0; i < mask.size(); i++) mask[i] = uint8_t(mask[i] + m[i] * (255 - mask[i]) / 255);
  }
  std::vector<uint8_t> raster(const std::vector<Contour>& shapes, bool stroked, float width, bool roundCap) {
    std::vector<uint8_t> out(mask.size(), 0);
    size_t points = 0;
    for (auto& c : shapes) points += c.xy.size() / 2;
    if (!points) return out;
    FT_Outline outline;
    if (FT_Outline_New(ft_, FT_UInt(points), FT_Int(shapes.size()), &outline) != 0) return out;
    size_t p = 0;
    for (size_t k = 0; k < shapes.size(); k++) {
      for (size_t i = 0; i < shapes[k].xy.size(); i += 2, p++) {
        outline.points[p].x = FT_Pos(lroundf(shapes[k].xy[i] * 64));
        outline.points[p].y = FT_Pos(lroundf((h_ - shapes[k].xy[i + 1]) * 64));  // FreeType's y is up
        outline.tags[p] = FT_CURVE_TAG_ON;
      }
      outline.contours[k] = short(p - 1);
    }
    FT_Outline* toFill = &outline;
    FT_Outline strokedOutline;
    bool haveStroked = false;
    if (stroked) {
      FT_Stroker st;
      FT_Stroker_New(ft_, &st);
      FT_Stroker_Set(st, FT_Fixed(lroundf(width / 2 * 64)), roundCap ? FT_STROKER_LINECAP_ROUND : FT_STROKER_LINECAP_BUTT,
                     FT_STROKER_LINEJOIN_ROUND, 0);
      FT_Stroker_ParseOutline(st, &outline, !shapes[0].closed);
      FT_UInt np = 0, nc = 0;
      FT_Stroker_GetCounts(st, &np, &nc);
      if (np && FT_Outline_New(ft_, np, FT_Int(nc), &strokedOutline) == 0) {
        strokedOutline.n_points = 0;
        strokedOutline.n_contours = 0;
        FT_Stroker_Export(st, &strokedOutline);
        toFill = &strokedOutline;
        haveStroked = true;
      }
      FT_Stroker_Done(st);
    }
    FT_Bitmap bm;
    memset(&bm, 0, sizeof(bm));
    bm.rows = unsigned(h_);
    bm.width = unsigned(w_);
    bm.pitch = w_;
    bm.buffer = out.data();
    bm.pixel_mode = FT_PIXEL_MODE_GRAY;
    bm.num_grays = 256;
    if (!stroked || haveStroked) FT_Outline_Get_Bitmap(ft_, toFill, &bm);
    if (haveStroked) FT_Outline_Done(ft_, &strokedOutline);
    FT_Outline_Done(ft_, &outline);
    return out;
  }

  FT_Library ft_;
  int w_, h_;
  float s_, ox_, oy_;
};

// IconView.onDraw: the 24-unit shapes. Default stroke width 2.2, round caps and joins.
void drawIcon(Pen& p, Icon kind, float level) {
  using V = std::vector<Contour>;
  switch (kind) {
    case Icon::Chevron: p.stroke({p.poly({8, 3, 16, 12, 8, 21}, false)}, 3); break;
    case Icon::Play: p.fill({p.poly({5, 3, 5, 21, 21, 12}, true)}); break;
    case Icon::Pause: p.fill({p.rect(5, 3, 10, 21), p.rect(14, 3, 19, 21)}); break;
    case Icon::Battery:
    case Icon::BatteryFrame:
      p.stroke({p.roundRect(0.7f, 6.5f, 20.5f, 17.5f, 2)}, 1.4f);
      p.fill({p.rect(21.5f, 9.5f, 23.5f, 14.5f)});
      if (kind == Icon::Battery) p.fill({p.rect(2.3f, 8, 2.3f + 16.6f * level, 16)});
      break;
    case Icon::BatteryCharge: p.fill({p.rect(2.3f, 8, 2.3f + 16.6f * level, 16)}); break;
    case Icon::Wifi:
      p.stroke({p.arc(1, 5, 23, 27, 225, 90), p.arc(5, 9, 19, 23, 225, 90), p.arc(9, 13, 15, 19, 225, 90)}, 2.2f);
      p.fill({p.circle(12, 19.5f, 1.6f)});
      break;
    case Icon::Search:
      p.stroke({p.circle(10, 10, 6.5f)}, 2.6f);
      p.stroke({p.poly({15, 15, 21, 21}, false)}, 2.6f);
      break;
    case Icon::Note:
      p.stroke({p.poly({9, 18, 9, 5, 21, 3, 21, 16}, false)}, 1.8f);
      p.stroke({p.circle(6, 18, 3)}, 1.8f);
      p.stroke({p.circle(18, 16, 3)}, 1.8f);
      break;
    case Icon::Shuffle:
      p.stroke({p.poly({2, 6, 6, 6, 16, 18, 21, 18}, false), p.poly({2, 18, 6, 18, 16, 6, 21, 6}, false),
                p.poly({18, 3, 21, 6, 18, 9}, false), p.poly({18, 15, 21, 18, 18, 21}, false)},
               1.8f);
      break;
    case Icon::Repeat:
      p.stroke({p.poly({3, 11, 3, 8, 18, 8}, false), p.poly({15, 5, 18, 8, 15, 11}, false),
                p.poly({21, 13, 21, 16, 6, 16}, false), p.poly({9, 13, 6, 16, 9, 19}, false)},
               1.8f);
      break;
    case Icon::Volume:
      p.fill({p.poly({3, 9, 7, 9, 12, 4, 12, 20, 7, 15, 3, 15}, true)});
      p.stroke({p.arc(10, 7, 20, 17, -55, 110)}, 2);
      break;
    case Icon::Check: p.stroke({p.poly({2, 12.5f, 9, 19.5f, 22, 5}, false)}, 3); break;
    case Icon::Bluetooth: p.stroke({p.poly({6, 7, 17, 17, 12, 21, 12, 3, 17, 7, 6, 17}, false)}, 2); break;
    case Icon::Folder: p.fill({p.poly({2, 6, 9, 6, 11, 8.5f, 22, 8.5f, 22, 19, 2, 19}, true)}); break;
    case Icon::Bars: p.fill({p.rect(3, 10, 7, 21), p.rect(10, 4, 14, 21), p.rect(17, 13, 21, 21)}); break;
    case Icon::Info:
      p.stroke({p.circle(12, 12, 9.5f)}, 2.2f);
      p.fill({p.circle(12, 7.5f, 1.5f)});
      p.fill({p.rect(10.8f, 10.5f, 13.2f, 17.5f)});
      break;
    case Icon::Person: {
      p.fill({p.circle(12, 8, 4.5f)});
      Contour body{{}, true};
      p.moveTo(body, 3.5f, 21);
      p.cubic(body, 4, 14, 20, 14, 20.5f, 21);
      p.fill({body});
      break;
    }
    case Icon::Disc:
      p.stroke({p.circle(12, 12, 8.5f)}, 3);
      p.fill({p.circle(12, 12, 2.2f)});
      break;
    case Icon::Sun: {
      p.fill({p.circle(12, 12, 4.5f)});
      V rays;
      for (int i = 0; i < 8; i++) {
        float a = float(i * 45.0 * M_PI / 180);
        rays.push_back(p.poly({12 + 7.5f * cosf(a), 12 + 7.5f * sinf(a), 12 + 10.5f * cosf(a), 12 + 10.5f * sinf(a)}, false));
      }
      p.stroke(rays, 2.2f);
      break;
    }
    case Icon::Timer:
      p.stroke({p.circle(12, 13, 8.5f)}, 2.2f);
      p.stroke({p.poly({12, 13, 12, 8}, false), p.poly({12, 13, 15.5f, 15}, false), p.poly({9.5f, 2.5f, 14.5f, 2.5f}, false)},
               2.2f);
      break;
    case Icon::Radio:
      p.stroke({p.poly({6, 8, 17, 2.5f}, false)}, 2);
      p.stroke({p.roundRect(2.5f, 8, 21.5f, 21, 3)}, 2);
      p.fill({p.circle(8.5f, 14.5f, 3.2f)});
      p.fill({p.rect(14, 11.5f, 18.5f, 13.1f), p.rect(14, 15.9f, 18.5f, 17.5f)});
      break;
    case Icon::Cloud:
      p.fill({p.circle(7.5f, 14.5f, 4.5f)});
      p.fill({p.circle(12.5f, 10.5f, 6)});
      p.fill({p.circle(17.5f, 14, 4.5f)});
      p.fill({p.roundRect(7.5f, 12.5f, 17.5f, 19, 1)});
      break;
    case Icon::Screen:
      p.stroke({p.roundRect(2.5f, 4, 21.5f, 16.5f, 2)}, 2.2f);
      p.stroke({p.poly({12, 16.5f, 12, 20}, false), p.poly({7.5f, 20.5f, 16.5f, 20.5f}, false)}, 2.2f);
      break;
    case Icon::Gear: {
      p.stroke({p.circle(12, 12, 5.5f)}, 4);
      V teeth;
      for (int i = 0; i < 8; i++) {
        float a = float((i * 45.0 + 22.5) * M_PI / 180);
        teeth.push_back(p.poly({12 + 6.5f * cosf(a), 12 + 6.5f * sinf(a), 12 + 10.5f * cosf(a), 12 + 10.5f * sinf(a)}, false));
      }
      p.stroke(teeth, 4.2f, false);
      break;
    }
    case Icon::Power:
      p.stroke({p.arc(4, 4.5f, 20, 20.5f, -55, 290)}, 2.4f);
      p.stroke({p.poly({12, 2.5f, 12, 11.5f}, false)}, 2.4f);
      break;
    case Icon::Theme:
      p.stroke({p.circle(12, 12, 8.5f)}, 2.2f);
      p.fill({p.arc(3.5f, 3.5f, 20.5f, 20.5f, -90, 180, true)});
      break;
    case Icon::Playlist:
      p.stroke({p.poly({3, 5.5f, 15, 5.5f}, false), p.poly({3, 10.5f, 15, 10.5f}, false), p.poly({3, 15.5f, 10, 15.5f}, false)},
               2.2f);
      p.stroke({p.poly({20, 7, 20, 18}, false)}, 1.8f);
      p.fill({p.circle(17, 18, 3)});
      break;
    case Icon::Plus: p.stroke({p.poly({12, 4.5f, 12, 19.5f}, false), p.poly({4.5f, 12, 19.5f, 12}, false)}, 2.6f); break;
    case Icon::Trash:
      p.stroke({p.poly({4, 7, 20, 7}, false), p.poly({9.5f, 7, 9.5f, 4.5f, 14.5f, 4.5f, 14.5f, 7}, false),
                p.poly({6.5f, 7, 7.4f, 20, 16.6f, 20, 17.5f, 7}, false), p.poly({10.2f, 11, 10.2f, 16.5f}, false),
                p.poly({13.8f, 11, 13.8f, 16.5f}, false)},
               2);
      break;
    case Icon::Refresh:
      p.stroke({p.arc(4, 4, 20, 20, -45, 315)}, 2.2f);
      p.stroke({p.poly({20, 3.8f, 20, 8.5f, 15.3f, 8.5f}, false)}, 2.2f);
      break;
    case Icon::Shorts:
      p.stroke({p.roundRect(5.5f, 2, 18.5f, 22, 3.5f)}, 2.2f);
      p.fill({p.poly({10, 8, 10, 16, 16, 12}, true)});
      break;
    default: break;
  }
}

}  // namespace

Color batteryColor(float level) { return level >= 0.5f ? rgb(0x34c759) : level >= 0.2f ? rgb(0xff9f0a) : rgb(0xff3b30); }

bool Icons::init() { return FT_Init_FreeType(&ft_) == 0; }

const Texture& Icons::mask(Icon icon, int w, int h, int level) {
  auto key = std::make_tuple(int(icon), w, h, level);
  auto it = cache_.find(key);
  if (it != cache_.end()) return it->second;
  std::vector<uint8_t> m;
  if (icon == Icon::MusicLogoDisc || icon == Icon::MusicLogoMarks) {
    // MusicLogo: the official icon's 176-unit grid, fitted to the smaller side
    float s = float(std::min(w, h)), u = s / 176;
    Pen p(ft_, w, h, u, (w - s) / 2, (h - s) / 2);
    if (icon == Icon::MusicLogoDisc) {
      p.fill({p.circle(88, 88, 88)});
    } else {
      p.stroke({p.circle(88, 88, 44)}, std::max(4 * u, 1.25f), true, true);
      p.fill({p.poly({72, 111, 111, 87, 72, 65}, true)});
    }
    m = p.mask;
  } else if (icon == Icon::VideoLogoButton || icon == Icon::VideoLogoPlay) {
    // VideoLogo: the official 28.57 x 20 outline
    float k = std::min(w / 28.57f, h / 20.f);
    Pen p(ft_, w, h, k, (w - 28.57f * k) / 2, (h - 20 * k) / 2);
    if (icon == Icon::VideoLogoButton) {
      Contour b{{}, true};
      p.moveTo(b, 27.9727f, 3.12324f);
      p.cubic(b, 27.6435f, 1.89323f, 26.6768f, 0.926623f, 25.4468f, 0.597366f);
      p.cubic(b, 23.2197f, 0, 14.285f, 0, 14.285f, 0);
      p.cubic(b, 14.285f, 0, 5.35042f, 0, 3.12323f, 0.597366f);
      p.cubic(b, 1.89323f, 0.926623f, 0.926623f, 1.89323f, 0.597366f, 3.12324f);
      p.cubic(b, 0, 5.35042f, 0, 10, 0, 10);
      p.cubic(b, 0, 10, 0, 14.6496f, 0.597366f, 16.8768f);
      p.cubic(b, 0.926623f, 18.1068f, 1.89323f, 19.0734f, 3.12323f, 19.4026f);
      p.cubic(b, 5.35042f, 20, 14.285f, 20, 14.285f, 20);
      p.cubic(b, 14.285f, 20, 23.2197f, 20, 25.4468f, 19.4026f);
      p.cubic(b, 26.6768f, 19.0734f, 27.6435f, 18.1068f, 27.9727f, 16.8768f);
      p.cubic(b, 28.5701f, 14.6496f, 28.5701f, 10, 28.5701f, 10);
      p.cubic(b, 28.5701f, 10, 28.5677f, 5.35042f, 27.9727f, 3.12324f);
      p.fill({b});
    } else {
      p.fill({p.poly({11.4253f, 14.2854f, 18.8477f, 10.0004f, 11.4253f, 5.71533f}, true)});
    }
    m = p.mask;
  } else {
    float s = std::min(w, h) / 24.f;
    Pen p(ft_, w, h, s, (w - 24 * s) / 2, (h - 24 * s) / 2);
    drawIcon(p, icon, std::clamp(level / 100.f, 0.f, 1.f));
    m = p.mask;
  }
  Texture t = createTexture(w, h, GL_ALPHA, GL_UNSIGNED_BYTE, m.data(), false);
  return cache_.emplace(key, t).first->second;
}

void Icons::draw(Canvas& c, Icon icon, Rect r, Color color, int level) {
  int w = int(lroundf(r.w)), h = int(lroundf(r.h));
  if (w <= 0 || h <= 0) return;
  const Texture& t = mask(icon, w, h, level);
  c.glyph(t, {r.x, r.y, float(w), float(h)}, {0, 0, 1, 1}, color);
}

void Icons::battery(Canvas& c, Rect r, Color color, float level) {
  int pct = int(lroundf(std::clamp(level, 0.f, 1.f) * 100));
  draw(c, Icon::BatteryFrame, r, color);
  draw(c, Icon::BatteryCharge, r, batteryColor(level), pct);
}

void Icons::musicLogo(Canvas& c, Rect r) {
  draw(c, Icon::MusicLogoDisc, r, rgb(0xff0000));
  draw(c, Icon::MusicLogoMarks, r, rgb(0xffffff));
}

void Icons::videoLogo(Canvas& c, Rect r) {
  draw(c, Icon::VideoLogoButton, r, rgb(0xff0000));
  draw(c, Icon::VideoLogoPlay, r, rgb(0xffffff));
}

}  // namespace rondo::ui
