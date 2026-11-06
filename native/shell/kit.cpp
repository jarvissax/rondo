#include "kit.h"

#include <stdio.h>

#include <algorithm>

namespace rondo::shell {

Label label(Env& e, const std::string& s, float sp, bool bold, float maxW) {
  Label l;
  l.font = e.font(sp, bold);
  l.run = &e.text.run(l.font, s, maxW > 0 ? maxW : 0);
  l.w = int(ceilf(l.run->width));
  l.h = e.text.lineHeight(l.font);
  return l;
}

void drawLabel(Env& e, const Label& l, float x, float top, Color c) {
  if (l.run) e.text.draw(e.c, *l.run, x, top + e.text.ascentInt(l.font), c);
}

namespace tiles {
const Tile music{Icon::Note, rgb(0x3a3a3a), rgb(0x141414), 2};
const Tile video{Icon::Play, rgb(0x3a3a3a), rgb(0x141414), 1};
const Tile device{Icon::Folder, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile nowPlaying{Icon::Bars, rgb(0x7be07b), rgb(0x26a83b)};
const Tile settings{Icon::Gear, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile wifi{Icon::Wifi, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile bluetooth{Icon::Bluetooth, rgb(0x6a8cff), rgb(0x2c4fd6)};
const Tile quality{Icon::Play, rgb(0xffb04d), rgb(0xf07a12)};
const Tile server{Icon::Cloud, rgb(0x7be07b), rgb(0x26a83b)};
const Tile about{Icon::Info, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile songs{Icon::Note, rgb(0xff5e7e), rgb(0xe0144c)};
const Tile artists{Icon::Person, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile albums{Icon::Disc, rgb(0xffb04d), rgb(0xf07a12)};
const Tile dashboard{Icon::Screen, rgb(0x3fd0c9), rgb(0x0f8f96)};
const Tile brightness{Icon::Sun, rgb(0xffd24d), rgb(0xf0a012)};
const Tile timer{Icon::Timer, rgb(0x9a8cff), rgb(0x5a3fe0)};
const Tile radio{Icon::Radio, rgb(0xffb04d), rgb(0xf07a12)};
const Tile lyrics{Icon::Note, rgb(0xff8fc8), rgb(0xd6338a)};
const Tile sound{Icon::Volume, rgb(0x7be07b), rgb(0x26a83b)};
const Tile accel{Icon::Bars, rgb(0x6a8cff), rgb(0x2c4fd6)};
const Tile wheel{Icon::Disc, rgb(0x6a8cff), rgb(0x2c4fd6)};
const Tile videos{Icon::Play, rgb(0xff6b5a), rgb(0xe01e1e)};
const Tile shuffle{Icon::Shuffle, rgb(0x7be07b), rgb(0x26a83b)};
const Tile power{Icon::Power, rgb(0xff7a7a), rgb(0xe0283c)};
const Tile restart{Icon::Refresh, rgb(0xffb04d), rgb(0xf07a12)};
const Tile theme{Icon::Theme, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile playlists{Icon::Playlist, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile add{Icon::Plus, rgb(0x7be07b), rgb(0x26a83b)};
const Tile search{Icon::Search, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile recent{Icon::Timer, rgb(0x3fd0c9), rgb(0x0f8f96)};
const Tile del{Icon::Trash, rgb(0xff7a7a), rgb(0xe0283c)};
const Tile rescan{Icon::Refresh, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile shorts{Icon::Shorts, rgb(0xff6b5a), rgb(0xe01e1e)};
const Tile update{Icon::Refresh, rgb(0x5fb7ff), rgb(0x1f73e0)};
}  // namespace tiles

// IconTile: the gradient tile (radius 7 units) with a 16-unit white glyph, or the white brand tile with
// a 1 px edge at 11 % black and the logo.
void drawTile(Env& e, const Tile& t, Rect r, float alpha) {
  e.c.pushAlpha(alpha);
  float radius = e.dpf(7);
  int layer = e.c.currentLayer();
  if (!t.brand) {
    e.c.shape(r, radius, t.top, t.bottom);
    int g = e.dp(16);
    e.c.layer(layer + 1);
    e.icons.draw(e.c, t.kind, {r.x + float((int(r.w) - g) / 2), r.y + float((int(r.h) - g) / 2), float(g), float(g)},
                 rgb(0xffffff));
  } else {
    // the stroke lies over the white fill: an 11 % black edge on white is #e3e3e3
    e.c.shape(r, radius, rgb(0xe3e3e3));
    e.c.shape({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, radius - 1, rgb(0xffffff));
    e.c.layer(layer + 1);
    int w = e.dp(19), h = t.brand == 1 ? e.dp(14) : e.dp(19);
    Rect lr{r.x + float((int(r.w) - w) / 2), r.y + float((int(r.h) - h) / 2), float(w), float(h)};
    if (t.brand == 1) e.icons.videoLogo(e.c, lr);
    else e.icons.musicLogo(e.c, lr);
  }
  e.c.layer(layer);
  e.c.popAlpha();
}

void fillDiagonal(Canvas& c, Rect r, Color c0, Color c1, Color c2) {
  // LinearGradient from the top-left to the bottom-right corner: t is the projection on the diagonal
  auto at = [&](float x, float y) {
    float t = ((x - r.x) * r.w + (y - r.y) * r.h) / (r.w * r.w + r.h * r.h);
    return t < 0.5f ? mix(c0, c1, t * 2) : mix(c1, c2, (t - 0.5f) * 2);
  };
  const int n = 12;
  for (int j = 0; j < n; j++) {
    for (int i = 0; i < n; i++) {
      float x0 = r.x + r.w * i / n, x1 = r.x + r.w * (i + 1) / n, y0 = r.y + r.h * j / n, y1 = r.y + r.h * (j + 1) / n;
      c.fillCorners({x0, y0, x1 - x0, y1 - y0}, at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1));
    }
  }
}

void statusBar(Env& e, float w, const Status& s, const std::string& title, const std::string& title2, float t) {
  const Palette& p = e.p;
  float H = float(e.dp(22));
  int layer = e.c.currentLayer();
  e.c.fillGradient({0, 0, w, H}, p.barTop, p.barBottom);
  e.c.fill({0, H - 1, w, 1}, p.barLine);
  e.c.layer(layer + 1);
  if (s.playing) {
    int sz = e.dp(12);
    e.icons.draw(e.c, Icon::Play, {float(e.dp(10)), float((int(H) - sz) / 2), float(sz), float(sz)}, p.accent);
  }
  auto drawTitle = [&](const std::string& text, float alpha) {
    if (text.empty() || alpha <= 0) return;
    Label l = label(e, text, 15, true);
    float x = float((int(w) - l.w) / 2), top = float((int(H) - l.h) / 2);
    e.c.pushAlpha(alpha);
    if (p.titleShadow >> 24) drawLabel(e, l, x, top + 1, p.titleShadow);  // setShadowLayer(1, 0, 1, white)
    drawLabel(e, l, x, top, p.text);
    e.c.popAlpha();
  };
  drawTitle(title, 1 - t);
  drawTitle(title2, t);
  // Wi-Fi (dimmed offline), the percentage and the battery, 10 units from the right
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", int(floorf(s.battery * 100 + 0.5f)));
  Label pl = label(e, pct, 11, true);
  int wifi = e.dp(14), bw = e.dp(24), bh = e.dp(14);
  int gw = wifi + e.dp(6) + pl.w + e.dp(3) + bw, gh = std::max({wifi, pl.h, bh});
  float gx = w - e.dp(10) - gw, gy = float((int(H) - gh) / 2);
  e.c.pushAlpha(s.online ? 1 : 0.25f);
  e.icons.draw(e.c, Icon::Wifi, {gx, gy + (gh - wifi) / 2, float(wifi), float(wifi)}, p.icon);
  e.c.popAlpha();
  drawLabel(e, pl, gx + wifi + e.dp(6), gy + (gh - pl.h) / 2, p.icon);
  e.icons.battery(e.c, {gx + wifi + e.dp(6) + pl.w + e.dp(3), gy + (gh - bh) / 2, float(bw), float(bh)}, p.icon, s.battery);
  e.c.layer(layer);
}

namespace {
// Kotlin's uppercase() for the header titles (ASCII and Latin-1 are what they hold)
std::string upper(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'a' && c <= 'z') {
      o += char(c - 32);
    } else if (c == 0xc3 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xa0 && (unsigned char)s[i + 1] <= 0xbe &&
               (unsigned char)s[i + 1] != 0xb7) {
      o += char(c), o += char((unsigned char)s[++i] - 0x20);
    } else {
      o += char(c);
    }
  }
  return o;
}
}  // namespace

std::string formatTime(int s) {
  char b[16];
  if (s < 0) s = 0;
  if (s >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
  return b;
}

int RowList::firstSelectable() const {
  for (int i = 0; i < int(rows_.size()); i++)
    if (selectable(i)) return i;
  return -1;
}

void RowList::setRows(std::vector<Row> rows, bool keepSelection) {
  int previous = selected_;
  rows_ = std::move(rows);
  message_ = rows_.empty() ? "Nothing here yet" : "";
  selected_ = keepSelection && selectable(previous) ? previous : firstSelectable();
  top_.clear();
  placed_ = false;
  if (onSelectionChanged) onSelectionChanged();
}

void RowList::layout(Env& e, float w, float h) {
  entryH_ = float(e.dp(36));
  headerH_ = float(e.dp(20));
  bool resized = w != w_ || h != h_;
  w_ = w, h_ = h;
  if (top_.size() != rows_.size() || resized) {
    top_.resize(rows_.size());
    float y = 0;
    for (size_t i = 0; i < rows_.size(); i++) {
      top_[i] = y;
      y += rowH(int(i));
    }
    total_ = y;
    if (!placed_) {
      target_ = 0;
      ensureVisible();
      scroll_.snap(target_);
      bar_.snap(selected_ >= 0 ? top_[selected_] : 0);
      placed_ = true;
    }
  }
}

// RowList.ensureVisible(): the first row puts the list at its top; a selection on the first or last
// fully visible row brings it to the middle ((height - row height) / 2 from the top).
void RowList::ensureVisible() {
  if (selected_ < 0 || top_.size() != rows_.size()) return;
  if (selected_ == firstSelectable()) {
    target_ = 0;
  } else {
    int first = -1, last = -1;
    for (int i = 0; i < int(rows_.size()); i++) {
      if (top_[i] >= target_ - 0.5f && first < 0) first = i;
      if (top_[i] + rowH(i) <= target_ + h_ + 0.5f) last = i;
    }
    if (selected_ <= first || selected_ >= last) target_ = top_[selected_] - float((int(h_) - int(entryH_)) / 2);
  }
  // RecyclerView leaves no gap at either end
  target_ = std::max(0.f, std::min(target_, std::max(0.f, total_ - h_)));
}

void RowList::selectIndex(int i) {
  if (!selectable(i)) return;
  int old = selected_;
  selected_ = i;
  ensureVisible();
  if (old != i && onSelectionChanged) onSelectionChanged();
}

void RowList::move(int dir, int steps) {
  int i = selected_;
  for (int k = 0; k < steps; k++) {
    int j = i + dir;
    while (j >= 0 && j < int(rows_.size()) && !selectable(j)) j += dir;
    if (j >= 0 && j < int(rows_.size())) i = j;
  }
  selectIndex(i);
}

bool RowList::down(int steps) {
  move(1, steps);
  return true;
}

bool RowList::up(int steps) {
  if (selected_ == firstSelectable() || selected_ < 0) return true;
  move(-1, steps);
  return true;
}

bool RowList::select() {
  const Row* r = selectedRow();
  if (!r || !r->onSelect) return false;
  r->onSelect();
  return true;
}

bool RowList::longSelect() {
  const Row* r = selectedRow();
  if (!r || !r->onLongSelect) return false;
  r->onLongSelect();
  return true;
}

bool RowList::step(float dt) {
  if (selected_ < 0 || top_.size() != rows_.size()) return false;
  // 1.0.x jumps; here the list and the highlight glide to the same places (the selection glide)
  bool a = scroll_.step(target_, dt, 43);
  bool b = bar_.step(top_[selected_], dt, 43);
  return a || b;
}

void RowList::draw(Env& e, float x, float y) {
  const Palette& p = e.p;
  Canvas& c = e.c;
  c.pushClip({x, y, w_, h_});
  if (rows_.empty()) {
    c.layer(3);
    Label l = label(e, message_, 15, false, w_ - 2 * e.dp(20));
    drawLabel(e, l, x + float((int(w_) - l.w) / 2), y + float((int(h_) - l.h) / 2), p.muted);
    c.popClip();
    return;
  }
  float scroll = scroll_.x;
  float H = entryH_;
  if (selected_ >= 0) {
    c.layer(1);
    c.fillGradient({x, y + bar_.x - scroll, w_, H}, p.accentTop, p.accent);
  }
  for (int i = 0; i < int(rows_.size()); i++) {
    float top = y + top_[i] - scroll;
    if (top + rowH(i) < y || top > y + h_) continue;
    const Row& r = rows_[i];
    if (r.header) {
      c.layer(0);
      c.fill({x, top, w_, headerH_}, p.header);
      c.layer(3);
      Label l = label(e, upper(r.title), 11, true);
      drawLabel(e, l, x + e.dp(12), top + headerH_ - e.dp(3) - l.h, p.muted);
      continue;
    }
    // how much of this row the highlight covers: its text turns white as the highlight passes
    float sel = selected_ >= 0 ? clamp01(1 - fabsf(bar_.x - top_[i]) / H) : 0;
    c.layer(0);
    c.fill({x + e.dp(8), top + H - 1, w_ - e.dp(8), 1}, p.rowLine);
    int padL = e.dp(8), padR = e.dp(10);
    bool hasTile = r.tile || !r.thumb.empty();
    int tileW = hasTile ? e.dp(26) + e.dp(9) : 0;
    Label tr = label(e, r.trailing, 12, false);
    int trailW = e.dp(6) + tr.w;
    int chevW = r.chevron ? e.dp(5) + e.dp(8) : 0;
    float colW = w_ - padL - padR - tileW - trailW - chevW;
    Label tl = label(e, r.title, r.subtitle.empty() ? 15 : 13.5f, sel > 0.5f, colW);
    Label sl;
    if (!r.subtitle.empty()) sl = label(e, r.subtitle, 11, false, colW);
    int colH = tl.h + (r.subtitle.empty() ? 0 : e.dp(1) + sl.h);
    float x0 = x + padL;
    if (hasTile) {
      int t = e.dp(26);
      Rect tr2{x0, top + float((int(H) - t) / 2), float(t), float(t)};
      c.layer(2);
      if (r.tile) {
        drawTile(e, *r.tile, tr2, r.enabled ? 1 : 0.4f);
      } else {
        float a = 1;
        const Texture* tex = e.covers.get(r.thumb, t, t, 0, e.now, &a);
        if (!tex || a < 1) c.shape(tr2, e.dpf(4), p.rowLine);
        c.layer(3);
        if (tex) c.image(*tex, tr2, {0, 0, 1, 1}, e.dpf(4), withAlpha(0xffffffff, a));
      }
    }
    c.layer(4);
    float tx = x0 + tileW, colTop = top + float((int(H) - colH) / 2);
    Color ink = mix(r.enabled ? p.text : p.disabled, p.white, sel);
    drawLabel(e, tl, tx, colTop, ink);
    if (!r.subtitle.empty()) drawLabel(e, sl, tx, colTop + tl.h + e.dp(1), mix(p.muted, 0xdcffffff, sel));
    float trx = x + w_ - padR - chevW - tr.w;
    drawLabel(e, tr, trx, top + float((int(H) - tr.h) / 2), mix(r.enabled ? p.muted : p.disabled, p.white, sel));
    if (r.chevron) {
      int cw = e.dp(8), ch = e.dp(12);
      e.icons.draw(c, Icon::Chevron, {x + w_ - padR - cw, top + float((int(H) - ch) / 2), float(cw), float(ch)},
                   mix(p.chevron, p.white, sel));
    }
  }
  c.layer(0);
  c.popClip();
}

}  // namespace rondo::shell
