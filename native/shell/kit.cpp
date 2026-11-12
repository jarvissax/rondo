#include "kit.h"

#include "../core/fold.h"

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
// the brands
const Tile music{Icon::Note, rgb(0x3a3a3a), rgb(0x141414), 2};
const Tile video{Icon::Play, rgb(0x3a3a3a), rgb(0x141414), 1};
// the main menu
const Tile nowPlaying{Icon::Bars, rgb(0x7be07b), rgb(0x26a83b)};
const Tile device{Icon::Library, rgb(0x5fb7ff), rgb(0x1f73e0)};  // Library
const Tile radio{Icon::Radio, rgb(0xffb04d), rgb(0xf07a12)};
const Tile settings{Icon::Gear, rgb(0xb8bcc4), rgb(0x6f747d)};
// Settings: its groups
const Tile dashboard{Icon::Screen, rgb(0x3fd0c9), rgb(0x0f8f96)};
const Tile connections{Icon::Broadcast, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile equalizer{Icon::Sliders, rgb(0xff7a93), rgb(0xd6304f)};  // Sound
const Tile display{Icon::Device, rgb(0x8c7bff), rgb(0x4b36d6)};
const Tile controls{Icon::Wheel, rgb(0xffb04d), rgb(0xf07a12)};
const Tile system{Icon::Chip, rgb(0x9aa0a8), rgb(0x5b616a)};
// Connections
const Tile wifi{Icon::Wifi, rgb(0x4fc3f7), rgb(0x0288d1)};
const Tile bluetooth{Icon::Bluetooth, rgb(0x6a8cff), rgb(0x2c4fd6)};
// YouTube
const Tile server{Icon::Cloud, rgb(0x7be07b), rgb(0x26a83b)};
const Tile account{Icon::Key, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile quality{Icon::Diamond, rgb(0xffb04d), rgb(0xf07a12)};
// Sound
const Tile output{Icon::Headphones, rgb(0xff7a93), rgb(0xd6304f)};
// Display, and its Theme
const Tile theme{Icon::Theme, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile subtitles{Icon::Captions, rgb(0x5fe0b5), rgb(0x10a37a)};  // Display › Subtitles
const Tile downloads{Icon::Download, rgb(0x7be07b), rgb(0x26a83b)};  // Library › Videos › YouTube downloads
const Tile brightness{Icon::Sun, rgb(0xffd24d), rgb(0xf0a012)};
const Tile timer{Icon::Timer, rgb(0x8fa3b8), rgb(0x52657a)};
const Tile lyrics{Icon::Mic, rgb(0xff8fc8), rgb(0xd6338a)};
const Tile textSize{Icon::Type, rgb(0x4fd8c8), rgb(0x139c8c)};
const Tile menuOrder{Icon::Menu, rgb(0x6a8cff), rgb(0x2c4fd6)};
// Controls
const Tile accel{Icon::Gauge, rgb(0x6a8cff), rgb(0x2c4fd6)};
const Tile wheel{Icon::Vibrate, rgb(0x4fd8c8), rgb(0x139c8c)};
const Tile sound{Icon::Volume, rgb(0x7be07b), rgb(0x26a83b)};  // Click sound
// System
const Tile clock{Icon::Clock, rgb(0x4fd8c8), rgb(0x139c8c)};
const Tile update{Icon::Download, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile about{Icon::Info, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile power{Icon::Power, rgb(0xff7a7a), rgb(0xe0283c)};
const Tile restart{Icon::Refresh, rgb(0xffb04d), rgb(0xf07a12)};
// Library: its sections
const Tile musicSection{Icon::Headphones, rgb(0xff5e7e), rgb(0xe0144c)};
const Tile videos{Icon::Film, rgb(0xff6b5a), rgb(0xe01e1e)};
const Tile photos{Icon::Photo, rgb(0xffd24d), rgb(0xf0a012)};
const Tile files{Icon::File, rgb(0x5fb7ff), rgb(0x1f73e0)};
// Library › Music
const Tile shuffle{Icon::Shuffle, rgb(0x7be07b), rgb(0x26a83b)};
const Tile search{Icon::Search, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile playlists{Icon::Playlist, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile songs{Icon::Note, rgb(0xff5e7e), rgb(0xe0144c)};
const Tile artists{Icon::Person, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile albumArtists{Icon::People, rgb(0x9a8cff), rgb(0x5a3fe0)};
const Tile albums{Icon::Disc, rgb(0xffb04d), rgb(0xf07a12)};
const Tile coverFlow{Icon::Stack, rgb(0x8fa3b8), rgb(0x52657a)};
const Tile genres{Icon::Tag, rgb(0xff8fc8), rgb(0xd6338a)};
const Tile folders{Icon::Folder, rgb(0x4fc3f7), rgb(0x0288d1)};
const Tile recent{Icon::Sparkle, rgb(0x3fd0c9), rgb(0x0f8f96)};  // Recently added
const Tile mostPlayed{Icon::Flame, rgb(0xff8a65), rgb(0xf4511e)};
const Tile played{Icon::History, rgb(0x8c7bff), rgb(0x4b36d6)};  // Recently played
const Tile topRated{Icon::Star, rgb(0xf5d76e), rgb(0xc99a00)};
const Tile rescan{Icon::Sync, rgb(0x9aa0a8), rgb(0x5b616a)};
const Tile organize{Icon::Tree, rgb(0xd4a373), rgb(0x9c6a3a)};
// playlists, Up next
const Tile smart{Icon::Wand, rgb(0xe28cff), rgb(0xb03fe0)};
const Tile add{Icon::Plus, rgb(0x7be07b), rgb(0x26a83b)};
const Tile del{Icon::Trash, rgb(0xff7a7a), rgb(0xe0283c)};
const Tile repeat{Icon::Repeat, rgb(0x6a8cff), rgb(0x2c4fd6)};
// Files' actions
const Tile openFile{Icon::Chevron, rgb(0x7be07b), rgb(0x26a83b)};
const Tile newFolder{Icon::Plus, rgb(0x4fc3f7), rgb(0x0288d1)};
const Tile renameTo{Icon::Type, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile moveTo{Icon::Tree, rgb(0xffb04d), rgb(0xf07a12)};
const Tile copyTo{Icon::Stack, rgb(0x8c7bff), rgb(0x4b36d6)};
// Settings › Sound (Playing on is output)
const Tile eqPreset{Icon::Sliders, rgb(0xffb04d), rgb(0xf07a12)};
const Tile eqEdit{Icon::Bars, rgb(0xc58cff), rgb(0x7b3fe0)};
const Tile match{Icon::Search, rgb(0xb8bcc4), rgb(0x6f747d)};
const Tile boost{Icon::Volume, rgb(0x7be07b), rgb(0x26a83b)};
const Tile leveling{Icon::Gauge, rgb(0x4fd8c8), rgb(0x139c8c)};
const Tile amplifier{Icon::Bolt, rgb(0xffd24d), rgb(0xf0a012)};
const Tile limit{Icon::Lock, rgb(0xff7a7a), rgb(0xe0283c)};
// the rest
const Tile usb{Icon::Usb, rgb(0x5fb7ff), rgb(0x1f73e0)};
const Tile shorts{Icon::Shorts, rgb(0xff6b5a), rgb(0xe01e1e)};
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
    e.c.layer(layer + 2);  // with the text: icons are in the glyph atlas
    if (t.glyph) e.icons.draw(e.c, t.kind, {r.x + float((int(r.w) - g) / 2), r.y + float((int(r.h) - g) / 2), float(g), float(g)},
                 rgb(0xffffff));
  } else {
    // the stroke lies over the white fill: an 11 % black edge on white is #e3e3e3
    e.c.shape(r, radius, rgb(0xe3e3e3));
    e.c.shape({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, radius - 1, rgb(0xffffff));
    e.c.layer(layer + 2);
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

std::string clockTime(time_t t, bool clock24) {
  struct tm tm;
  localtime_r(&t, &tm);
  char b[16];
  if (clock24) snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
  else snprintf(b, sizeof(b), "%d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  return b;
}

std::string clockDate(time_t t, bool weekdayAndYear) {
  static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct tm tm;
  localtime_r(&t, &tm);
  char b[32];
  if (weekdayAndYear) snprintf(b, sizeof(b), "%s, %s %d, %d", kDays[tm.tm_wday], kMonths[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
  else snprintf(b, sizeof(b), "%s %d", kMonths[tm.tm_mon], tm.tm_mday);
  return b;
}

void readClock(Status& s, bool clock24) {
  time_t now = time(nullptr);
  s.date = clockDate(now, false);
  s.time = clockTime(now, clock24);
}

void statusBar(Env& e, float w, const Status& s, const std::string& title, const std::string& title2, float t) {
  const Palette& p = e.p;
  float H = float(e.dp(22));
  int layer = e.c.currentLayer();
  e.c.fillGradient({0, 0, w, H}, p.barTop, p.barBottom);
  e.c.fill({0, H - 1, w, 1}, p.barLine);
  e.c.layer(layer + 1);
  float leftEnd = float(e.dp(10));
  if (s.playing) {
    int sz = e.dp(12);
    e.icons.draw(e.c, Icon::Play, {float(e.dp(10)), float((int(H) - sz) / 2), float(sz), float(sz)}, p.accent);
    leftEnd += sz + e.dp(6);
  }
  // the clock on the left, after the play state: the time, then the date (2.0, Nov 10)
  if (!s.time.empty()) {
    Label tl = label(e, s.time, 11, true), dl = label(e, s.date, 11, false);
    drawLabel(e, tl, leftEnd, float((int(H) - tl.h) / 2), p.icon);
    leftEnd += tl.w;
    if (!s.date.empty()) {
      leftEnd += e.dp(5);
      drawLabel(e, dl, leftEnd, float((int(H) - dl.h) / 2), p.icon);
      leftEnd += dl.w;
    }
  }
  // Wi-Fi (dimmed offline), the percentage and the battery, 10 units from the right
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", int(floorf(s.battery * 100 + 0.5f)));
  Label pl = label(e, pct, 11, true);
  int wifi = e.dp(14), bw = e.dp(24), bh = e.dp(14), bolt = s.charging ? Icons::chargeRoom(float(bw), float(bh)) : 0;
  int gw = wifi + e.dp(6) + pl.w + e.dp(3) + bolt + bw, gh = std::max({wifi, pl.h, bh});
  float gx = w - e.dp(10) - gw, gy = float((int(H) - gh) / 2);
  // The title stays centered and keeps 10 units clear of the icons on the right and of the clock on the
  // left, the same distance on both sides; a longer one (a Wi-Fi network's name) ends in an ellipsis
  // (2.0: 1.0.x let it run into them)
  float side = std::max(w - gx, leftEnd) + e.dp(10), maxW = std::max(0.0f, w - 2 * side);
  auto drawTitle = [&](const std::string& text, float alpha) {
    if (text.empty() || alpha <= 0) return;
    Label l = label(e, text, 15, true, maxW);
    float x = float((int(w) - l.w) / 2), top = float((int(H) - l.h) / 2);
    e.c.pushAlpha(alpha);
    if (p.titleShadow >> 24) drawLabel(e, l, x, top + 1, p.titleShadow);  // setShadowLayer(1, 0, 1, white)
    drawLabel(e, l, x, top, p.text);
    e.c.popAlpha();
  };
  drawTitle(title, 1 - t);
  drawTitle(title2, t);
  e.c.pushAlpha(s.online ? 1 : 0.25f);
  e.icons.draw(e.c, Icon::Wifi, {gx, gy + (gh - wifi) / 2, float(wifi), float(wifi)}, p.icon);
  e.c.popAlpha();
  drawLabel(e, pl, gx + wifi + e.dp(6), gy + (gh - pl.h) / 2, p.icon);
  e.icons.battery(e.c, {gx + wifi + e.dp(6) + pl.w + e.dp(3) + bolt, gy + (gh - bh) / 2, float(bw), float(bh)}, p.icon, s.battery, s.charging);
  e.c.layer(layer);
}


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
  float entry = float(e.dp(style == RowStyle::Shell ? 36 : style == RowStyle::Music ? 42 : 66));
  float header = float(e.dp(style == RowStyle::Shell ? 20 : 28));
  // a new text size changes the rows' heights: laid out again, the selection kept in view
  bool rescaled = entry != entryH_ || header != headerH_;
  entryH_ = entry, headerH_ = header;
  bool resized = w != w_ || h != h_ || rescaled;
  if (rescaled) placed_ = false;
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
  if (onNearEnd && selected_ >= int(rows_.size()) - 5) onNearEnd();
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

void RowList::appendRows(std::vector<Row> more) {
  if (more.empty()) return;
  for (Row& r : more) rows_.push_back(std::move(r));
  top_.clear();  // laid out again with the new rows, the scroll kept
}

bool RowList::up(int steps) {
  if (selected_ == firstSelectable() || selected_ < 0) return onTopReached ? onTopReached() : true;
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
    // a TextView of up to 4 lines, centered, 20 units in from each side
    c.layer(3);
    float tw = w_ - 2 * e.dp(20);
    // up to 4 lines in all; a "\n" in the message starts a new one
    std::vector<Label> lines;
    size_t start = 0;
    while (start <= message_.size() && lines.size() < 4) {
      size_t nl = message_.find('\n', start);
      std::string part = message_.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
      for (Label& l : wrap(e, part.empty() ? " " : part, 15, false, tw, int(4 - lines.size()))) lines.push_back(l);
      if (nl == std::string::npos) break;
      start = nl + 1;
    }
    int th = 0;
    for (auto& l : lines) th += l.h;
    float ty = y + float((int(h_) - th) / 2);
    for (auto& l : lines) {
      drawLabel(e, l, centerText(x + e.dp(20), tw, l.w), ty, style == RowStyle::Shell ? p.muted : p.darkMuted);
      ty += l.h;
    }
    c.popClip();
    return;
  }
  if (style != RowStyle::Shell) {
    drawMusic(e, x, y);
    c.popClip();
    return;
  }
  float scroll = scroll_.x;
  float H = entryH_;
  if (selected_ >= 0 && active) {
    c.layer(1);
    c.fillGradient({x, y + bar_.x - scroll, w_, H}, p.accentTop, p.accent);
  }
  for (int i = 0; i < int(rows_.size()); i++) {
    float top = y + top_[i] - scroll;
    if (top + rowH(i) < y || top > y + h_) continue;
    Row lazy;
    if (fill && !rows_[i].header) {
      lazy = rows_[i];
      fill(i, lazy);
    }
    const Row& r = fill && !rows_[i].header ? lazy : rows_[i];
    if (r.header) {
      c.layer(0);
      c.fill({x, top, w_, headerH_}, p.header);
      c.layer(3);
      Label l = label(e, rondo::upper(r.title), 11, true, w_ - e.dp(12));  // String.uppercase()
      drawLabel(e, l, x + e.dp(12), top + headerH_ - e.dp(3) - l.h, p.muted);
      continue;
    }
    // how much of this row the highlight covers: its text turns white as the highlight passes
    float sel = active && selected_ >= 0 ? clamp01(1 - fabsf(bar_.x - top_[i]) / H) : 0;
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

// RowViews.media() in the MUSIC style: a card inset 5 x 2 units in each 42-unit row, the 34-unit
// artwork, title and subtitle, trailing text, a chevron. The selected row's card is white (`focus`,
// radius 7) with dark text, its title bold; the card glides from row to row.
void RowList::drawMusic(Env& e, float x, float y) {
  const Palette& p = e.p;
  Canvas& c = e.c;
  float scroll = scroll_.x, H = entryH_;
  int mx = e.dp(5), my = e.dp(2);
  if (selected_ >= 0 && active) {
    c.layer(1);
    c.shape({x + mx, y + bar_.x - scroll + my, w_ - 2 * mx, H - 2 * my}, e.dpf(7), p.focus);
  }
  for (int i = 0; i < int(rows_.size()); i++) {
    float top = y + top_[i] - scroll;
    if (top + rowH(i) < y || top > y + h_) continue;
    Row lazy;
    if (fill && !rows_[i].header) {
      lazy = rows_[i];
      fill(i, lazy);
    }
    const Row& r = fill && !rows_[i].header ? lazy : rows_[i];
    if (r.header) {
      c.layer(3);
      Label l = label(e, r.title, 15, true, w_ - e.dp(10));
      drawLabel(e, l, x + e.dp(10), top + headerH_ - e.dp(5) - l.h, p.white);
      continue;
    }
    float sel = active && selected_ >= 0 ? clamp01(1 - fabsf(bar_.x - top_[i]) / H) : 0;
    // the card's box and its padding
    float cx = x + mx, cw = w_ - 2 * mx, ctop = top + my;
    int ch = int(H) - 2 * my;
    float left = cx + e.dp(4), right = cx + cw - e.dp(8);
    bool video = style == RowStyle::Video;
    // MUSIC: 34-unit square artwork, radius 3; VIDEO: a 104 x 58 thumbnail, radius 5, its length in a badge
    int artW = e.dp(video ? 104 : 34), artH = e.dp(video ? 58 : 34);
    float radius = e.dpf(video ? 5 : 3);
    Rect ar{left, ctop + float((ch - artH) / 2), float(artW), float(artH)};
    c.layer(2);
    if (r.tile) {
      drawTile(e, *r.tile, ar, 1);
    } else {
      float a = 1;
      const Texture* tex = r.thumb.empty() ? nullptr : e.covers.get(r.thumb, artW, artH, 0, e.now, &a);
      if (!tex || a < 1) c.shape(ar, radius, p.raised);
      c.layer(3);
      if (tex) c.image(*tex, ar, {0, 0, 1, 1}, radius, withAlpha(0xffffffff, a));
    }
    if (video) {
      if (!r.trailing.empty()) {  // the length badge: 9 bold white on argb(220) black, 3 units from the corner
        Label b = label(e, r.trailing, 9, true);
        int bw = b.w + 2 * e.dp(3), bh = b.h + 2 * e.dp(1);
        Rect br{ar.x + ar.w - e.dp(3) - bw, ar.y + ar.h - e.dp(3) - bh, float(bw), float(bh)};
        c.layer(3);
        c.shape(br, e.dpf(3), 0xdc000000);
        c.layer(4);
        drawLabel(e, b, br.x + e.dp(3), br.y + e.dp(1), p.white);
      }
      float colX = left + artW + e.dp(9), colW = right - colX;
      c.layer(4);
      std::vector<Label> title = wrap(e, r.title, 12, sel > 0.5f, colW, 2);
      Label sl;
      if (!r.subtitle.empty()) sl = label(e, r.subtitle, 10, false, colW);
      int colH = r.subtitle.empty() ? 0 : e.dp(2) + sl.h;
      for (auto& l : title) colH += l.h;
      float ty = ctop + float((ch - colH) / 2);
      for (auto& l : title) drawLabel(e, l, colX, ty, mix(p.white, p.focusText, sel)), ty += l.h;
      if (!r.subtitle.empty()) drawLabel(e, sl, colX, ty + e.dp(2), mix(p.darkMuted, p.focusMuted, sel));
      continue;
    }
    float colX = left + artW + e.dp(8);
    float end = right;
    int cvw = e.dp(7), cvh = e.dp(11);
    c.layer(4);
    if (r.chevron && style == RowStyle::Music) {
      end -= cvw;
      e.icons.draw(c, Icon::Chevron, {end, ctop + float((ch - cvh) / 2), float(cvw), float(cvh)}, mix(p.darkFaint, p.focusMuted, sel));
      end -= e.dp(5);
    }
    if (!r.trailing.empty()) {
      Label tr = label(e, r.trailing, 11, false);
      end -= tr.w;
      drawLabel(e, tr, end, ctop + float((ch - tr.h) / 2), mix(p.darkMuted, p.focusMuted, sel));
      end -= e.dp(6);
    }
    float colW = end - colX;
    Label tl = label(e, r.title, 13, sel > 0.5f, colW);
    Label sl;
    if (!r.subtitle.empty()) sl = label(e, r.subtitle, 11, false, colW);
    int colH = tl.h + (r.subtitle.empty() ? 0 : e.dp(2) + sl.h);
    float ty = ctop + float((ch - colH) / 2);
    drawLabel(e, tl, colX, ty, mix(p.white, p.focusText, sel));
    if (!r.subtitle.empty()) drawLabel(e, sl, colX, ty + tl.h + e.dp(2), mix(p.darkMuted, p.focusMuted, sel));
  }
  c.layer(0);
}

}  // namespace rondo::shell

namespace rondo::shell {

namespace {
// the code point at i, and where the next one starts
uint32_t cpAt(const std::string& s, size_t i, size_t* next) {
  unsigned char c = (unsigned char)s[i];
  int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  uint32_t cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
  size_t k = 1;
  for (; k < size_t(len) && i + k < s.size() && ((unsigned char)s[i + k] & 0xc0) == 0x80; k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3f);
  *next = i + k;
  return cp;
}
// what stays with the character before it: accents, the variation selectors, the keycap, skin tones,
// tags (a subdivision flag)
bool extends(uint32_t cp) {
  return (cp >= 0x300 && cp <= 0x36f) || (cp >= 0xfe00 && cp <= 0xfe0f) || cp == 0x20e3 || (cp >= 0x1f3fb && cp <= 0x1f3ff) ||
         (cp >= 0xe0020 && cp <= 0xe007f);
}
bool regional(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }
// One character as the reader sees it (a grapheme cluster, near enough): a code point with what
// extends it, across a joiner the next one too (a family, a profession), a flag's two letters together.
// A line is never cut, and "…" never put, inside one.
size_t nextChar(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  size_t n;
  uint32_t cp = cpAt(s, i, &n);
  if (regional(cp) && n < s.size()) {
    size_t m;
    if (regional(cpAt(s, n, &m))) n = m;
  }
  while (n < s.size()) {
    size_t m;
    uint32_t c = cpAt(s, n, &m);
    if (c == 0x200d) {  // the joiner, and what it joins
      n = m;
      if (n < s.size()) cpAt(s, n, &n);
      continue;
    }
    if (!extends(c)) break;
    n = m;
  }
  return n;
}
// where each character from `from` ends, up to `to`
std::vector<size_t> charEnds(const std::string& s, size_t from, size_t to) {
  std::vector<size_t> b;
  for (size_t i = from; i < to;) b.push_back(i = std::min(to, nextChar(s, i)));
  return b;
}
// the last of `ends` (in order) where fits() holds, by halves: the width grows with the text, and each
// look shapes the text once (one at a time, a long title cost a shaping per character); -1: none
template <typename F>
int lastFitting(const std::vector<size_t>& ends, F fits) {
  int lo = 0, hi = int(ends.size()) - 1, found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (fits(ends[size_t(mid)])) found = mid, lo = mid + 1;
    else hi = mid - 1;
  }
  return found;
}
}  // namespace

// Android 4.2's StaticLayout for plain text: a line ends at the last space that fits (its spaces hang
// past the edge), or after the characters that fit when one word is wider than the line. The last
// line allowed, with more text after it, gets "…" in place of its own last character, or of as many of
// its characters as no longer fit beside it (calculateEllipsis with forceEllipsis): "| Viva…", not
// "| Viva İ…".
std::vector<WrappedLine> wrapText(Env& e, const std::string& s, float sp, bool bold, float maxW, int maxLines) {
  std::vector<WrappedLine> lines;
  int font = e.font(sp, bold);
  auto width = [&](size_t from, size_t to) { return e.text.run(font, std::string_view(s).substr(from, to - from)).width; };
  size_t at = 0;
  while (at < s.size() && int(lines.size()) < maxLines) {
    while (at < s.size() && s[at] == ' ') at++;
    if (at >= s.size()) break;
    if (width(at, s.size()) <= maxW) {  // the rest fits: most lines do
      lines.push_back({s.substr(at), false});
      break;
    }
    // as many words as fit (a line ends at a space)
    std::vector<size_t> cuts;
    for (size_t next = s.find(' ', at + 1); next != std::string::npos; next = s.find(' ', next + 1)) cuts.push_back(next);
    cuts.push_back(s.size());
    int w = lastFitting(cuts, [&](size_t cut) { return width(at, cut) <= maxW; });
    size_t best = w >= 0 ? cuts[size_t(w)] : std::string::npos;
    if (best == std::string::npos) {  // one word wider than the line: as many of its characters as fit
      std::vector<size_t> ends = charEnds(s, at, s.size());
      int k = lastFitting(ends, [&](size_t n) { return width(at, n) <= maxW; });
      best = ends[size_t(std::max(0, k))];  // at least one, as StaticLayout
    }
    if (int(lines.size()) < maxLines - 1) {
      lines.push_back({s.substr(at, best - at), false});
      at = best;
      continue;
    }
    size_t lineEnd = best;
    while (lineEnd < s.size() && s[lineEnd] == ' ') lineEnd++;  // with the spaces that hang at its end
    float ell = e.text.run(font, "\xe2\x80\xa6").width;
    std::vector<size_t> ends = charEnds(s, at, lineEnd);
    int k = lastFitting(ends, [&](size_t n) { return width(at, n) + ell <= maxW; });
    size_t keep = k >= 0 ? ends[size_t(k)] : at;
    // all of it fits: its last character (a whole emoji sequence) gives way
    if (keep == lineEnd) keep = ends.size() > 1 ? ends[ends.size() - 2] : at;
    lines.push_back({s.substr(at, keep - at) + "\xe2\x80\xa6", false});
    break;
  }
  return lines;
}

std::vector<Label> wrap(Env& e, const std::string& s, float sp, bool bold, float maxW, int maxLines) {
  std::vector<Label> out;
  for (const WrappedLine& l : wrapText(e, s, sp, bold, maxW, maxLines)) out.push_back(label(e, l.text, sp, bold, l.ellipsize ? maxW : 0));
  return out;
}

}  // namespace rondo::shell
