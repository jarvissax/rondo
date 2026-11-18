// The on-screen keyboard, navigated like a TV remote (today's TvKeyboard.kt): the wheel walks the keys
// in reading order, Next/Prev move right/left in the row, Play moves down, Menu moves up (Menu on the
// top row closes it), Center types. Lower case, upper case and symbols, so Wi-Fi passwords can be
// typed. It looks like the app it opens in (Rondo's shell, YouTube's, WhatsApp's); the highlight glides
// between keys.
//
// The emoji (the 😊 key beside #+=): ten tabs on top (Recent and Unicode's nine groups), every emoji the
// Y1's font draws under them in one list that scrolls three rows at a time, and the keys under it (the
// skin tone, Find, abc, Space, Del, the action). The wheel goes through the whole list (a fast spin a row
// at a time), Next/Prev along the row, held Next/Prev to the next or previous group, Play/Menu down and
// up a row (from the list to the keys under it or the tabs above); Menu on the tabs goes back to the
// letters. Find: type part of a name (heart, cat, flag); what matches is the row on top.
#pragma once
#include <string>
#include <vector>

#include "../platform/platform.h"
#include "motion.h"
#include "text.h"
#include "theme.h"

namespace rondo::ui {

class Icons;

class Keyboard {
 public:
  // Picked: an emoji chosen on an emoji-only keyboard (typed() is it)
  enum class Result { None, Typed, Delete, Action, Close, Picked };
  // the app it opens in: Rondo's shell (light or dark, the accent's action key), YouTube and YouTube
  // Music (round keys, the white pill, YouTube red), WhatsApp (its greens, Send as the paper plane)
  enum class Style { Shell, YouTube, WhatsApp };

  explicit Keyboard(std::string actionLabel = "Search", Style style = Style::Shell, bool emoji = true);
  // Emoji only (WhatsApp's reactions): opens on the emoji, center on one is Result::Picked, Menu on the
  // tabs Result::Close; extra (if any) is a key beside the tone's (Take back) that is Result::Action
  static Keyboard picker(Style style, std::string extra = "");
  // the last character of UTF-8 text gone: a whole emoji (with its skin tone, variation and joined
  // parts), else one code point
  static void deleteLast(std::string& s);
  // A key press (wheel pulses: Key::WheelCw/Ccw). Typed text is in typed().
  Result handle(Key k);
  // TvKeyboard.handle()'s actions: the next or previous key in reading order (steps: the wheel's spin),
  // right/left in the row, the next or previous emoji group (held Next/Prev; elsewhere right/left), down
  // a row (Play), up a row (Menu; false on the top row: the screen closes the keyboard), type.
  void wheel(int steps);
  void nextKey() { wheel(1); }
  void prevKey() { wheel(-1); }
  void right() { horizontal(1); }
  void left() { horizontal(-1); }
  void nextGroup(int dir);
  void down();
  bool up();
  Result press();
  void clear() { typed_.clear(); }
  void setTyped(std::string s) { typed_ = std::move(s); }
  const std::string& typed() const { return typed_; }
  // At most n characters (InputFilter.LengthFilter in 1.0.x): a key typed past it is taken back
  void keepAtMost(size_t n) {
    while (characters(typed_) > n && !typed_.empty()) deleteLast(typed_);
  }
  // the characters (code points) of UTF-8 text
  static size_t characters(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += (uint8_t(c) & 0xc0) != 0x80;
    return n;
  }
  // The line under the keyboard while on the emoji: what the highlighted one is ("Thumbs up" and its
  // group), or what a key does; empty on the letters (the screen says how to move there)
  struct Caption {
    std::string strong, soft;
    bool empty() const { return strong.empty() && soft.empty(); }
  };
  Caption caption() const;
  bool step(float dt);  // true while the highlight moves
  // bigPx, smallPx: the label sizes (1.0.x: dp(15) and dp(12), whole pixels); icons: the action key's
  // paper plane and Find's magnifier (null: words)
  void draw(Canvas& c, Text& text, const Palette& p, Rect area, int bigPx, int smallPx, Icons* icons = nullptr);
  // While Rondo starts, idle: what the emoji pages draw, and their first page, made ready (Text::prefetch)
  static void warm(Text& text, int bigPx, int smallPx);

 private:
  enum class Layout { Lower, Upper, Symbols, Emoji, Find };
  enum class Kind : uint8_t { Char, Special, Action, Extra, EmojiKey, Tab, Tone, FindKey, Field };
  struct K {
    std::string label, value;
    int row, col, span;
    Kind kind = Kind::Char;
    int group = 0;  // a tab's
  };
  // what scrolls: the emoji list (three rows of ten) or what Find found (one row, sideways)
  struct Item {
    std::string text;
    int index;  // into emoji::kList (-1: unknown)
    int group, row, col;
  };
  void build(Layout l);
  void switchTo(Layout l);
  void buildList();
  void buildFound();
  void horizontal(int dir);
  void vertical(int dir);
  void toItem(int i);       // the highlight onto the list's item i, scrolled into view
  void toKeyNear(int row, float center);
  void toTab(int group);
  void scrollToGroup(int group);
  int firstOfGroup(int group) const;
  int itemInRow(int row, int col) const;  // the item at col of the list's row (its last when shorter)
  int rowsOf() const { return items_.empty() ? 0 : items_.back().row + 1; }
  int visibleRows() const { return layout_ == Layout::Emoji ? 3 : 1; }
  void pick(const std::string& e, Result* r);
  // the highlight's target, in key units
  float targetCol() const, targetRow() const, targetSpan() const;

  std::string action_, extra_;
  Style style_;
  bool emoji_, pickOnly_ = false;
  Layout layout_ = Layout::Lower;
  std::vector<K> keys_;
  int cursor_ = 0;
  bool inList_ = false;  // on the list (items_), not a key
  int item_ = 0;
  std::vector<Item> items_;
  std::string query_;  // Find's
  std::vector<std::string> recent_;  // the recent ones as they were when the emoji opened (what's picked comes in next time)
  int scrollTo_ = 0;   // the list's first row in view (Find: its first column)
  Spring scroll_;
  int tab_ = 1;        // the group in view: its tab is marked
  std::string typed_;
  Spring hx_, hy_, hw_;  // the highlight, in key units
  bool placed_ = false;
  Text* text_ = nullptr;  // the emoji font's, for what it draws (from the first draw)
  // the emoji ahead, made ready while idle: each group's first rows once the list is built, the rows
  // around the view as it moves
  bool queueGroups_ = false;
  int queuedTop_ = -100;
  void queueAhead(Text& text, int bigPx);
};

}  // namespace rondo::ui
