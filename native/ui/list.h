// A wheel-driven list: one pulse moves the selection at once (the very next frame), and the selection
// bar and the scroll follow with springs that every pulse retargets. Like the iPod, the list only
// scrolls when the selection would leave the view.
#pragma once
#include <math.h>

#include <algorithm>

#include "motion.h"

namespace rondo::ui {

class WheelList {
 public:
  void configure(int count, float rowHeight, float viewHeight) {
    count_ = count;
    rowH_ = rowHeight;
    viewH_ = viewHeight;
    selected_ = std::clamp(selected_, 0, std::max(0, count - 1));
    targetScroll_ = clampScroll(targetScroll_);
    bar_.snap(selected_ * rowH_);
    scroll_.snap(targetScroll_);
  }

  // A wheel pulse (+1 down, -1 up). Returns false at either end.
  bool move(int delta) {
    int next = std::clamp(selected_ + delta, 0, std::max(0, count_ - 1));
    if (next == selected_) return false;
    selected_ = next;
    float top = selected_ * rowH_, bottom = top + rowH_;
    if (top < targetScroll_) targetScroll_ = top;
    if (bottom > targetScroll_ + viewH_) targetScroll_ = bottom - viewH_;
    targetScroll_ = clampScroll(targetScroll_);
    return true;
  }
  void select(int index) {
    selected_ = std::clamp(index, 0, std::max(0, count_ - 1));
    targetScroll_ = clampScroll(selected_ * rowH_ - (viewH_ - rowH_) / 2);
    bar_.snap(selected_ * rowH_);
    scroll_.snap(targetScroll_);
  }

  // Advances the springs by dt seconds; true while anything moves.
  bool step(float dt) {
    bool a = bar_.step(selected_ * rowH_, dt, kOmega);
    bool b = scroll_.step(targetScroll_, dt, kOmega);
    return a || b;
  }

  int selected() const { return selected_; }
  int count() const { return count_; }
  float rowHeight() const { return rowH_; }
  float scroll() const { return scroll_.x; }
  float barY() const { return bar_.x - scroll_.x; }  // in view coordinates
  // The rows that touch the view now.
  int first() const { return std::max(0, int(floorf(scroll_.x / rowH_))); }
  int last() const { return std::min(count_ - 1, int(floorf((scroll_.x + viewH_ - 0.01f) / rowH_))); }

 private:
  // ~95 % of the way in 110 ms: the selection glide of the motion spec
  static constexpr float kOmega = 43;

  float clampScroll(float s) const { return std::clamp(s, 0.0f, std::max(0.0f, count_ * rowH_ - viewH_)); }

  int count_ = 0, selected_ = 0;
  float rowH_ = 1, viewH_ = 1, targetScroll_ = 0;
  Spring bar_, scroll_;
};

}  // namespace rondo::ui
