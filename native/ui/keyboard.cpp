#include "keyboard.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#include <algorithm>

#include "emoji.h"
#include "icons.h"

namespace rondo::ui {
namespace {
const char* kLower = "\001lower";
const char* kUpper = "\001upper";
const char* kSymbols = "\001symbols";
const char* kDelete = "\001delete";
const char* kSmile = "\xf0\x9f\x98\x8a";  // 😊

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
bool regional(uint32_t cp) { return cp >= 0x1f1e6 && cp <= 0x1f1ff; }
// what clings to an emoji: the variation selector, the joiner, a skin tone, the keycap, a flag's tags
bool clings(uint32_t cp) {
  return cp == 0xfe0f || cp == 0x200d || (cp >= 0x1f3fb && cp <= 0x1f3ff) || cp == 0x20e3 || (cp >= 0xe0020 && cp <= 0xe007f);
}
constexpr float kOmega = 43;  // the selection glide
int gridPx(int bigPx) { return int(roundf(bigPx * 1.2f)); }  // the emoji in the list: a fifth bigger than the keys' labels

// The three looks: key fills, the highlight, the action key, the shapes
struct Look {
  Color key, special, label, muted, hl, hlText, action, actionText, mark;
  float radius;  // in units; < 0: round (half the key's height)
};
Look lookFor(Keyboard::Style s, const Palette& p) {
  switch (s) {
    case Keyboard::Style::YouTube:  // its chips: #272727, the white pill selected, YouTube red
      return {rgb(0x272727), rgb(0x3f3f3f), p.white, p.darkMuted, p.focus, p.focusText, p.red, p.white, p.focus, -1};
    case Keyboard::Style::WhatsApp:  // its panels and greens; the action's word or plane in green
      return {rgb(0x202c33), rgb(0x2a3942), rgb(0xe9edef), rgb(0x8696a0), rgb(0x00a884), rgb(0x0b141a), rgb(0x2a3942), rgb(0x00a884),
              rgb(0x00a884), 6};
    default:  // the shell: 1.0.x's TvKeyboard in the shell's colors
      return {p.header, mix(p.header, p.text, 0.12f), p.text, p.muted, p.text, p.shellBg, p.accent, p.white, p.accent, 4};
  }
}
}  // namespace

Keyboard::Keyboard(std::string actionLabel, Style style, bool emoji) : action_(std::move(actionLabel)), style_(style), emoji_(emoji) {
  build(Layout::Lower);
}

Keyboard Keyboard::picker(Style style, std::string extra) {
  Keyboard k("", style, true);
  k.pickOnly_ = true;
  k.extra_ = std::move(extra);
  k.switchTo(Layout::Emoji);
  return k;
}

void Keyboard::deleteLast(std::string& s) {
  uint32_t cp = popCodePoint(s);
  // a variation, a skin tone, a tag or a joiner went: the emoji they belonged to goes with them
  while (clings(cp) && !s.empty()) cp = popCodePoint(s);
  // a flag is two letters: both go
  if (regional(cp) && regional(lastCodePoint(s))) {
    // only when it was the second of its pair (an odd run of letters before it pairs up otherwise)
    std::string copy = s;
    int run = 0;
    while (!copy.empty() && regional(lastCodePoint(copy))) popCodePoint(copy), run++;
    if (run % 2 == 1) popCodePoint(s);
  }
  // a joined emoji (a family, a heart on fire): the parts before each joiner too
  while (!s.empty() && lastCodePoint(s) == 0x200d) {
    popCodePoint(s);
    do cp = popCodePoint(s);
    while (clings(cp) && !s.empty());
  }
}

// ---------------------------------------------------------------------------------------------------
// The layouts

void Keyboard::build(Layout l) {
  layout_ = l;
  keys_.clear();
  auto row = [&](int r, const char* chars, int from = 0) {
    for (int i = 0; chars[i]; i++) keys_.push_back({std::string(1, chars[i]), std::string(1, chars[i]), r, from + i, 1});
  };
  auto add = [&](std::string label, std::string value, int r, int col, int span, Kind kind) {
    keys_.push_back({std::move(label), std::move(value), r, col, span, kind});
  };
  if (l == Layout::Emoji) {  // the tabs, the list (items_) under them, the keys under it
    for (int g = 0; g < emoji::kGroups; g++) {
      add(emoji::groupIcon(g), "", 0, g, 1, Kind::Tab);
      keys_.back().group = g;
    }
    std::string tone = std::string(emoji::toneHand(emoji::tone())) + " Tone";
    if (pickOnly_) {
      int t = extra_.empty() ? 5 : 3;
      add(tone, "", 4, 0, t, Kind::Tone);
      add("Find", "", 4, t, t, Kind::FindKey);
      if (!extra_.empty()) add(extra_, "", 4, 2 * t, 10 - 2 * t, Kind::Extra);
    } else {
      add(tone, "", 4, 0, 2, Kind::Tone);
      add("Find", "", 4, 2, 1, Kind::FindKey);
      add("abc", kLower, 4, 3, 1, Kind::Special);
      add("Space", " ", 4, 4, 2, Kind::Special);
      add("Del", kDelete, 4, 6, 2, Kind::Special);
      add(action_, "", 4, 8, 2, Kind::Action);
    }
    return;
  }
  if (l == Layout::Find) {  // what matches on top (items_), the letters, the query
    row(1, "abcdefghij"), row(2, "klmnopqrst"), row(3, "uvwxyz");
    add("Space", " ", 3, 6, 4, Kind::Special);
    add(kSmile, "", 4, 0, 2, Kind::EmojiKey);
    add("", "", 4, 2, 6, Kind::Field);
    add("Del", kDelete, 4, 8, 2, Kind::Special);
    return;
  }
  if (l == Layout::Lower) row(0, "abcdefghij"), row(1, "klmnopqrst"), row(2, "uvwxyz.-_@");
  if (l == Layout::Upper) row(0, "ABCDEFGHIJ"), row(1, "KLMNOPQRST"), row(2, "UVWXYZ'&!?");
  if (l == Layout::Symbols) row(0, "!@#$%^&*()"), row(1, "-_=+[]{};:"), row(2, "'\",./?<>\\|");
  row(3, "1234567890");
  if (l == Layout::Symbols) {
    add("abc", kLower, 4, 0, 2, Kind::Special);
    add("~ `", "~", 4, 2, 1, Kind::Char);
  } else {
    add(l == Layout::Lower ? "ABC" : "abc", l == Layout::Lower ? kUpper : kLower, 4, 0, 2, Kind::Special);
    add("#+=", kSymbols, 4, 2, 1, Kind::Special);
  }
  if (emoji_) {  // the emoji's key takes a column of Space
    add(kSmile, "", 4, 3, 1, Kind::EmojiKey);
    add("Space", " ", 4, 4, 2, Kind::Special);
  } else {
    add("Space", " ", 4, 3, 3, Kind::Special);
  }
  add("Del", kDelete, 4, 6, 2, Kind::Special);
  add(action_, "", 4, 8, 2, Kind::Action);
}

void Keyboard::switchTo(Layout l) {
  K at = keys_.empty() || inList_ ? K{"", "", 4, 3, 1} : keys_[cursor_];
  Layout from = layout_;
  inList_ = false;
  build(l);
  cursor_ = 0;
  if (l == Layout::Emoji) {
    if (from != Layout::Find) recent_ = emoji::recent();
    buildList();
    if (from == Layout::Find) {  // back on Find's key
      for (size_t i = 0; i < keys_.size(); i++)
        if (keys_[i].kind == Kind::FindKey) cursor_ = int(i);
      return;
    }
    // on the first emoji: the most recent one, else the first smiley
    scrollTo_ = 0;
    scroll_.snap(0);
    if (!items_.empty()) toItem(0);
    return;
  }
  if (l == Layout::Find) {
    query_.clear();
    buildFound();
    for (size_t i = 0; i < keys_.size(); i++)
      if (keys_[i].row == 1 && keys_[i].col == 0) cursor_ = int(i);
    return;
  }
  // stay on the same spot of the grid (from the emoji: on 😊)
  if (from == Layout::Emoji || from == Layout::Find) at = {"", "", 4, 3, 1};
  for (size_t i = 0; i < keys_.size(); i++)
    if (keys_[i].row == at.row && at.col >= keys_[i].col && at.col < keys_[i].col + keys_[i].span) cursor_ = int(i);
}

// The emoji list: the recent ones, then each group from a new row, in the tone chosen
void Keyboard::buildList() {
  items_.clear();
  queueGroups_ = true;
  queuedTop_ = -100;
  int row = 0, col = 0, group = -1;
  auto add = [&](std::string text, int index, int g) {
    if (g != group && col > 0) row++, col = 0;
    if (col == 10) row++, col = 0;
    group = g;
    items_.push_back({std::move(text), index, g, row, col++});
  };
  for (const std::string& e : recent_)
    if (!text_ || text_->emojiGlyph(e)) add(e, emoji::indexOf(e), 0);
  int tone = emoji::tone();
  if (text_) {
    for (int i : emoji::drawn(*text_)) add(emoji::form(*text_, i, tone), i, emoji::kList[i].group);
  } else {
    for (int i = 0; i < emoji::kCount; i++) add(emoji::kList[i].text, i, emoji::kList[i].group);
  }
}

// Find's row: what matches the query, in the tone chosen
void Keyboard::buildFound() {
  items_.clear();
  scrollTo_ = 0;
  scroll_.snap(0);
  if (!text_) return;
  int col = 0, tone = emoji::tone();
  for (int i : emoji::find(*text_, query_)) items_.push_back({emoji::form(*text_, i, tone), i, emoji::kList[i].group, 0, col++});
}

int Keyboard::firstOfGroup(int group) const {
  for (size_t i = 0; i < items_.size(); i++)
    if (items_[i].group == group) return int(i);
  return -1;
}

int Keyboard::itemInRow(int row, int col) const {
  int best = -1;
  for (size_t i = 0; i < items_.size(); i++) {
    if (items_[i].row != row) continue;
    best = int(i);
    if (items_[i].col >= col) break;
  }
  return best;
}

void Keyboard::toItem(int i) {
  if (i < 0 || i >= int(items_.size())) return;
  inList_ = true;
  item_ = i;
  const Item& it = items_[size_t(i)];
  if (layout_ == Layout::Find) {
    if (it.col < scrollTo_) scrollTo_ = it.col;
    if (it.col > scrollTo_ + 9) scrollTo_ = it.col - 9;
    return;
  }
  if (it.row < scrollTo_) scrollTo_ = it.row;
  if (it.row > scrollTo_ + 2) scrollTo_ = it.row - 2;
  scrollTo_ = std::max(0, std::min(scrollTo_, rowsOf() - 3));
  tab_ = it.group;
}

void Keyboard::scrollToGroup(int group) {
  tab_ = group;
  int i = firstOfGroup(group);
  scrollTo_ = i < 0 ? 0 : std::max(0, std::min(items_[size_t(i)].row, rowsOf() - 3));
}

void Keyboard::toTab(int group) {
  for (size_t i = 0; i < keys_.size(); i++)
    if (keys_[i].kind == Kind::Tab && keys_[i].group == group) cursor_ = int(i);
  inList_ = false;
  scrollToGroup(group);
}

void Keyboard::toKeyNear(int row, float center) {
  float best = 1e9f;
  for (size_t i = 0; i < keys_.size(); i++) {
    if (keys_[i].row != row || keys_[i].kind == Kind::Field) continue;
    float d = fabsf(keys_[i].col + keys_[i].span / 2.0f - center);
    if (d < best) best = d, cursor_ = int(i);
  }
  if (best < 1e9f) inList_ = false;
}

// ---------------------------------------------------------------------------------------------------
// Moving

void Keyboard::wheel(int steps) {
  int dir = steps < 0 ? -1 : 1;
  if (inList_) {
    int n = int(items_.size());
    if (layout_ == Layout::Emoji && abs(steps) >= 2) {  // a fast spin: a row a pulse
      const Item& it = items_[size_t(item_)];
      int r = it.row + dir;
      if (r >= 0 && r < rowsOf()) toItem(itemInRow(r, it.col));
      return;
    }
    toItem(std::max(0, std::min(n - 1, item_ + (layout_ == Layout::Find ? steps : dir))));
    return;
  }
  if (layout_ == Layout::Emoji) return horizontal(dir);  // the tabs, or the keys under the list
  int n = int(keys_.size());
  do cursor_ = (cursor_ + dir + n) % n;
  while (keys_[cursor_].kind == Kind::Field);
}

void Keyboard::horizontal(int dir) {
  if (inList_) {
    if (layout_ == Layout::Find) return toItem(std::max(0, std::min(int(items_.size()) - 1, item_ + dir)));
    std::vector<int> row;
    for (size_t i = 0; i < items_.size(); i++)
      if (items_[i].row == items_[size_t(item_)].row) row.push_back(int(i));
    int at = int(std::find(row.begin(), row.end(), item_) - row.begin());
    return toItem(row[size_t((at + dir + int(row.size())) % int(row.size()))]);
  }
  std::vector<int> row;
  for (size_t i = 0; i < keys_.size(); i++)
    if (keys_[i].row == keys_[cursor_].row && keys_[i].kind != Kind::Field) row.push_back(int(i));
  int at = 0;
  for (size_t i = 0; i < row.size(); i++)
    if (row[i] == cursor_) at = int(i);
  cursor_ = row[size_t((at + dir + int(row.size())) % int(row.size()))];
  if (keys_[cursor_].kind == Kind::Tab) scrollToGroup(keys_[cursor_].group);  // the list follows the tab
}

void Keyboard::nextGroup(int dir) {
  bool onTab = !inList_ && keys_[cursor_].kind == Kind::Tab;
  if (layout_ != Layout::Emoji || (!inList_ && !onTab)) return horizontal(dir);
  int g = inList_ ? items_[size_t(item_)].group : keys_[cursor_].group;
  for (int n = g + dir; n >= 0 && n < emoji::kGroups; n += dir) {
    int i = firstOfGroup(n);
    if (i < 0) continue;
    if (onTab) return toTab(n);
    toItem(i);
    scrollTo_ = std::max(0, std::min(items_[size_t(i)].row, rowsOf() - 3));  // its first row on top
    return;
  }
}

void Keyboard::vertical(int dir) {
  const K& k = keys_[cursor_];
  int row = k.row + dir;
  if (row < 0 || row > 4) return;
  toKeyNear(row, k.col + k.span / 2.0f);
}

void Keyboard::down() {
  if (layout_ == Layout::Emoji) {
    if (inList_) {
      const Item& it = items_[size_t(item_)];
      if (it.row + 1 < rowsOf() && it.row + 1 <= scrollTo_ + 2) toItem(itemInRow(it.row + 1, it.col));
      else toKeyNear(4, it.col + 0.5f);
    } else if (keys_[cursor_].kind == Kind::Tab && !items_.empty()) {
      int r = std::min(scrollTo_, rowsOf() - 1);
      toItem(itemInRow(r, keys_[cursor_].col));
    }
    return;
  }
  if (layout_ == Layout::Find && inList_) return toKeyNear(1, items_[size_t(item_)].col - scrollTo_ + 0.5f);
  vertical(1);
}

bool Keyboard::up() {
  if (layout_ == Layout::Emoji) {
    if (inList_) {
      const Item& it = items_[size_t(item_)];
      if (it.row > scrollTo_) toItem(itemInRow(it.row - 1, it.col));
      else toTab(it.group);
    } else if (keys_[cursor_].kind == Kind::Tab) {
      if (pickOnly_) return false;
      switchTo(Layout::Lower);  // back to the letters, on 😊
    } else if (!items_.empty()) {
      int r = std::min(scrollTo_ + 2, rowsOf() - 1);
      toItem(itemInRow(r, keys_[cursor_].col));
    }
    return true;
  }
  if (layout_ == Layout::Find) {
    if (inList_ || (keys_[cursor_].row == 1 && items_.empty())) {
      switchTo(Layout::Emoji);
      return true;
    }
    if (keys_[cursor_].row == 1) {
      int col = std::max(0, std::min(int(items_.size()) - 1, scrollTo_ + keys_[cursor_].col));
      toItem(col);
      return true;
    }
    vertical(-1);
    return true;
  }
  if (keys_[cursor_].row == 0) return false;
  vertical(-1);
  return true;
}

void Keyboard::pick(const std::string& e, Result* r) {
  emoji::used(e);
  if (pickOnly_) {
    typed_ = e;
    *r = Result::Picked;
  } else {
    typed_ += e;
    *r = Result::Typed;
  }
}

Keyboard::Result Keyboard::press() {
  Result r = Result::None;
  if (inList_) {
    if (item_ < int(items_.size())) pick(items_[size_t(item_)].text, &r);
    return r;
  }
  const K k = keys_[cursor_];
  switch (k.kind) {
    case Kind::Tab: {
      int i = firstOfGroup(k.group);
      if (i >= 0) {
        toItem(i);
        scrollTo_ = std::max(0, std::min(items_[size_t(i)].row, rowsOf() - 3));
      }
      return r;
    }
    case Kind::Tone: {  // the next tone; the list in it, the highlight where it was
      emoji::setTone((emoji::tone() + 1) % emoji::kToneCount);
      int keep = cursor_, top = scrollTo_;
      build(Layout::Emoji);
      cursor_ = keep;
      buildList();
      scrollTo_ = std::max(0, std::min(top, rowsOf() - 3));
      return r;
    }
    case Kind::FindKey: switchTo(Layout::Find); return r;
    case Kind::EmojiKey: switchTo(Layout::Emoji); return r;
    case Kind::Action:
    case Kind::Extra: return Result::Action;
    case Kind::Field: return r;
    default: break;
  }
  const std::string& v = k.value;
  if (v == kLower) switchTo(Layout::Lower);
  else if (v == kUpper) switchTo(Layout::Upper);
  else if (v == kSymbols) switchTo(Layout::Symbols);
  else if (layout_ == Layout::Find) {  // the query, not the message
    if (v == kDelete) deleteLast(query_);
    else if (query_.size() < 40) query_ += v;
    buildFound();
  } else if (v == kDelete) {
    deleteLast(typed_);  // one character, or a whole emoji
    return Result::Delete;
  } else {
    typed_ += v;
    return Result::Typed;
  }
  return r;
}

void Keyboard::warm(Text& text, int bigPx, int smallPx) {
  const std::vector<int>& all = emoji::drawn(text);
  int tabs = text.font(Weight::Regular, float(smallPx)), grid = text.font(Weight::Regular, float(gridPx(bigPx)));
  for (int g = 0; g < emoji::kGroups; g++) text.prefetch(tabs, emoji::groupIcon(g));
  text.prefetch(text.font(Weight::Regular, float(bigPx)), kSmile);
  for (const std::string& e : emoji::recent()) text.prefetch(grid, e);
  for (size_t i = 0; i < all.size() && i < 30; i++) text.prefetch(grid, emoji::form(text, all[i], emoji::tone()));
}

void Keyboard::queueAhead(Text& text, int bigPx) {
  if (layout_ != Layout::Emoji || items_.empty()) return;
  int grid = text.font(Weight::Regular, float(gridPx(bigPx)));
  if (queueGroups_) {  // each group's first three rows (where its tab and held Next/Prev land) and the
    queueGroups_ = false;  // two before them (what a jump glides in through)
    std::vector<bool> near(size_t(rowsOf()) + 1, false);
    int group = -1;
    for (const Item& it : items_)
      if (it.group != group) {
        group = it.group;
        for (int r = it.row - 2; r <= it.row + 2; r++)
          if (r >= 0 && r < rowsOf()) near[size_t(r)] = true;
      }
    for (const Item& it : items_)
      if (near[size_t(it.row)]) text.prefetch(grid, it.text);
  }
  if (scrollTo_ != queuedTop_) {  // the rows just out of view, either way
    queuedTop_ = scrollTo_;
    for (const Item& it : items_)
      if ((it.row >= scrollTo_ - 3 && it.row < scrollTo_) || (it.row > scrollTo_ + 2 && it.row <= scrollTo_ + 5)) text.prefetch(grid, it.text);
  }
}

Keyboard::Result Keyboard::handle(Key key) {
  switch (key) {
    case Key::WheelCw: wheel(1); break;
    case Key::WheelCcw: wheel(-1); break;
    case Key::Next: right(); break;
    case Key::Prev: left(); break;
    case Key::Play: down(); break;
    case Key::Menu: return up() ? Result::None : Result::Close;
    case Key::Center: return press();
    default: break;
  }
  return Result::None;
}

Keyboard::Caption Keyboard::caption() const {
  auto count = [](size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
  if (layout_ != Layout::Emoji && layout_ != Layout::Find) return {};
  if (inList_ && item_ < int(items_.size())) {
    const Item& it = items_[size_t(item_)];
    std::string name = emoji::nameOf(it.text);
    return {name.empty() ? it.text : name, emoji::groupName(it.group)};
  }
  const K& k = keys_[cursor_];
  if (layout_ == Layout::Find) {
    if (query_.empty()) return {"Find an emoji", "type a word of its name: heart, cat, flag"};
    if (items_.empty()) return {"Nothing found", "try another word"};
    return {count(items_.size(), "emoji", "emoji") + " found", "Menu: up to them"};
  }
  switch (k.kind) {
    case Kind::Tab: {
      size_t n = 0;
      for (const Item& it : items_) n += it.group == k.group;
      if (k.group == 0 && !n) return {"Recent", "none yet: the ones you pick come here"};
      return {emoji::groupName(k.group), count(n, "emoji", "emoji")};
    }
    case Kind::Tone: {
      std::string t = emoji::toneName(emoji::tone());
      t[0] = char(toupper(t[0]));
      return {"Skin tone: " + t, "center: the next one"};
    }
    case Kind::FindKey: return {"Find", "an emoji by its name"};
    default: return {};
  }
}

float Keyboard::targetCol() const {
  if (!inList_) return float(keys_[cursor_].col);
  const Item& it = items_[size_t(item_)];
  return float(layout_ == Layout::Find ? it.col - scrollTo_ : it.col);
}
float Keyboard::targetRow() const {
  if (!inList_) return float(keys_[cursor_].row);
  return layout_ == Layout::Find ? 0.f : float(1 + items_[size_t(item_)].row - scrollTo_);
}
float Keyboard::targetSpan() const { return inList_ ? 1.f : float(keys_[cursor_].span); }

bool Keyboard::step(float dt) {
  if (inList_ && item_ >= int(items_.size())) inList_ = false, cursor_ = 0;
  float tx = targetCol(), ty = targetRow(), tw = targetSpan();
  if (!placed_) {
    hx_.snap(tx), hy_.snap(ty), hw_.snap(tw);
    scroll_.snap(float(scrollTo_));
    placed_ = true;
  }
  bool a = hx_.step(tx, dt, kOmega), b = hy_.step(ty, dt, kOmega), c = hw_.step(tw, dt, kOmega);
  // a long way (another group): cut to near it and glide the rest, not through every row between (each
  // new row is ten emoji to draw for the first time)
  float far = float(scrollTo_) - scroll_.x;
  if (fabsf(far) > 2.5f) scroll_.snap(float(scrollTo_) - copysignf(1.5f, far));
  bool d = scroll_.step(float(scrollTo_), dt, kOmega);
  return a || b || c || d;
}

// TvKeyboard.onDraw(): ten columns and five rows over the view, each key inset by 2 units on every side
// with 4-unit corners (YouTube: round, WhatsApp: 6). The highlighted key is the look's highlight with
// its label bold in the highlight's text color; it glides between keys, labels changing color as it
// passes. The emoji have no keys of their own: they sit on the background, the list scrolling under
// the tabs.
void Keyboard::draw(Canvas& c, Text& text, const Palette& p, Rect area, int bigPx, int smallPx, Icons* icons) {
  if (!text_) {
    text_ = &text;
    if (layout_ == Layout::Emoji) buildList();  // now with what the font draws
  }
  queueAhead(text, bigPx);
  const Look L = lookFor(style_, p);
  float cw = area.w / 10, rh = area.h / 5;
  float gap = float(int(floorf(2 * p.scale + 0.5f)));
  auto radiusFor = [&](float h) { return L.radius < 0 ? h / 2 : float(int(floorf(L.radius * p.scale + 0.5f))); };
  auto cell = [&](float col, float row, float span) {
    return Rect{area.x + col * cw + gap, area.y + row * rh + gap, span * cw - 2 * gap, rh - 2 * gap};
  };
  int layer = c.currentLayer();
  bool settled = fabsf(hx_.x - targetCol()) < 0.02f && fabsf(hy_.x - targetRow()) < 0.02f && fabsf(hw_.x - targetSpan()) < 0.02f;
  // how much of a spot the highlight covers
  auto cover = [&](float col, float row, float span, bool isCursor) {
    float on = fmaxf(0, 1 - fabsf(hx_.x - col) - fabsf(hy_.x - row) - fabsf(hw_.x - span));
    return isCursor ? on : fminf(on, 0.999f);
  };
  // the keys' fills
  for (size_t i = 0; i < keys_.size(); i++) {
    const K& k = keys_[i];
    if (k.kind == Kind::Tab) continue;  // the tabs are bare
    if (settled && !inList_ && int(i) == cursor_) continue;  // drawn once, in the highlight's color
    Color fill = k.kind == Kind::Action ? L.action : k.kind == Kind::Char || k.kind == Kind::Field ? L.key : L.special;
    Rect r = cell(float(k.col), float(k.row), float(k.span));
    c.layer(layer);
    c.shape(r, radiusFor(r.h), fill);
  }
  // the highlight
  Rect hr = cell(hx_.x, hy_.x, hw_.x);
  c.layer(layer + 1);
  c.shape(hr, radiusFor(hr.h), L.hl);
  // the keys' labels
  int small = text.font(Weight::Regular, float(smallPx));
  auto label = [&](const std::string& s, int px, float on, Rect r, Color base, bool emojiOnly) {
    int font = text.font(on > 0.5f && !emojiOnly ? Weight::Bold : Weight::Regular, float(px));  // an emoji has no bold
    const TextRun& run = text.run(font, s, r.w - 4);
    // Paint.Align.CENTER at the key's center; the baseline where ascent and descent balance
    float x = roundf(r.x + r.w / 2 - run.width / 2);
    float baseline = roundf(r.y + r.h / 2 + (text.ascent(font) - text.descent(font)) / 2);
    text.draw(c, run, x, baseline, emojiOnly ? (Color(uint32_t(255) << 24) | 0xffffff) : mix(base, L.hlText, on));
  };
  c.layer(layer + 2);
  for (size_t i = 0; i < keys_.size(); i++) {
    const K& k = keys_[i];
    Rect r = cell(float(k.col), float(k.row), float(k.span));
    float on = cover(float(k.col), float(k.row), float(k.span), !inList_ && int(i) == cursor_);
    Color base = k.kind == Kind::Action ? L.actionText : L.label;
    if (k.kind == Kind::Tab) {  // dim but the group in view, marked under it
      bool here = k.group == tab_;
      int font = text.font(Weight::Regular, float(smallPx));
      const TextRun& run = text.run(font, k.label);
      float baseline = roundf(r.y + r.h / 2 + (text.ascent(font) - text.descent(font)) / 2);
      text.draw(c, run, roundf(r.x + r.w / 2 - run.width / 2), baseline, Color(uint32_t(here || on > 0.5f ? 255 : 110) << 24) | 0xffffff);
      if (here) {
        float bw = roundf(r.w * 0.5f), bh = gap;
        c.shape({roundf(r.x + (r.w - bw) / 2), r.y + r.h - bh - gap, bw, bh}, bh / 2, mix(L.mark, L.hlText, on));
      }
      continue;
    }
    if (k.kind == Kind::Field) {  // Find's query, with the cursor, and how many matched
      Color col = L.label;
      float x = r.x + gap * 3;
      if (icons) {
        int g = smallPx;
        icons->draw(c, Icon::Search, {x, roundf(r.y + (r.h - g) / 2), float(g), float(g)}, L.muted);
        x += g + gap * 2;
      }
      std::string count = query_.empty() ? "" : std::to_string(items_.size());
      float cwid = count.empty() ? 0 : text.run(small, count).width + gap * 3;
      const TextRun& q = text.run(small, query_.empty() ? std::string("name of an emoji") : query_ + "|", r.x + r.w - x - cwid - gap * 2);
      float baseline = roundf(r.y + r.h / 2 + (text.ascent(small) - text.descent(small)) / 2);
      text.draw(c, q, roundf(x), baseline, query_.empty() ? L.muted : col);
      if (!count.empty()) {
        const TextRun& n = text.run(small, count);
        text.draw(c, n, roundf(r.x + r.w - gap * 3 - n.width), baseline, L.muted);
      }
      continue;
    }
    // the paper plane for WhatsApp's Send, the magnifier for Find
    Icon icon = Icon::Chevron;
    bool useIcon = false;
    if (icons && k.kind == Kind::Action && style_ == Style::WhatsApp && action_ == "Send") icon = Icon::Send, useIcon = true;
    if (icons && k.kind == Kind::FindKey && k.span == 1) icon = Icon::Search, useIcon = true;
    if (useIcon) {
      int g = k.kind == Kind::Action ? bigPx : smallPx + 2;
      icons->draw(c, icon, {roundf(r.x + (r.w - g) / 2), roundf(r.y + (r.h - g) / 2), float(g), float(g)}, mix(base, L.hlText, on));
      continue;
    }
    bool big = k.kind == Kind::EmojiKey || (k.kind == Kind::Char && k.label.size() == 1);
    label(k.label, big ? bigPx : smallPx, on, r, base, k.kind == Kind::EmojiKey);
  }
  // the list: three rows under the tabs, or Find's row on top
  if (layout_ == Layout::Emoji || layout_ == Layout::Find) {
    bool find = layout_ == Layout::Find;
    Rect clip = find ? Rect{area.x, area.y, area.w, rh} : Rect{area.x, area.y + rh, area.w, 3 * rh};
    c.pushClip(clip);
    int px = gridPx(bigPx);
    float s = scroll_.x;
    for (size_t i = 0; i < items_.size(); i++) {
      const Item& it = items_[i];
      float col = find ? it.col - s : float(it.col), row = find ? 0.f : 1 + it.row - s;
      if (find ? col <= -1 || col >= 10 : row <= 0 || row >= 4) continue;  // out of view: not drawn (nor made)
      Rect r = cell(col, row, 1);
      label(it.text, px, cover(col, row, 1, inList_ && int(i) == item_), r, L.label, true);
    }
    c.popClip();
    if (!find && rowsOf() > 3) {  // where the view is in the whole list
      float tx = area.x + area.w + 1, ty = area.y + rh + gap, th = 3 * rh - 2 * gap, tw = gap;
      float bh = fmaxf(gap * 4, th * 3 / rowsOf()), by = ty + (th - bh) * fminf(1, fmaxf(0, s / float(rowsOf() - 3)));
      c.layer(layer);
      c.shape({tx, ty, tw, th}, tw / 2, (L.label & 0x00ffffff) | 0x1a000000);
      c.layer(layer + 1);
      c.shape({tx, by, tw, bh}, tw / 2, L.mark);
    }
  }
  c.layer(layer);
}

}  // namespace rondo::ui
