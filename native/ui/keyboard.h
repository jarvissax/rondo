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
  const std::string& typed() const { return typed_; }
  bool step(float dt);  // true while the highlight moves
  void draw(Canvas& c, Text& text, const Palette& p, Rect area, int labelFont, int smallFont);

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
