// LyricsView; see lyrics_view.h.
#include "lyrics_view.h"

#include <ctype.h>
#include <math.h>

namespace rondo::shell {
namespace {

inline Color whiteA(int a) { return uint32_t(std::max(0, std::min(255, a))) << 24 | 0xffffff; }

// an LRC tag on a line of its own, like [by:...] or [ar:...]: not lyrics
bool isTag(const std::string& s) {
  if (s.size() < 4 || s[0] != '[' || s.back() != ']') return false;
  size_t i = 1;
  while (i < s.size() && (isalpha((unsigned char)s[i]) || s[i] == '#')) i++;
  if (i == 1 || i > 11 || i >= s.size() || s[i] != ':') return false;
  return s.find(']', i) == s.size() - 1;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

}  // namespace

void LyricsView::showMessage(const std::string& m) {
  message_ = m;
  lines_.clear();
  placed_ = false;
  animStart_ = 0;
}

void LyricsView::show(const Lyrics& l) {
  message_.clear();
  timed_ = !l.synced.empty();
  manual_ = false;
  lines_.clear();
  if (timed_) {
    for (auto& s : l.synced) lines_.push_back({s.t, s.text, {}, {}, 0, 0});
  } else {
    bool started = false;
    size_t start = 0;
    const std::string& p = l.plain;
    while (start <= p.size() && !p.empty()) {
      size_t nl = p.find_first_of("\r\n", start);
      std::string line = trim(p.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
      start = nl == std::string::npos ? p.size() + 1 : nl + (p[nl] == '\r' && nl + 1 < p.size() && p[nl + 1] == '\n' ? 2 : 1);
      if (isTag(line)) continue;
      if (!started && line.empty()) continue;
      started = true;
      lines_.push_back({0, line, {}, {}, 0, 0});
    }
  }
  if (lines_.empty()) message_ = l.instrumental ? "Instrumental" : "No lyrics found for this song";
  current_ = -1;
  fading_ = -2;
  animStart_ = 0;
  placed_ = false;
  dirty_ = true;
  laid_ = 0;
  laidY_ = 0;
}

void LyricsView::startLayout(float w, float h) {
  w_ = w, h_ = h;
  dirty_ = true;
  laid_ = 0;
  laidY_ = 0;
  placed_ = false;
}

bool LyricsView::layoutSome(int64_t budgetNs) {
  if (!layingOut()) return false;
  Env& e = *env_;
  int64_t until = now_ns() + budgetNs;
  float w = w_;
  gap_ = e.dpf(9);
  side_ = float(e.dp(16));
  // StaticLayout with spacing 1.1: each line is the font's height plus a tenth of it, rounded
  auto lineOf = [&](float sp, bool bold) {
    int lh = e.text.lineHeight(e.font(sp, bold));
    return float(lh + int(lh * 0.1f + 0.5f));
  };
  lineSmall_ = lineOf(15, false);
  lineBig_ = lineOf(17.5f, true);
  spacing_ = float(e.text.lineHeight(e.font(15, false)));
  float tw = w - 2 * side_, y = laidY_;
  for (; laid_ < lines_.size(); laid_++) {
    if (now_ns() > until) {
      laidY_ = y;
      return false;
    }
    Line& l = lines_[laid_];
    // a pause between verses shows as three dots
    std::string text = l.text.empty() ? (timed_ ? "\xe2\x80\xa2  \xe2\x80\xa2  \xe2\x80\xa2" : " ") : l.text;
    l.small = wrapText(e, text, 15, false, tw, 1000);
    if (l.small.empty()) l.small.push_back({" ", false});
    if (timed_) {
      l.big = wrapText(e, text, 17.5f, true, tw, 1000);
      if (l.big.empty()) l.big.push_back({" ", false});
    }
    float hs = l.small.size() * lineSmall_, hb = timed_ ? l.big.size() * lineBig_ : hs;
    l.slot = std::max(hs, hb);
    l.top = y;
    y += l.slot + gap_;
  }
  total_ = y;
  dirty_ = false;
  placed_ = false;
  return true;
}

int LyricsView::sungIndex(int64_t pos) const {
  int lo = 0, hi = int(lines_.size()) - 1, found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (lines_[mid].t <= pos) found = mid, lo = mid + 1;
    else hi = mid - 1;
  }
  return found;
}

float LyricsView::offsetAt(int64_t now) const {
  if (!animating(now)) return offset_;
  float x = float(now - animStart_) / float(kGlide);
  float f = 1 - powf(1 - x, 3.2f);  // DecelerateInterpolator(1.6)
  return slideFrom_ + (slideTo_ - slideFrom_) * f;
}

bool LyricsView::update(int64_t pos, int64_t dur, int64_t now, bool animate) {
  if (lines_.empty() || dirty_ || h_ <= 0) return false;
  if (!timed_) {
    if (manual_) return false;
    float next = dur > 0 ? maxScroll() * float(pos) / float(dur) : 0;
    bool moved = !placed_ || floorf(next) != floorf(offset_);
    offset_ = next;
    placed_ = true;
    return moved;
  }
  int i = sungIndex(pos);
  if (placed_ && i == current_) {
    if (animStart_ && !animating(now)) offset_ = slideTo_, animStart_ = 0, fading_ = -2;
    return false;
  }
  float target = anchorFor(i);
  float from = offsetAt(now);
  bool jump = !animate || !placed_ || fabsf(target - from) > h_ * 0.8f;
  int was = current_;
  current_ = i;
  placed_ = true;
  if (jump) {
    offset_ = target;
    animStart_ = 0;
    fading_ = -2;
  } else {
    fading_ = was;
    slideFrom_ = from;
    slideTo_ = target;
    offset_ = target;
    animStart_ = now;
  }
  return true;
}

int64_t LyricsView::wakeAt(int64_t pos, int64_t now) const {
  if (lines_.empty()) return 0;
  if (!timed_) return manual_ ? 0 : now + 100 * 1000000LL;
  if (current_ + 1 >= int(lines_.size())) return 0;
  int64_t wait = std::max<int64_t>(0, lines_[current_ + 1].t - pos);
  return now + wait * 1000000LL;
}

void LyricsView::scrollBy(int detents) {
  if (!scrollable()) return;
  manual_ = true;
  offset_ = std::max(0.f, std::min(offset_ + detents * spacing_ * 2, maxScroll()));
}

void LyricsView::pass(Env& e, Rect r, int highlight, float alpha, float offset) {
  float a = anchor();
  for (int i = 0; i < int(lines_.size()); i++) {
    const Line& l = lines_[i];
    float y = r.y + a + l.top - offset;
    if (y + l.slot < r.y || y > r.y + r.h) continue;
    bool sung = timed_ && i == highlight;
    int al = !timed_           ? 225
             : sung            ? 255
             : i < highlight   ? std::max(30, 80 - (highlight - i - 1) * 20)  // sung: fade upwards
                               : std::max(60, 125 - (i - highlight - 1) * 15);  // still to come
    const auto& rows = sung ? l.big : l.small;
    float line = sung ? lineBig_ : lineSmall_;
    float sp = sung ? 17.5f : 15;
    // centered in its slot, so the bigger sung line does not push its neighbours
    float top = y + (l.slot - rows.size() * line) / 2;
    Color c = whiteA(int(al * alpha));
    for (const WrappedLine& w : rows) {
      Label lb = label(e, w.text, sp, sung, w.ellipsize ? r.w - 2 * side_ : 0);
      drawLabel(e, lb, r.x + side_, floorf(top), c);
      top += line;
    }
  }
}

void LyricsView::draw(Env& e, Rect r, int64_t pos, int64_t dur) {
  if (!env_) env_.reset(new Env(e));
  if (w_ != r.w || h_ != r.h) startLayout(r.w, r.h);
  if (!message_.empty() || dirty_) {
    const std::string msg = !message_.empty() ? message_ : std::string("Finding lyrics\xe2\x80\xa6");
    // the message as a StaticLayout: left-aligned at the side, in the middle of the height
    float tw = r.w - 2 * float(e.dp(16));
    int lh = e.text.lineHeight(e.font(15, false));
    float line = float(lh + int(lh * 0.1f + 0.5f));
    std::vector<WrappedLine> rows;
    size_t start = 0;
    while (start <= msg.size()) {
      size_t nl = msg.find('\n', start);
      auto part = wrapText(e, msg.substr(start, nl == std::string::npos ? std::string::npos : nl - start), 15, false, tw, 1000);
      if (part.empty()) part.push_back({" ", false});
      rows.insert(rows.end(), part.begin(), part.end());
      start = nl == std::string::npos ? msg.size() + 1 : nl + 1;
    }
    float top = r.y + (r.h - rows.size() * line) / 2;
    for (const WrappedLine& w : rows) {
      drawLabel(e, label(e, w.text, 15, false, w.ellipsize ? tw : 0), r.x + float(e.dp(16)), floorf(top), whiteA(200));
      top += line;
    }
    return;
  }
  if (!placed_) update(pos, dur, e.now, false);
  float offset = offsetAt(e.now);
  e.c.pushClip(r);
  e.c.setFade(r.y, r.y + r.h, e.dpf(28));
  if (animating(e.now) && fading_ != -2) {
    // the highlight dissolves in the first 45 % of the glide (~200 ms) while the slide eases on
    float x = float(e.now - animStart_) / float(kGlide);
    float t = std::min(1.f, x / 0.45f);
    float fade = t * t * (3 - 2 * t);
    pass(e, r, fading_, 1 - fade, offset);
    pass(e, r, current_, fade, offset);
  } else {
    pass(e, r, current_, 1, offset);
  }
  e.c.clearFade();
  e.c.popClip();
}

}  // namespace rondo::shell
