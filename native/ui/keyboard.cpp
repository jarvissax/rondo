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
const char* kEmoji = "\001emoji";
const char* kPagePrev = "\001pageprev";
const char* kPageNext = "\001pagenext";

// the emoji pages: faces, more faces, hands and hearts, things (40 each)
const std::vector<std::string> kEmojiList = {
    "\xf0\x9f\x98\x80", "\xf0\x9f\x98\x83", "\xf0\x9f\x98\x84", "\xf0\x9f\x98\x81", "\xf0\x9f\x98\x86", "\xf0\x9f\x98\x85", "\xf0\x9f\x98\x82", "\xf0\x9f\xa4\xa3", "\xf0\x9f\x98\x8a", "\xf0\x9f\x98\x87", "\xf0\x9f\x99\x82", "\xf0\x9f\x99\x83", "\xf0\x9f\x98\x89", "\xf0\x9f\x98\x8c", "\xf0\x9f\x98\x8d", "\xf0\x9f\xa5\xb0", "\xf0\x9f\x98\x98", "\xf0\x9f\x98\x97", "\xf0\x9f\x98\x9a", "\xf0\x9f\x98\x8b", "\xf0\x9f\x98\x9b", "\xf0\x9f\x98\x9c", "\xf0\x9f\xa4\xaa", "\xf0\x9f\x98\x9d", "\xf0\x9f\xa4\x97", "\xf0\x9f\xa4\xad", "\xf0\x9f\xa4\x94", "\xf0\x9f\xa4\xa8", "\xf0\x9f\x98\x90", "\xf0\x9f\x98\x91", "\xf0\x9f\x98\xb6", "\xf0\x9f\x99\x84", "\xf0\x9f\x98\x8f", "\xf0\x9f\x98\xac", "\xf0\x9f\x98\xb4", "\xf0\x9f\xa4\xa4", "\xf0\x9f\x98\xaa", "\xf0\x9f\x98\xb7", "\xf0\x9f\xa4\x92", "\xf0\x9f\xa4\x95",
    "\xf0\x9f\xa4\xa2", "\xf0\x9f\xa4\xae", "\xf0\x9f\xa4\xa7", "\xf0\x9f\xa5\xb5", "\xf0\x9f\xa5\xb6", "\xf0\x9f\x98\xb5", "\xf0\x9f\xa4\xaf", "\xf0\x9f\xa5\xb3", "\xf0\x9f\x98\x8e", "\xf0\x9f\xa4\x93", "\xf0\x9f\xa7\x90", "\xf0\x9f\x98\x95", "\xf0\x9f\x98\x9f", "\xf0\x9f\x99\x81", "\xf0\x9f\x98\xae", "\xf0\x9f\x98\xaf", "\xf0\x9f\x98\xb2", "\xf0\x9f\x98\xb3", "\xf0\x9f\xa5\xba", "\xf0\x9f\x98\xa6", "\xf0\x9f\x98\xa7", "\xf0\x9f\x98\xa8", "\xf0\x9f\x98\xb0", "\xf0\x9f\x98\xa5", "\xf0\x9f\x98\xa2", "\xf0\x9f\x98\xad", "\xf0\x9f\x98\xb1", "\xf0\x9f\x98\x96", "\xf0\x9f\x98\xa3", "\xf0\x9f\x98\x9e", "\xf0\x9f\x98\x93", "\xf0\x9f\x98\xa9", "\xf0\x9f\x98\xab", "\xf0\x9f\xa5\xb1", "\xf0\x9f\x98\xa4", "\xf0\x9f\x98\xa1", "\xf0\x9f\x98\xa0", "\xf0\x9f\xa4\xac", "\xf0\x9f\x98\x88", "\xf0\x9f\x92\x80",
    "\xf0\x9f\x91\x8d", "\xf0\x9f\x91\x8e", "\xf0\x9f\x91\x8c", "\xe2\x9c\x8c\xef\xb8\x8f", "\xf0\x9f\xa4\x9e", "\xf0\x9f\xa4\x9f", "\xf0\x9f\xa4\x98", "\xf0\x9f\xa4\x99", "\xf0\x9f\x91\x88", "\xf0\x9f\x91\x89", "\xf0\x9f\x91\x86", "\xf0\x9f\x91\x87", "\xe2\x98\x9d\xef\xb8\x8f", "\xe2\x9c\x8b", "\xf0\x9f\x91\x8b", "\xf0\x9f\x91\x8f", "\xf0\x9f\x99\x8c", "\xf0\x9f\x91\x90", "\xf0\x9f\xa4\xb2", "\xf0\x9f\x99\x8f", "\xf0\x9f\xa4\x9d", "\xf0\x9f\x92\xaa", "\xe2\x9c\x8d\xef\xb8\x8f", "\xf0\x9f\x91\x80", "\xf0\x9f\x99\x88", "\xf0\x9f\x99\x89", "\xf0\x9f\x99\x8a", "\xf0\x9f\x92\x8b", "\xe2\x9d\xa4\xef\xb8\x8f", "\xf0\x9f\xa7\xa1", "\xf0\x9f\x92\x9b", "\xf0\x9f\x92\x9a", "\xf0\x9f\x92\x99", "\xf0\x9f\x92\x9c", "\xf0\x9f\x96\xa4", "\xf0\x9f\xa4\x8d", "\xf0\x9f\xa4\x8e", "\xf0\x9f\x92\x94", "\xf0\x9f\x92\x95", "\xf0\x9f\x92\xaf",
    "\xf0\x9f\x94\xa5", "\xe2\x9c\xa8", "\xe2\xad\x90", "\xf0\x9f\x8c\x9f", "\xe2\x9a\xa1", "\xf0\x9f\x8c\x88", "\xe2\x98\x80\xef\xb8\x8f", "\xf0\x9f\x8c\x99", "\xe2\x9d\x84\xef\xb8\x8f", "\xf0\x9f\x8c\xa7\xef\xb8\x8f", "\xf0\x9f\x8c\xb9", "\xf0\x9f\x8c\xb8", "\xf0\x9f\x8c\xbb", "\xf0\x9f\x8d\x80", "\xf0\x9f\x8e\x89", "\xf0\x9f\x8e\x8a", "\xf0\x9f\x8e\x81", "\xf0\x9f\x8e\x82", "\xf0\x9f\x8e\x88", "\xf0\x9f\x8e\xb6", "\xe2\x98\x95", "\xf0\x9f\x8d\x95", "\xf0\x9f\x8d\x94", "\xf0\x9f\x8d\x9f", "\xf0\x9f\x8d\xb0", "\xf0\x9f\x8d\xab", "\xf0\x9f\x8d\xba", "\xf0\x9f\x8d\xb7", "\xe2\x9a\xbd", "\xf0\x9f\x8f\x86", "\xf0\x9f\x9a\x97", "\xe2\x9c\x88\xef\xb8\x8f", "\xf0\x9f\x8f\xa0", "\xf0\x9f\x93\xb7", "\xf0\x9f\x93\xb1", "\xf0\x9f\x92\xbb", "\xe2\x8f\xb0", "\xf0\x9f\x92\xa1", "\xe2\x9c\x85", "\xe2\x9d\x8c",
};

// a UTF-8 text's last code point, taken off it (0 when empty)
uint32_t popCodePoint(std::string& s) {
  if (s.empty()) return 0;
  size_t at = s.size() - 1;
  while (at > 0 && (uint8_t(s[at]) & 0xc0) == 0x80) at--;
  uint32_t cp = uint8_t(s[at]);
  int n = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
  cp &= n == 3 ? 0x07 : n == 2 ? 0x0f : n == 1 ? 0x1f : 0x7f;
  for (int i = 1; i <= n && at + size_t(i) < s.size(); i++) cp = cp << 6 | (uint8_t(s[at + size_t(i)]) & 0x3f);
  s.resize(at);
  return cp;
}
uint32_t lastCodePoint(const std::string& s) {
  std::string copy = s;
  return popCodePoint(copy);
}
// what clings to an emoji: the variation selector, the joiner, a skin tone, the keycap
bool clings(uint32_t cp) { return cp == 0xfe0f || cp == 0x200d || (cp >= 0x1f3fb && cp <= 0x1f3ff) || cp == 0x20e3; }
constexpr float kOmega = 43;  // the selection glide
}  // namespace

Keyboard::Keyboard(std::string actionLabel, bool shell, bool emoji) : action_(std::move(actionLabel)), shell_(shell), emoji_(emoji) {
  build(Layout::Lower);
}

const std::vector<std::string>& Keyboard::emojiList() { return kEmojiList; }

void Keyboard::deleteLast(std::string& s) {
  uint32_t cp = popCodePoint(s);
  // a variation, a skin tone or a joiner went: the emoji they belonged to goes with them
  while (clings(cp) && !s.empty()) cp = popCodePoint(s);
  // a joined emoji (a family, a heart on fire): the parts before each joiner too
  while (!s.empty() && lastCodePoint(s) == 0x200d) {
    popCodePoint(s);
    do cp = popCodePoint(s);
    while (clings(cp) && !s.empty());
  }
}

void Keyboard::build(Layout l) {
  layout_ = l;
  keys_.clear();
  auto row = [&](int r, const char* chars) {
    for (int i = 0; chars[i]; i++) keys_.push_back({std::string(1, chars[i]), std::string(1, chars[i]), r, i, 1});
  };
  if (l == Layout::Emoji) {  // four rows of a page, and the page's turning under them
    int pages = int(kEmojiList.size()) / kEmojiPerPage;
    emojiPage_ = (emojiPage_ % pages + pages) % pages;
    for (int i = 0; i < kEmojiPerPage; i++) {
      const std::string& e = kEmojiList[size_t(emojiPage_ * kEmojiPerPage + i)];
      K k{e, e, i / 10, i % 10, 1};
      k.emoji = true;
      keys_.push_back(k);
    }
    keys_.push_back({"abc", kLower, 4, 0, 2});
    keys_.push_back({"\xe2\x80\xb9", kPagePrev, 4, 2, 1});
    keys_.push_back({"\xe2\x80\xba", kPageNext, 4, 3, 1});
    keys_.push_back({"Space", " ", 4, 4, 2});
    keys_.push_back({"Del", kDelete, 4, 6, 2});
    keys_.push_back({action_, kAction, 4, 8, 2});
    return;
  }
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
  if (emoji_) {  // the emoji pages' key takes a column of Space
    K smile{"\xf0\x9f\x98\x8a", kEmoji, 4, 3, 1};
    smile.emoji = true;
    keys_.push_back(smile);
    keys_.push_back({"Space", " ", 4, 4, 2});
  } else {
    keys_.push_back({"Space", " ", 4, 3, 3});
  }
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
  else if (v == kEmoji) switchTo(Layout::Emoji);
  else if (v == kPagePrev || v == kPageNext) {
    emojiPage_ += v == kPageNext ? 1 : -1;
    switchTo(Layout::Emoji);
  } else if (v == kDelete) {
    deleteLast(typed_);  // one character, or a whole emoji
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
    float px = float(k.label.size() > 1 && !k.emoji ? smallPx : bigPx);
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
