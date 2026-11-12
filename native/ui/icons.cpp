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
    case Icon::Clock:  // Settings › Date & time: a clock face
      p.stroke({p.circle(12, 12, 9)}, 2.2f);
      p.stroke({p.poly({12, 12, 12, 6.5f}, false), p.poly({12, 12, 16, 13.5f}, false)}, 2.2f);
      break;
    case Icon::Star: {  // ratings, Top rated: five points
      Contour c{{}, true};
      for (int i = 0; i < 10; i++) {
        float a = float((-90 + i * 36) * M_PI / 180), r = i % 2 ? 4.4f : 10;
        p.lineTo(c, 12 + r * cosf(a), 12.8f + r * sinf(a));
      }
      p.fill({c});
      break;
    }
    case Icon::Photo:  // a landscape in a frame
      p.stroke({p.roundRect(2.5f, 4.5f, 21.5f, 19.5f, 2.5f)}, 2);
      p.fill({p.poly({5, 17.2f, 10, 10.5f, 13.5f, 14.8f, 15.6f, 12.6f, 19, 17.2f}, true)});
      p.fill({p.circle(16, 8.8f, 1.8f)});
      break;
    case Icon::File:  // a page with its corner folded
      p.stroke({p.poly({6, 2.5f, 14, 2.5f, 19, 7.5f, 19, 21.5f, 6, 21.5f}, true)}, 2);
      p.stroke({p.poly({14, 2.5f, 14, 7.5f, 19, 7.5f}, false)}, 2);
      p.stroke({p.poly({9, 12.5f, 16, 12.5f}, false), p.poly({9, 16.5f, 16, 16.5f}, false)}, 1.8f);
      break;
    case Icon::Headphones:
      p.stroke({p.arc(4, 4, 20, 20, 180, 180)}, 2.2f);
      p.stroke({p.poly({4, 12, 4, 15}, false), p.poly({20, 12, 20, 15}, false)}, 2.2f);
      p.fill({p.roundRect(2.5f, 13, 8, 21, 2), p.roundRect(16, 13, 21.5f, 21, 2)});
      break;
    case Icon::Film:  // a strip with a play button
      p.stroke({p.roundRect(3, 3.5f, 21, 20.5f, 2)}, 2);
      p.stroke({p.poly({7.2f, 3.5f, 7.2f, 20.5f}, false), p.poly({16.8f, 3.5f, 16.8f, 20.5f}, false)}, 1.6f);
      p.fill({p.poly({10.2f, 8.8f, 10.2f, 15.2f, 14.6f, 12}, true)});
      break;
    case Icon::People: {  // two people: album artists
      p.fill({p.circle(16.2f, 7.6f, 3.1f)});
      Contour back{{}, true};
      p.moveTo(back, 13, 13.6f);
      p.cubic(back, 17, 11.8f, 21.6f, 13.5f, 22, 19.5f);
      p.lineTo(back, 15.5f, 19.5f);
      p.fill({back});
      p.fill({p.circle(9, 8.6f, 3.8f)});
      Contour front{{}, true};
      p.moveTo(front, 2, 21);
      p.cubic(front, 2.4f, 14.2f, 15.6f, 14.2f, 16, 21);
      p.fill({front});
      break;
    }
    case Icon::Tag:  // genres
      p.stroke({p.poly({3, 3, 11.5f, 3, 21, 12.5f, 12.5f, 21, 3, 11.5f}, true)}, 2.2f);
      p.fill({p.circle(7.6f, 7.6f, 1.8f)});
      break;
    case Icon::Flame: {  // Most played
      Contour c{{}, true};
      p.moveTo(c, 12, 2.2f);
      p.cubic(c, 15.5f, 6.5f, 19.5f, 9.8f, 19, 15);
      p.cubic(c, 18.6f, 19.4f, 15.6f, 21.8f, 12, 21.8f);
      p.cubic(c, 8.4f, 21.8f, 5, 19.4f, 5, 15);
      p.cubic(c, 5, 11.6f, 7.2f, 9.6f, 8.4f, 7.2f);
      p.cubic(c, 9.4f, 9.4f, 10.4f, 10.4f, 11.2f, 11);
      p.cubic(c, 12.2f, 8.2f, 12.6f, 5.4f, 12, 2.2f);
      p.fill({c});
      break;
    }
    case Icon::History: {  // Recently played: a clock face turning back
      p.stroke({p.arc(3.5f, 3.5f, 20.5f, 20.5f, 205, 295)}, 2.2f);
      p.stroke({p.poly({12, 7.5f, 12, 12, 15, 14}, false)}, 2.2f);
      // the arrow at the arc's start, pointing back along it
      float a = float(205 * M_PI / 180), x = 12 + 8.5f * cosf(a), y = 12 + 8.5f * sinf(a);
      float dx = sinf(a), dy = -cosf(a);  // against the arc's direction
      p.fill({p.poly({x + dx * 3.4f, y + dy * 3.4f, x - dy * 3 - dx * 0.6f, y + dx * 3 - dy * 0.6f, x + dy * 3 - dx * 0.6f,
                      y - dx * 3 - dy * 0.6f},
                     true)});
      break;
    }
    case Icon::Sparkle:  // Recently added, smart playlists: a four-point star and a small one
      p.fill({p.poly({11, 3, 13, 10, 20, 12, 13, 14, 11, 21, 9, 14, 2, 12, 9, 10}, true)});
      p.fill({p.poly({19, 2, 19.8f, 4.2f, 22, 5, 19.8f, 5.8f, 19, 8, 18.2f, 5.8f, 16, 5, 18.2f, 4.2f}, true)});
      break;
    case Icon::Stack:  // Cover Flow: a cover facing you between two turned ones
      p.fill({p.poly({2, 7, 6.5f, 5.6f, 6.5f, 16.4f, 2, 15}, true), p.poly({17.5f, 5.6f, 22, 7, 22, 15, 17.5f, 16.4f}, true)});
      p.fill({p.roundRect(8, 4, 16, 17, 1.2f)});
      p.fill({p.rect(8, 18.6f, 16, 20)});
      break;
    case Icon::Tree:  // Organize: folders under one
      p.fill({p.roundRect(2.5f, 2.5f, 10, 7.5f, 1.5f)});
      p.stroke({p.poly({6, 7.5f, 6, 17.5f, 11, 17.5f}, false), p.poly({6, 10.5f, 11, 10.5f}, false)}, 2);
      p.fill({p.roundRect(11.5f, 8, 21, 13, 1.5f), p.roundRect(11.5f, 15, 21, 20, 1.5f)});
      break;
    case Icon::Sliders:  // Sound: three faders
      p.stroke({p.poly({6, 3, 6, 21}, false), p.poly({12, 3, 12, 21}, false), p.poly({18, 3, 18, 21}, false)}, 2);
      p.fill({p.circle(6, 15, 2.8f), p.circle(12, 8, 2.8f), p.circle(18, 13, 2.8f)});
      break;
    case Icon::Gauge:  // Wheel acceleration
      p.stroke({p.arc(3, 4.5f, 21, 22.5f, 180, 180)}, 2.2f);
      p.stroke({p.poly({12, 13.5f, 16.6f, 8}, false)}, 2.4f);
      p.fill({p.circle(12, 13.5f, 2.2f)});
      p.stroke({p.poly({3, 13.5f, 5, 13.5f}, false), p.poly({19, 13.5f, 21, 13.5f}, false), p.poly({12, 4.5f, 12, 6.5f}, false)}, 1.8f);
      break;
    case Icon::Vibrate:  // Wheel vibration: a buzz on both sides
      p.stroke({p.roundRect(8, 3.5f, 16, 20.5f, 2)}, 2);
      p.stroke({p.poly({4.5f, 8, 2.5f, 10, 4.5f, 12, 2.5f, 14, 4.5f, 16}, false),
                p.poly({19.5f, 8, 21.5f, 10, 19.5f, 12, 21.5f, 14, 19.5f, 16}, false)},
               1.6f);
      break;
    case Icon::Wheel:  // Controls: the click wheel and its center button
      p.stroke({p.circle(12, 12, 9)}, 2.2f);
      p.fill({p.circle(12, 12, 3)});
      p.fill({p.circle(12, 5.6f, 1.1f), p.circle(12, 18.4f, 1.1f), p.circle(5.6f, 12, 1.1f), p.circle(18.4f, 12, 1.1f)});
      break;
    case Icon::Chip:  // System
      p.stroke({p.roundRect(6, 6, 18, 18, 2)}, 2);
      p.fill({p.rect(9.5f, 9.5f, 14.5f, 14.5f)});
      p.stroke({p.poly({9.5f, 2.5f, 9.5f, 6}, false), p.poly({14.5f, 2.5f, 14.5f, 6}, false), p.poly({9.5f, 18, 9.5f, 21.5f}, false),
                p.poly({14.5f, 18, 14.5f, 21.5f}, false), p.poly({2.5f, 9.5f, 6, 9.5f}, false), p.poly({2.5f, 14.5f, 6, 14.5f}, false),
                p.poly({18, 9.5f, 21.5f, 9.5f}, false), p.poly({18, 14.5f, 21.5f, 14.5f}, false)},
               1.8f);
      break;
    case Icon::Key:  // the YouTube account
      p.stroke({p.circle(7.5f, 12, 4.5f)}, 2.2f);
      p.stroke({p.poly({12, 12, 21.5f, 12}, false), p.poly({18, 12, 18, 15.5f}, false), p.poly({21, 12, 21, 14.8f}, false)}, 2.2f);
      break;
    case Icon::Diamond:  // Video quality
      p.stroke({p.poly({7, 4, 17, 4, 21.5f, 9.5f, 12, 20.5f, 2.5f, 9.5f}, true)}, 2);
      p.stroke({p.poly({2.5f, 9.5f, 21.5f, 9.5f}, false), p.poly({9.5f, 4, 12, 9.5f, 14.5f, 4}, false), p.poly({12, 9.5f, 12, 19}, false)},
               1.6f);
      break;
    case Icon::Queue:  // Up next: the list and what plays next
      p.stroke({p.poly({3, 6, 16, 6}, false), p.poly({3, 11, 16, 11}, false), p.poly({3, 16, 9.5f, 16}, false)}, 2.2f);
      p.fill({p.poly({13, 13.5f, 13, 21.5f, 20.5f, 17.5f}, true)});
      break;
    case Icon::Seek:  // Scrub: fast forward
      p.fill({p.poly({2.5f, 6, 2.5f, 18, 11.5f, 12}, true), p.poly({12, 6, 12, 18, 21, 12}, true)});
      break;
    case Icon::Download:  // Software update
      p.stroke({p.poly({12, 3, 12, 14.5f}, false), p.poly({7.2f, 10, 12, 14.8f, 16.8f, 10}, false)}, 2.2f);
      p.stroke({p.poly({4, 15, 4, 20.5f, 20, 20.5f, 20, 15}, false)}, 2.2f);
      break;
    case Icon::Sync: {  // Rescan: two arrows chasing each other
      p.stroke({p.arc(4, 4, 20, 20, 205, 125), p.arc(4, 4, 20, 20, 25, 125)}, 2.2f);
      for (float end : {330.f, 150.f}) {
        float a = float(end * M_PI / 180), x = 12 + 8 * cosf(a), y = 12 + 8 * sinf(a);
        float dx = -sinf(a), dy = cosf(a);  // along the arc
        p.fill({p.poly({x + dx * 3.2f, y + dy * 3.2f, x - dy * 2.8f - dx * 0.5f, y + dx * 2.8f - dy * 0.5f, x + dy * 2.8f - dx * 0.5f,
                        y - dx * 2.8f - dy * 0.5f},
                       true)});
      }
      break;
    }
    case Icon::Usb:  // the USB drive: the trident
      p.stroke({p.poly({12, 3.5f, 12, 18}, false), p.poly({12, 14, 6.5f, 10.5f, 6.5f, 7.5f}, false), p.poly({12, 12, 17.5f, 8.5f, 17.5f, 6.5f}, false)},
               2);
      p.fill({p.poly({9.4f, 5, 14.6f, 5, 12, 1.5f}, true)});
      p.fill({p.circle(12, 19.5f, 2.4f), p.circle(6.5f, 6.4f, 1.8f), p.rect(15.8f, 3.4f, 19.2f, 6.8f)});
      break;
    case Icon::Device:  // Display: the Y1, its screen over its wheel
      p.stroke({p.roundRect(5, 1.8f, 19, 22.2f, 3)}, 2);
      p.fill({p.roundRect(7.6f, 4.4f, 16.4f, 10.6f, 1)});
      p.stroke({p.circle(12, 16.2f, 3.2f)}, 1.8f);
      break;
    case Icon::Broadcast:  // Connections: waves both ways
      p.fill({p.circle(12, 12, 2.3f)});
      p.stroke({p.arc(7, 7, 17, 17, 140, 80), p.arc(7, 7, 17, 17, -40, 80), p.arc(2.5f, 2.5f, 21.5f, 21.5f, 140, 80),
                p.arc(2.5f, 2.5f, 21.5f, 21.5f, -40, 80)},
               2.1f);
      break;
    case Icon::Library:  // the main menu's Library: records on a shelf
      p.fill({p.roundRect(3, 4, 7, 20, 1), p.roundRect(9, 4, 13, 20, 1)});
      p.fill({p.poly({15, 5.2f, 18.6f, 4.2f, 22, 18.8f, 18.4f, 19.8f}, true)});
      p.fill({p.rect(2, 21, 22, 22.6f)});
      break;
    case Icon::Type:  // Text size: a big T and a small one
      p.stroke({p.poly({3.5f, 5, 14.5f, 5}, false), p.poly({9, 5, 9, 20}, false)}, 2.4f);
      p.stroke({p.poly({13.5f, 11, 21, 11}, false), p.poly({17.25f, 11, 17.25f, 20}, false)}, 2);
      break;
    case Icon::Menu:  // Main menu: the rows, each with its tile
      p.fill({p.roundRect(2.5f, 4, 7, 8.5f, 1), p.roundRect(2.5f, 9.75f, 7, 14.25f, 1), p.roundRect(2.5f, 15.5f, 7, 20, 1)});
      p.stroke({p.poly({10, 6.25f, 21, 6.25f}, false), p.poly({10, 12, 21, 12}, false), p.poly({10, 17.75f, 21, 17.75f}, false)}, 2);
      break;
    case Icon::Wand:  // smart playlists
      p.stroke({p.poly({4, 20, 14.5f, 9.5f}, false)}, 2.8f);
      p.fill({p.poly({17.5f, 2, 18.6f, 5.4f, 22, 6.5f, 18.6f, 7.6f, 17.5f, 11, 16.4f, 7.6f, 13, 6.5f, 16.4f, 5.4f}, true)});
      p.fill({p.circle(9, 4.5f, 1.2f), p.circle(20, 15, 1.2f)});
      break;
    case Icon::Bolt:  // Amplifier
      p.fill({p.poly({13.5f, 2, 5, 13.5f, 11, 13.5f, 10, 22, 19, 10, 12.8f, 10}, true)});
      break;
    case Icon::Lock:  // Volume limit
      p.fill({p.roundRect(5, 10.5f, 19, 20.5f, 2)});
      p.stroke({p.arc(8, 4, 16, 14, 180, 180), p.poly({8, 9, 8, 10.5f}, false), p.poly({16, 9, 16, 10.5f}, false)}, 2.2f);
      break;
    case Icon::Captions:  // subtitles: a picture with two lines of text on it
      p.stroke({p.roundRect(2.5f, 4.5f, 21.5f, 19.5f, 2.5f)}, 2.2f);
      p.stroke({p.poly({6.5f, 12.5f, 11.5f, 12.5f}, false), p.poly({14, 12.5f, 17.5f, 12.5f}, false),
                p.poly({6.5f, 16, 9.5f, 16}, false), p.poly({12, 16, 17.5f, 16}, false)}, 2);
      break;
    case Icon::Mic:  // lyrics: what's sung
      p.fill({p.roundRect(9, 2.5f, 15, 14, 3)});
      p.stroke({p.arc(5.5f, 4.5f, 18.5f, 17.5f, 0, 180)}, 2);
      p.stroke({p.poly({12, 17.5f, 12, 21}, false), p.poly({8.5f, 21.2f, 15.5f, 21.2f}, false)}, 2);
      break;
    default: break;
  }
}

}  // namespace

Color batteryColor(float level) { return level >= 0.5f ? rgb(0x34c759) : level >= 0.2f ? rgb(0xff9f0a) : rgb(0xff3b30); }

bool Icons::init(Text& text) {
  text_ = &text;
  return FT_Init_FreeType(&ft_) == 0;
}

Text::Mask Icons::mask(Icon icon, int w, int h, int level) {
  if (generation_ != text_->atlasGeneration()) {  // the atlas was cleared
    cache_.clear();
    generation_ = text_->atlasGeneration();
  }
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
  Text::Mask t = text_->addMask(w, h, m.data());
  if (!t.ok) {  // the atlas was full: it was cleared; add again
    generation_ = text_->atlasGeneration();
    cache_.clear();
    t = text_->addMask(w, h, m.data());
  }
  cache_.emplace(key, t);
  return t;
}

void Icons::draw(Canvas& c, Icon icon, Rect r, Color color, int level) {
  int w = int(lroundf(r.w)), h = int(lroundf(r.h));
  if (w <= 0 || h <= 0) return;
  Text::Mask t = mask(icon, w, h, level);
  if (t.ok) c.glyph(text_->atlas(), {r.x, r.y, float(w), float(h)}, {t.u0, t.v0, t.u1 - t.u0, t.v1 - t.v0}, color);
}

namespace {
// Where the charging bolt goes for a battery in r: Bolt's shape is 14 x 20 of its 24 units (x 5..19,
// y 2..22), drawn 72 % of the battery's height tall, its right edge a gap left of the frame, both
// centered on the same line
struct BoltBox {
  float x, y, box;
};
BoltBox boltBox(Rect r) {
  int w = int(lroundf(r.w)), h = int(lroundf(r.h));
  float s = float(std::min(w, h)) / 24, ox = r.x + (w - 24 * s) / 2;
  float tall = floorf(h * 0.72f + 0.5f), k = tall / 20;
  float gap = std::max(1.f, floorf(1.5f * s + 0.5f));
  float right = ox + 0.7f * s - gap;
  return {floorf(right - 19 * k + 0.5f), floorf(r.y + (h - tall) / 2 - 2 * k + 0.5f), floorf(24 * k + 0.5f)};
}
}  // namespace

int Icons::chargeRoom(float w, float h) {
  BoltBox b = boltBox({0, 0, w, h});
  return std::max(0, int(ceilf(-(b.x + 5 * b.box / 24))));
}

void Icons::battery(Canvas& c, Rect r, Color color, float level, bool charging) {
  draw(c, Icon::BatteryFrame, r, color);
  // The charge is a plain rectangle (2.3..18.9 x 8..16 of the 24-unit box): drawn as a shape, so a new
  // level never adds a mask to the glyph atlas (an atlas upload while the last frame uses it costs the
  // next frame a whole refresh).
  int w = int(lroundf(r.w)), h = int(lroundf(r.h));
  float s = float(std::min(w, h)) / 24, ox = r.x + (w - 24 * s) / 2, oy = r.y + (h - 24 * s) / 2;
  float l = std::clamp(level, 0.f, 1.f);
  if (l > 0) c.shape({ox + 2.3f * s, oy + 8 * s, 16.6f * l * s, 8 * s}, 0, charging ? batteryColor(1) : batteryColor(level));
  if (charging) {
    BoltBox b = boltBox(r);
    draw(c, Icon::Bolt, {b.x, b.y, b.box, b.box}, batteryColor(1));  // the charge's green: one sign
  }
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
