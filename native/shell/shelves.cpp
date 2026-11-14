// shelves.h.
#include "shelves.h"

#include <math.h>

#include <algorithm>

namespace rondo::shell {

namespace {
// the motion: the page and the shelves glide as the lists do (RowList's 43), the focus card a touch
// quicker so it leads
constexpr float kGlide = 38, kFocus = 46;
}  // namespace

void ShelfView::setShelves(std::vector<Shelf> shelves, bool keepSelection) {
  int was = at_;
  shelves_ = std::move(shelves);
  order_.clear();
  for (int s = 0; s < int(shelves_.size()); s++)
    for (int c = 0; c < int(shelves_[size_t(s)].cards.size()); c++) order_.push_back({s, c});
  at_ = keepSelection && was < int(order_.size()) ? was : 0;
  slide_.assign(shelves_.size(), Spring());
  slideTo_.assign(shelves_.size(), 0.f);
  w_ = 0;  // laid out again
  placed_ = false;
}

void ShelfView::appendShelves(std::vector<Shelf> more) {
  for (Shelf& s : more) {
    int at = int(shelves_.size());
    for (int c = 0; c < int(s.cards.size()); c++) order_.push_back({at, c});
    shelves_.push_back(std::move(s));
    slide_.push_back(Spring());
    slideTo_.push_back(0.f);
  }
  w_ = 0;  // laid out again; the place stays
}

int ShelfView::artW(const Card& c) const { return c.shape == Card::Shape::Wide ? artH_ * 16 / 9 : artH_; }

void ShelfView::layout(Env& e, float w, float h) {
  if (w == w_ && h == h_ && top_.size() == shelves_.size()) {
    if (!placed_ && !order_.empty()) place(true), placed_ = true;
    return;
  }
  w_ = w, h_ = h;
  // a shelf: its title (15 bold), its covers 92 units tall, two lines under each (12 and 10)
  artH_ = e.dp(92), gap_ = e.dp(10), pad_ = e.dp(10), titleH_ = e.dp(26), textH_ = e.dp(36), bannerH_ = e.dp(48);
  top_.assign(shelves_.size(), 0.f);
  height_.assign(shelves_.size(), 0.f);
  x_.assign(shelves_.size(), {});
  float y = float(e.dp(2));
  for (size_t s = 0; s < shelves_.size(); s++) {
    const Shelf& sh = shelves_[s];
    top_[s] = y;
    if (sh.banner) {
      height_[s] = float(bannerH_);
      x_[s] = {float(pad_)};
    } else {
      height_[s] = float((sh.title.empty() ? e.dp(4) : titleH_) + artH_ + textH_ + e.dp(6));
      float x = float(pad_);
      for (const Card& c : sh.cards) {
        x_[s].push_back(x);
        x += float(artW(c) + gap_);
      }
    }
    y += height_[s];
  }
  if (!order_.empty()) place(!placed_), placed_ = true;
}

const Card* ShelfView::selected() const {
  if (order_.empty() || at_ >= int(order_.size())) return nullptr;
  auto [s, c] = order_[size_t(at_)];
  return &shelves_[size_t(s)].cards[size_t(c)];
}

void ShelfView::place(bool snap) {
  if (order_.empty() || top_.size() != shelves_.size()) return;
  auto [s, c] = order_[size_t(at_)];
  const Shelf& sh = shelves_[size_t(s)];
  int fpad = std::max(1, artH_ / 23);  // the focus card's margin around a cover (4 units)
  float cardX, cardW, cardTop, cardH;
  if (sh.banner) {
    cardX = float(pad_ / 2), cardW = w_ - float(pad_), cardTop = top_[size_t(s)] + float(fpad), cardH = float(bannerH_ - 2 * fpad);
    slideTo_[size_t(s)] = 0;
  } else {
    // sideways: the card whole, the next one peeking in from the right while there is one
    const Card& card = sh.cards[size_t(c)];
    float x = x_[size_t(s)][size_t(c)], cw = float(artW(card));
    float last = x_[size_t(s)].back() + float(artW(sh.cards.back()));
    float peek = c + 1 < int(sh.cards.size()) ? float(artH_) * 0.4f : 0.f;
    float& off = slideTo_[size_t(s)];
    if (x - off < float(pad_)) off = x - float(pad_);
    if (x + cw - off > w_ - float(pad_) - peek) off = x + cw - (w_ - float(pad_) - peek);
    off = std::max(0.f, std::min(off, std::max(0.f, last + float(pad_) - w_)));
    cardX = x - off - float(fpad), cardW = cw + float(2 * fpad);
    cardTop = top_[size_t(s)] + float(sh.title.empty() ? artH_ / 23 : titleH_) - float(fpad);
    cardH = float(artH_ + textH_);
  }
  // up and down: the shelf whole (the first one from the page's top)
  float top = top_[size_t(s)], bottom = top + height_[size_t(s)];
  if (s == 0) scrollTo_ = 0;
  else if (top < scrollTo_) scrollTo_ = top;
  else if (bottom > scrollTo_ + h_) scrollTo_ = bottom - h_;
  scrollTo_ = std::max(0.f, scrollTo_);
  fxTo_ = cardX, fyTo_ = cardTop, fwTo_ = cardW;
  (void)cardH;
  if (snap) {
    scroll_.snap(scrollTo_);
    for (size_t i = 0; i < slide_.size(); i++) slide_[i].snap(slideTo_[i]);
    fx_.snap(fxTo_), fy_.snap(fyTo_), fw_.snap(fwTo_);
  }
}

void ShelfView::move(int by) {
  int to = std::max(0, std::min(int(order_.size()) - 1, at_ + by));
  if (to == at_) return;
  at_ = to;
  place(false);
  if (onNearEnd && order_[size_t(at_)].first >= int(shelves_.size()) - 2) onNearEnd();
}

bool ShelfView::down(int steps) {
  if (order_.empty()) return false;
  move(steps);
  return true;
}

bool ShelfView::up(int steps) {
  if (order_.empty() || at_ == 0) return false;
  move(-steps);
  return true;
}

bool ShelfView::select() {
  const Card* c = selected();
  if (!c || !c->onSelect) return false;
  auto f = c->onSelect;  // the card may go with what it opens
  f();
  return true;
}

bool ShelfView::longSelect() {
  const Card* c = selected();
  if (!c || !c->onLongSelect) return false;
  auto f = c->onLongSelect;
  f();
  return true;
}

bool ShelfView::step(float dt) {
  if (order_.empty()) return false;
  bool moving = scroll_.step(scrollTo_, dt, kGlide);
  for (size_t i = 0; i < slide_.size(); i++) moving = slide_[i].step(slideTo_[i], dt, kGlide) || moving;
  // the focus card follows its card as the shelf slides under it (its target moves with the slide)
  if (!order_.empty()) {
    auto [s, c] = order_[size_t(at_)];
    if (!shelves_[size_t(s)].banner) {
      int fpad = std::max(1, artH_ / 23);
      fxTo_ = x_[size_t(s)][size_t(c)] - slideTo_[size_t(s)] - float(fpad);
    }
  }
  moving = fx_.step(fxTo_, dt, kFocus) || moving;
  moving = fy_.step(fyTo_, dt, kFocus) || moving;
  moving = fw_.step(fwTo_, dt, kFocus) || moving;
  return moving;
}

void ShelfView::draw(Env& e, float x, float y) {
  const Palette& p = e.p;
  Canvas& c = e.c;
  if (order_.empty()) return;
  c.pushClip({x, y, w_, h_});
  float scroll = scroll_.x;
  int fpad = std::max(1, artH_ / 23);
  auto [selS, selC] = order_[size_t(at_)];
  // the focus card, under the chosen one
  if (active) {
    const Shelf& sh = shelves_[size_t(selS)];
    float fh = sh.banner ? float(bannerH_ - 2 * fpad) : float(artH_ + textH_);
    c.layer(1);
    c.shape({x + fx_.x, y + fy_.x - scroll, fw_.x, fh}, e.dpf(7), p.focus);
  }
  for (size_t s = 0; s < shelves_.size(); s++) {
    float top = y + top_[s] - scroll;
    if (top + height_[s] < y || top > y + h_) continue;
    const Shelf& sh = shelves_[s];
    if (sh.banner) {  // as a list's MUSIC row: tile, title and subtitle
      const Card& card = sh.cards[0];
      float sel = active && int(s) == selS ? 1.f : 0.f;
      float left = x + float(pad_ / 2) + e.dp(6), ctop = top + float(fpad);
      int ch = bannerH_ - 2 * fpad, art = e.dp(34);
      Rect ar{left, ctop + float((ch - art) / 2), float(art), float(art)};
      c.layer(2);
      if (card.tile) drawTile(e, *card.tile, ar, 1);
      else c.shape(ar, e.dpf(3), p.raised);
      float colX = left + art + e.dp(8), colW = x + w_ - float(pad_) - e.dp(8) - colX;
      c.layer(4);
      Label tl = label(e, card.title, 13, sel > 0.5f, colW);
      Label sl;
      if (!card.subtitle.empty()) sl = label(e, card.subtitle, 11, false, colW);
      int colH = tl.h + (card.subtitle.empty() ? 0 : e.dp(2) + sl.h);
      float ty = ctop + float((ch - colH) / 2);
      drawLabel(e, tl, colX, ty, mix(p.white, p.focusText, sel));
      if (!card.subtitle.empty()) drawLabel(e, sl, colX, ty + tl.h + e.dp(2), mix(p.darkMuted, p.focusMuted, sel));
      continue;
    }
    float rowTop = top;
    if (!sh.title.empty()) {
      c.layer(3);
      Label l = label(e, sh.title, 15, true, w_ - float(2 * pad_));
      drawLabel(e, l, x + float(pad_), top + float(titleH_ - e.dp(6) - l.h), p.white);
      rowTop += float(titleH_);
    } else {
      rowTop += float(artH_ / 23);
    }
    float slide = slide_[s].x;
    for (size_t k = 0; k < sh.cards.size(); k++) {
      const Card& card = sh.cards[k];
      int cw = artW(card);
      float cx = x + x_[s][k] - slide;
      if (cx > x + w_ || cx + float(cw) < x) continue;
      // how much the focus card is on this one (its text darkens as it arrives)
      float sel = 0;
      if (active && int(s) == selS && int(k) == selC) {
        float dx = fabsf(fx_.x + float(fpad) - (cx - x)), dy = fabsf(fy_.x + float(fpad) - (rowTop - y + scroll));
        sel = clamp01(1 - (dx + dy) / float(cw));
      }
      Rect ar{cx, rowTop, float(cw), float(artH_)};
      float radius = card.shape == Card::Shape::Round ? float(artH_) / 2 : e.dpf(card.shape == Card::Shape::Wide ? 5 : 4);
      c.layer(2);
      float a = 1;
      const Texture* tex = card.thumb.empty() ? nullptr : e.covers.get(card.thumb, cw, artH_, 0, e.now, &a);
      if (!tex || a < 1) c.shape(ar, radius, p.raised);
      c.layer(3);
      if (tex) c.image(*tex, ar, {0, 0, 1, 1}, radius, withAlpha(0xffffffff, a));
      // two lines under it (a round artist's centered, as the app has them)
      c.layer(4);
      float tw = float(cw);
      Label tl = label(e, card.title, 12, sel > 0.5f, tw);
      float ty = rowTop + float(artH_) + e.dp(4);
      float tx = card.shape == Card::Shape::Round ? centerText(cx, tw, tl.w) : cx;
      drawLabel(e, tl, tx, ty, mix(p.white, p.focusText, sel));
      if (!card.subtitle.empty()) {
        Label sl = label(e, card.subtitle, 10, false, tw);
        float sx = card.shape == Card::Shape::Round ? centerText(cx, tw, sl.w) : cx;
        drawLabel(e, sl, sx, ty + tl.h + e.dp(1), mix(p.darkMuted, p.focusMuted, sel));
      }
    }
  }
  c.layer(0);
  c.popClip();
}

}  // namespace rondo::shell
