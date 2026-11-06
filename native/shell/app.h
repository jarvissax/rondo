// The shell's screen stack (ShellActivity in 1.0.x) and the screens every list is built on.
#pragma once
#include <stdio.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../core/clock.h"
#include "../core/config.h"
#include "../core/library.h"
#include "../player/player.h"
#include "dispatch.h"
#include "kit.h"

namespace rondo::shell {

extern Config g_config;
extern Library g_library;
extern PlayerHost g_player;
// RONDO_SLOW=1: frames over 8 ms print where their time went (main.cpp, App::draw)
extern bool g_traceSlow;

class App;

class Screen {
 public:
  virtual ~Screen() = default;
  virtual std::string title() const = 0;
  // An action the screen uses (true), or leaves to the shell's global meaning.
  virtual bool onAction(Action a, int steps) { return false; }
  virtual bool step(float dt) { return false; }
  // The content under the status bar, at the canvas's current offset.
  virtual void draw(Env& e, float w, float h) = 0;
  virtual void onShow() {}
  // false: the screen draws its own header (Now Playing) and gets the whole screen
  virtual bool statusBar() const { return true; }
  // While nothing moves the loop looks every 250 ms and at wakeAt(): true when what the screen shows
  // changed (the player's position, a timer running out).
  virtual bool tick(int64_t now) { return false; }
  virtual int64_t wakeAt() const { return 0; }
};

// ShellActivity: one stack of screens; the main menu is its bottom and never closes.
class App {
 public:
  void push(std::unique_ptr<Screen> s) {
    if (s) {
      finishTransition();
      stack_.push_back(std::move(s));
      stack_.back()->onShow();
      transition_.begin(0, 1, now_, 250 * kMs);
      popping_ = false;
    }
  }
  void pop() {
    if (stack_.size() < 2) return;
    finishTransition();
    leaving_ = std::move(stack_.back());
    stack_.pop_back();
    stack_.back()->onShow();
    transition_.begin(0, 1, now_, 250 * kMs);
    popping_ = true;
  }
  void action(Action a, int steps) {
    if (stack_.back()->onAction(a, steps)) return;
    // the shell's global meanings (ShellActivity.onWheel)
    switch (a) {
      case Action::Back: pop(); break;
      case Action::PlayPause: g_player.togglePlay(); break;
      case Action::Next: g_player.next(); break;
      case Action::Prev: g_player.previous(); break;
      case Action::LongNext:
      case Action::LongPrev: {  // 2 % of the length per repeat, at least 10 s
        int64_t d = g_player.state().durationMs;
        int64_t step = std::max<int64_t>(10000, d / 50);
        g_player.seekBy(a == Action::LongNext ? step : -step);
        break;
      }
      default: break;
    }
  }
  void setNow(int64_t now) { now_ = now; }
  bool tick(int64_t now) {
    bool changed = stack_.back()->tick(now);
    if (volumeUntil_ && now >= volumeUntil_) volumeUntil_ = 0, changed = true;
    return changed;
  }
  int64_t wakeAt() const {
    int64_t t = stack_.back()->wakeAt();
    if (volumeUntil_ && (!t || volumeUntil_ < t)) t = volumeUntil_;
    return t;
  }
  // ShellActivity.changeVolume(): one step of STREAM_MUSIC and the volume panel for 1.5 s
  void changeVolume(int dir) {
    PlayerState s = g_player.state();
    int max = s.volumeMax > 0 ? s.volumeMax : 15;
    volumeLevel_ = std::max(0, std::min(max, (volumeUntil_ ? volumeLevel_ : s.volume) + dir));
    volumeMax_ = max;
    volumeUntil_ = now_ns() + 1500 * kMs;
    g_player.changeVolume(dir);
  }
  const Status& status() const { return status_; }
  bool step(float dt) {
    tick(now_);
    bool moving = false;
    for (auto& s : stack_) moving |= s->step(dt);
    if (leaving_) moving |= leaving_->step(dt);
    if (transition_.running(now_)) moving = true;
    else leaving_.reset();
    return moving;
  }
  void draw(Env& e, float W, float H, const Status& status) {
    status_ = status;
    float bar = float(e.dp(22));
    Screen* top = stack_.back().get();
    Screen* other = popping_ ? leaving_.get() : (stack_.size() > 1 ? stack_[stack_.size() - 2].get() : nullptr);
    bool sliding = transition_.running(now_) && other;
    float p = sliding ? transition_.at(now_) : 1;
    // The status bar stays while both screens have one (its title cross-fades); a screen without one
    // slides whole, and the bar with the other.
    bool fixedBar = top->statusBar() && (!sliding || other->statusBar());
    auto content = [&](Screen* s, float dx) {
      float y = s->statusBar() ? bar : 0;
      e.c.pushOffset(dx, y);
      e.c.pushClip({0, 0, W, H - y});
      int64_t d0 = g_traceSlow ? now_ns() : 0;
      s->draw(e, W, H - y);
      if (g_traceSlow && now_ns() - d0 > 4 * kMs) printf("slow: screen %s drew in %.1f ms\n", s->title().c_str(), (now_ns() - d0) / 1e6);
      e.c.popClip();
      e.c.popOffset();
      if (!fixedBar && s->statusBar()) {
        e.c.pushOffset(dx, 0);
        e.c.layer(6);
        statusBar(e, W, status, s->title());
        e.c.layer(0);
        e.c.popOffset();
      }
    };
    if (sliding) {
      // push: the new screen comes in from the right and pushes the old one out; pop reverses it
      float off = popping_ ? W * p : -W * p;
      content(other, roundf(off));
      content(top, roundf(popping_ ? off - W : off + W));
    } else {
      content(top, 0);
    }
    if (fixedBar) {
      e.c.layer(6);
      statusBar(e, W, status, sliding ? other->title() : top->title(), sliding ? top->title() : "", sliding ? p : 0);
      e.c.layer(0);
    }
    if (volumeUntil_) drawVolume(e, W, H);
  }

 private:
  // A frame skipped (see the loop): what started moving starts one refresh later instead of jumping.
 public:
  void delay(int64_t by) { transition_.start += by; }

 private:
  void drawVolume(Env& e, float W, float H);
  void finishTransition() {
    transition_.duration = 0;
    leaving_.reset();
  }
  std::vector<std::unique_ptr<Screen>> stack_;
  std::unique_ptr<Screen> leaving_;
  Tween transition_;
  bool popping_ = false;
  int64_t now_ = 0;
  Status status_;
  int volumeLevel_ = 0, volumeMax_ = 15;
  int64_t volumeUntil_ = 0;
};

extern App* g_app;

// ProgressBarView: a rounded track, the played part, an optional round thumb and the scrub marker
// (marker < 0: none). In view coordinates (x, y, w, h).
void progressBar(Env& e, Rect r, Color track, Color played, float fraction, int thumbDp, int barDp, float marker = -1);

// ShellListScreen: the status bar's title and a RowList 3 units below it.
class ShellListScreen : public Screen {
 public:
  explicit ShellListScreen(std::string title) : title_(std::move(title)) {}
  std::string title() const override { return title_; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.down(steps);
      case Action::Up: return list_.up(steps);
      case Action::Select: return list_.select();
      case Action::LongSelect: return list_.longSelect();
      default: return false;
    }
  }
  bool step(float dt) override { return list_.step(dt); }
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.shellBg);
    float pad = float(e.dp(3));
    list_.layout(e, w, h - pad);
    list_.draw(e, 0, pad);
  }

 protected:
  std::string title_;
  RowList list_;
};

// LetterListScreen: Next/Prev jump to the next or previous first letter, like the iPod's letter overlay.
class LetterListScreen : public ShellListScreen {
 public:
  using ShellListScreen::ShellListScreen;
  bool onAction(Action a, int steps) override {
    if (letters_.size() > 1 && (a == Action::Next || a == Action::Prev)) {
      int at = list_.selected();
      int target = -1;
      if (a == Action::Next) {
        for (auto& l : letters_)
          if (l.first > at) {
            target = l.first;
            break;
          }
      } else {
        for (auto& l : letters_)
          if (l.first < at) target = l.first;
      }
      if (target >= 0) list_.selectIndex(target);
      return true;
    }
    return ShellListScreen::onAction(a, steps);
  }

 protected:
  // the row index of each letter's first row (rows that aren't the search row); others go to '#'
  void indexLetters() {
    letters_.clear();
    std::vector<uint32_t> seen;
    const auto& rows = list_.rows();
    for (int i = 0; i < int(rows.size()); i++) {
      const Row& r = rows[i];
      if (r.header || r.tile == &tiles::search || r.title.empty()) continue;
      uint32_t c = firstLetter(r.title);
      if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
      seen.push_back(c);
      letters_.push_back({i, c});
    }
  }
  static uint32_t firstLetter(const std::string& s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return isalpha(c) ? uint32_t(toupper(c)) : '#';
    // a letter outside ASCII: its UTF-8 lead bytes are enough to tell letters apart
    uint32_t cp = c;
    for (size_t i = 1; i < s.size() && i < 4 && ((unsigned char)s[i] & 0xc0) == 0x80; i++) cp = cp << 8 | (unsigned char)s[i];
    return cp;
  }
  std::vector<std::pair<int, uint32_t>> letters_;
};

}  // namespace rondo::shell
