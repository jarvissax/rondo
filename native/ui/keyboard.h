// The on-screen keyboard, navigated like a TV remote (today's TvKeyboard.kt): the wheel walks the keys
// in reading order, Next/Prev move right/left in the row, Play moves down, Menu moves up (Menu on the
// top row closes it), Center types. Lower case, upper case and symbols, so Wi-Fi passwords can be
// typed. The highlight glides between keys.
#pragma once
#include <string>
#include <vector>

#include "../platform/platform.h"
#include "motion.h"
#include "text.h"
#include "theme.h"

namespace rondo::ui {

class Keyboard {
 public:
  enum class Result { None, Typed, Delete, Action, Close };

  explicit Keyboard(std::string actionLabel = "Search", bool shell = false);
  // A key press (wheel pulses: Key::WheelCw/Ccw). Typed text is in typed().
  Result handle(Key k);
  // TvKeyboard.handle()'s actions: the next or previous key in reading order, right/left in the row,
  // down a row (Play), up a row (Menu; false on the top row: the screen closes the keyboard), type.
  void nextKey();
  void prevKey();
  void right() { horizontal(1); }
  void left() { horizontal(-1); }
  void down() { vertical(1); }
  bool up();
  Result press();
  void clear() { typed_.clear(); }
  void setTyped(std::string s) { typed_ = std::move(s); }
  const std::string& typed() const { return typed_; }
  // At most n characters (InputFilter.LengthFilter in 1.0.x): a key typed past it is taken back
  void keepAtMost(size_t n) {
    while (characters(typed_) > n && !typed_.empty()) {
      while (!typed_.empty() && (uint8_t(typed_.back()) & 0xc0) == 0x80) typed_.pop_back();
      if (!typed_.empty()) typed_.pop_back();
    }
  }
  // the characters (code points) of UTF-8 text
  static size_t characters(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += (uint8_t(c) & 0xc0) != 0x80;
    return n;
  }
  bool step(float dt);  // true while the highlight moves
  // bigPx, smallPx: the label sizes (1.0.x: dp(15) and dp(12), whole pixels)
  void draw(Canvas& c, Text& text, const Palette& p, Rect area, int bigPx, int smallPx);

 private:
  enum class Layout { Lower, Upper, Symbols };
  struct K {
    std::string label, value;
    int row, col, span;
  };
  void build(Layout l);
  void switchTo(Layout l);
  void horizontal(int dir);
  void vertical(int dir);

  std::string action_;
  bool shell_;
  Layout layout_ = Layout::Lower;
  std::vector<K> keys_;
  int cursor_ = 0;
  std::string typed_;
  Spring hx_, hy_, hw_;  // the highlight, in key units
  bool placed_ = false;
};

}  // namespace rondo::ui
