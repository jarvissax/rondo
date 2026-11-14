// Shelves: a page of titled rows of cards, as YouTube Music's app shows its home (MusicCarouselShelf):
// each shelf a row of covers that slides sideways under its title. One card is chosen at a time and
// the wheel moves through them in reading order, along a shelf and on into the next; the chosen card
// sits on the white focus card (the lists' MUSIC style), its shelf slides to keep it in view (with the
// next one peeking) and the page glides to keep its shelf whole. A banner is a shelf of one card the
// page's width (Downloads, a message to try again).
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "kit.h"

namespace rondo::shell {

struct Card {
  enum class Shape { Square, Wide, Round };  // songs, albums, playlists; videos (16:9); artists
  Shape shape = Shape::Square;
  std::string title, subtitle, thumb;
  const Tile* tile = nullptr;  // a banner's, instead of a picture
  std::function<void()> onSelect, onLongSelect;
};

struct Shelf {
  std::string title;  // "" none
  std::vector<Card> cards;
  bool banner = false;  // one card, the page's width
};

class ShelfView {
 public:
  void setShelves(std::vector<Shelf> shelves, bool keepSelection = false);
  void appendShelves(std::vector<Shelf> more);
  void clear() { setShelves({}); }
  bool empty() const { return order_.empty(); }
  const std::vector<Shelf>& shelves() const { return shelves_; }
  void layout(Env& e, float w, float h);
  bool down(int steps);
  bool up(int steps);  // false at the first card: the tabs above take the wheel
  bool select();
  bool longSelect();
  const Card* selected() const;
  // the chosen card's picture and how it's drawn (the screen's ambient color follows it)
  int artW(const Card& c) const;
  int artH() const { return artH_; }
  // the next page: once the chosen card is in one of the last two shelves
  std::function<void()> onNearEnd;
  bool active = true;  // false while the tabs have the wheel: no card is chosen
  bool step(float dt);  // true while moving
  void draw(Env& e, float x, float y);

 private:
  void place(bool snap);  // the targets (and positions, snap) for the chosen card
  void move(int by);
  std::vector<Shelf> shelves_;
  std::vector<std::pair<int, int>> order_;  // shelf, card: the wheel's order
  std::vector<float> top_, height_;           // each shelf's place on the page
  std::vector<std::vector<float>> x_;         // each card's left in its shelf
  std::vector<Spring> slide_;                 // each shelf's sideways scroll
  std::vector<float> slideTo_;
  Spring scroll_, fx_, fy_, fw_;              // the page; the focus card's left, top and width (on screen)
  float scrollTo_ = 0, fxTo_ = 0, fyTo_ = 0, fwTo_ = 0;
  int at_ = 0;  // in order_
  float w_ = 0, h_ = 0;
  int artH_ = 92, gap_ = 10, pad_ = 10, titleH_ = 24, textH_ = 34, bannerH_ = 46;
  bool placed_ = false;
};

}  // namespace rondo::shell
