// LyricsView (1.0.x): lyrics that follow the song. Timed lyrics: the sung line is white, larger and bold
// at 40 % of the height, lines already sung fade out above, the next ones wait below in gray; the list
// glides to each new line exactly on its timestamp while the highlight cross-fades. Untimed lyrics
// scroll with the song's progress until the wheel takes over. The top and bottom edges fade.
//
// 1.0.x drew the lines once into bitmaps to spare Android 4.2's renderer; here they are atlas glyphs,
// drawn every frame of the glide.
#pragma once
#include "../core/lyrics.h"
#include "kit.h"

namespace rondo::shell {

class LyricsView {
 public:
  void showMessage(const std::string& m);
  void show(const Lyrics& l);
  // Follows the song: true when what is drawn changed. animate: glide to a new line (else jump).
  bool update(int64_t posMs, int64_t durMs, int64_t now, bool animate = true);
  bool animating(int64_t now) const { return animStart_ && now < animStart_ + kGlide; }
  // When it must look again: the next timed line's start, or in 100 ms while untimed lyrics scroll.
  int64_t wakeAt(int64_t posMs, int64_t now) const;
  // untimed lyrics only: the wheel reads through them (about two lines a detent)
  bool scrollable() const { return !timed_ && !lines_.empty(); }
  void scrollBy(int detents);
  void draw(Env& e, Rect r, int64_t posMs, int64_t durMs);

 private:
  static constexpr int64_t kGlide = 450 * 1000000LL;
  struct Line {
    int64_t t = 0;
    std::string text;
    std::vector<WrappedLine> small, big;
    float top = 0, slot = 0;
  };
  void relayout(Env& e, float w, float h);
  float anchor() const { return timed_ ? h_ * 0.40f : h_ * 0.15f; }
  float anchorFor(int i) const { return i < 0 ? -lines_[0].slot / 2 - gap_ * 2 : lines_[i].top + lines_[i].slot / 2; }
  float maxScroll() const { return std::max(0.f, total_ - h_ * 0.35f); }
  int sungIndex(int64_t pos) const;
  float offsetAt(int64_t now) const;
  void pass(Env& e, Rect r, int highlight, float alpha, float offset);

  std::string message_ = "Finding lyrics\xe2\x80\xa6";
  std::vector<Line> lines_;
  bool timed_ = false, manual_ = false, dirty_ = true, placed_ = false;
  float w_ = 0, h_ = 0, total_ = 0, gap_ = 12.6f, side_ = 22, lineSmall_ = 26, lineBig_ = 32, spacing_ = 24.6f;
  int current_ = -1, fading_ = -2;
  float offset_ = 0, slideFrom_ = 0, slideTo_ = 0;
  int64_t animStart_ = 0;
};

}  // namespace rondo::shell
