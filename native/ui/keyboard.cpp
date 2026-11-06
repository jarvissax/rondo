#include "keyboard.h"

#include <math.h>
#include <stdlib.h>

namespace rondo::ui {
namespace {
const char* kLower = "\001lower";
const char* kUpper = "\001upper";
const char* kSymbols = "\001symbols";
const char* kDelete = "\001delete";
const char* kAction = "\001action";
constexpr float kOmega = 43;  // the selection glide
}  // namespace

Keyboard::Keyboard(std::string actionLabel, bool shell) : action_(std::move(actionLabel)), shell_(shell) {
  build(Layout::Lower);
}

void Keyboard::build(Layout l) {
  layout_ = l;
  keys_.clear();
  auto row = [&](int r, const char* chars) {
    for (int i = 0; chars[i]; i++) keys_.push_back({std::string(1, chars[i]), std::string(1, chars[i]), r, i, 1});
  };
  if (l == Layout::Lower) row(0, "abcdefghij"), row(1, "klmnopqrst"), row(2, "uvwxyz.-_@");
  if (l == Layout::Upper) row(0, "ABCDEFGHIJ"), row(1, "KLMNOPQRST"), row(2, "UVWXYZ'&!?");
  if (l == Layout::Symbols) row(0, "!@#$%^&*()"), row(1, "-_=+[]{};:"), row(2, "'\",./?<>\\|");
  row(3, "1234567890");
  if (l == Layout::Symbols) {
    keys_.push_back({"abc", kLower, 4, 0, 2});
    keys_.push_back({"~ `", "~", 4, 2, 1});
  } else {
    keys_.push_back({l == Layout::Lower ? "ABC" : "abc", l == Layout::Lower ? kUpper : kLower, 4, 0, 2});
    keys_.push_back({"#+=", kSymbols, 4, 2, 1});
  }
  keys_.push_back({"Space", " ", 4, 3, 3});
  keys_.push_back({"Del", kDelete, 4, 6, 2});
  keys_.push_back({action_, kAction, 4, 8, 2});
}

void Keyboard::switchTo(Layout l) {
  K at = keys_[cursor_];
  build(l);
  cursor_ = 0;  // stay on the same spot of the grid
  for (size_t i = 0; i < keys_.size(); i++)
    if (keys_[i].row == at.row && at.col >= keys_[i].col && at.col < keys_[i].col + keys_[i].span) cursor_ = int(i);
}

void Keyboard::horizontal(int dir) {
  std::vector<int> row;
  for (size_t i = 0; i < keys_.size(); i++)
    if (keys_[i].row == keys_[cursor_].row) row.push_back(int(i));
  int at = 0;
  for (size_t i = 0; i < row.size(); i++)
    if (row[i] == cursor_) at = int(i);
  cursor_ = row[(at + dir + int(row.size())) % int(row.size())];
}

void Keyboard::vertical(int dir) {
  const K& k = keys_[cursor_];
  int row = k.row + dir;
  if (row < 0 || row > 4) return;
  float center = k.col + k.span / 2.0f, best = 1e9f;
  for (size_t i = 0; i < keys_.size(); i++) {
    if (keys_[i].row != row) continue;
    float d = fabsf(keys_[i].col + keys_[i].span / 2.0f - center);
    if (d < best) best = d, cursor_ = int(i);
  }
}

void Keyboard::nextKey() { cursor_ = (cursor_ + 1) % int(keys_.size()); }
void Keyboard::prevKey() { cursor_ = (cursor_ - 1 + int(keys_.size())) % int(keys_.size()); }

bool Keyboard::up() {
  if (keys_[cursor_].row == 0) return false;
  vertical(-1);
  return true;
}

Keyboard::Result Keyboard::press() {
  const std::string& v = keys_[cursor_].value;
  if (v == kLower) switchTo(Layout::Lower);
  else if (v == kUpper) switchTo(Layout::Upper);
  else if (v == kSymbols) switchTo(Layout::Symbols);
  else if (v == kDelete) {
    // one UTF-8 character
    while (!typed_.empty() && (uint8_t(typed_.back()) & 0xc0) == 0x80) typed_.pop_back();
    if (!typed_.empty()) typed_.pop_back();
    return Result::Delete;
  } else if (v == kAction) {
    return Result::Action;
  } else {
    typed_ += v;
    return Result::Typed;
  }
  return Result::None;
}

Keyboard::Result Keyboard::handle(Key key) {
  switch (key) {
    case Key::WheelCw: nextKey(); break;
    case Key::WheelCcw: prevKey(); break;
    case Key::Next: right(); break;
    case Key::Prev: left(); break;
    case Key::Play: down(); break;
    case Key::Menu: return up() ? Result::None : Result::Close;
    case Key::Center: return press();
    default: break;
  }
  return Result::None;
}

bool Keyboard::step(float dt) {
  const K& k = keys_[cursor_];
  if (!placed_) {
    hx_.snap(float(k.col)), hy_.snap(float(k.row)), hw_.snap(float(k.span));
    placed_ = true;
  }
  bool a = hx_.step(float(k.col), dt, kOmega), b = hy_.step(float(k.row), dt, kOmega), c = hw_.step(float(k.span), dt, kOmega);
  return a || b || c;
}

// TvKeyboard.onDraw(): ten columns and five rows over the view, each key inset by 2 units on every side
// with 4-unit corners. The highlighted key is the shell's text color with its label in the background
// color and bold; here it glides between keys, labels changing color as it passes.
void Keyboard::draw(Canvas& c, Text& text, const Palette& p, Rect area, int bigPx, int smallPx) {
  float cw = area.w / 10, rh = area.h / 5;
  float gap = float(int(floorf(2 * p.scale + 0.5f))), radius = float(int(floorf(4 * p.scale + 0.5f)));
  auto cell = [&](float col, float row, float span) {
    return Rect{area.x + col * cw + gap, area.y + row * rh + gap, span * cw - 2 * gap, rh - 2 * gap};
  };
  int layer = c.currentLayer();
  const K& at = keys_[cursor_];
  bool settled = fabsf(hx_.x - at.col) < 0.02f && fabsf(hy_.x - at.row) < 0.02f && fabsf(hw_.x - at.span) < 0.02f;
  for (const K& k : keys_) {
    // the highlighted key is drawn once, in the highlight's color (no fringe of its own color around it)
    if (settled && &k == &at) continue;
    bool special = k.value[0] == '\001';
    Color fill = k.value == kAction ? (shell_ ? p.accent : p.red)
                 : special          ? (shell_ ? mix(p.header, p.text, 0.12f) : p.darkPanelSelected)
                                    : (shell_ ? p.header : p.raised);
    c.layer(layer);
    c.shape(cell(float(k.col), float(k.row), float(k.span)), radius, fill);
  }
  c.layer(layer + 1);
  c.shape(cell(hx_.x, hy_.x, hw_.x), radius, shell_ ? p.text : p.white);
  for (size_t i = 0; i < keys_.size(); i++) {
    const K& k = keys_[i];
    Rect r = cell(float(k.col), float(k.row), float(k.span));
    // how much of this key the highlight covers
    float on = fmaxf(0, 1 - fabsf(hx_.x - k.col) - fabsf(hy_.x - k.row) - fabsf(hw_.x - k.span));
    if (int(i) != cursor_) on = fminf(on, 0.999f);
    Color base = shell_ && k.value != kAction ? p.text : p.white;
    Color onColor = shell_ ? p.shellBg : p.dark;
    float px = float(k.label.size() > 1 ? smallPx : bigPx);
    int font = text.font(on > 0.5f ? Weight::Bold : Weight::Regular, px);
    const TextRun& run = text.run(font, k.label);
    // Paint.Align.CENTER at the key's center; the baseline where ascent and descent balance
    float x = roundf(r.x + r.w / 2 - run.width / 2);
    float baseline = roundf(r.y + r.h / 2 + (text.ascent(font) - text.descent(font)) / 2);
    c.layer(layer + 2);
    text.draw(c, run, x, baseline, mix(base, onColor, on));
  }
  c.layer(layer);
}

}  // namespace rondo::ui
