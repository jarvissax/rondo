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

Keyboard::Result Keyboard::handle(Key key) {
  int n = int(keys_.size());
  switch (key) {
    case Key::WheelCw: cursor_ = (cursor_ + 1) % n; break;
    case Key::WheelCcw: cursor_ = (cursor_ - 1 + n) % n; break;
    case Key::Next: horizontal(1); break;
    case Key::Prev: horizontal(-1); break;
    case Key::Play: vertical(1); break;
    case Key::Menu:
      if (keys_[cursor_].row == 0) return Result::Close;
      vertical(-1);
      break;
    case Key::Center: {
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
      break;
    }
    default: return Result::None;
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

void Keyboard::draw(Canvas& c, Text& text, const Palette& p, Rect area, int labelFont, int smallFont) {
  float cw = area.w / 10, rh = area.h / 5, gap = roundf(p.u(2)), radius = p.u(4);
  auto cell = [&](float col, float row, float span) {
    return Rect{area.x + col * cw + gap / 2, area.y + row * rh + gap / 2, span * cw - gap, rh - gap};
  };
  int layer = c.currentLayer();
  for (const K& k : keys_) {
    bool special = k.value[0] == '\001' || k.value == " ";
    Color fill = k.value == kAction ? (shell_ ? p.accent : p.red)
                 : special          ? (shell_ ? mix(p.header, p.text, 0.12f) : p.darkPanelSelected)
                                    : (shell_ ? p.header : p.raised);
    c.layer(layer);
    c.shape(cell(float(k.col), float(k.row), float(k.span)), radius, fill);
  }
  // the highlight glides between keys; labels under it switch color with it
  Rect h = cell(hx_.x, hy_.x, hw_.x);
  c.layer(layer + 1);
  c.shape(h, radius, shell_ ? p.text : p.white);
  for (size_t i = 0; i < keys_.size(); i++) {
    const K& k = keys_[i];
    Rect r = cell(float(k.col), float(k.row), float(k.span));
    float overlap = fmaxf(0, 1 - fabsf(hx_.x - k.col) - fabsf(hy_.x - k.row));
    Color ink = k.value == kAction || !shell_ ? p.white : p.text;
    Color on = shell_ ? p.shellBg : p.dark;
    int font = k.label.size() > 1 ? smallFont : labelFont;
    const TextRun& run = text.run(font, k.label, r.w);
    c.layer(layer + 2);
    text.draw(c, run, roundf(r.x + (r.w - run.width) / 2), roundf(r.y + (r.h + text.ascent(font) * 0.7f) / 2),
              mix(ink, on, int(i) == cursor_ ? fminf(1, overlap * 1.5f) : 0));
  }
  c.layer(layer);
}

}  // namespace rondo::ui
