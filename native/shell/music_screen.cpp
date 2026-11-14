// YouTube Music (MusicScreen.kt in 1.0.x, MusicHome.dc.html): the logo, the pill tabs and the search
// button, a background tinted by the selected artwork, the mini player. Home is shelves since 3.0.3, as
// the app's home is: each section a titled row of covers (shelves.h), Downloads above them; Library is
// a list. The pills are reached by turning the wheel up past the first card or row; there the wheel
// moves between them and center opens one. Home scrolls forever.
#include <math.h>

#include "../core/api.h"
#include "music_list.h"

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;

class MusicScreen : public TabbedListScreen {
 public:
  MusicScreen() : TabbedListScreen("YouTube Music") {
    tabs_ = wantedTabs();
    focusTabsFromTop();
    list_.onNearEnd = [this] { more(); };
    shelves_.onNearEnd = [this] { more(); };
    load(0);
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    // the selected card's or row's artwork, also while the pills have the focus (RowList.selectedEntry() in 1.0.x)
    bool shelves = showingShelves();
    if (shelves) {
      const Card* c = shelves_.selected();
      ambient_.follow(e, c ? c->thumb : "", c ? shelves_.artW(*c) : 0, c ? shelves_.artH() : 0);
    } else {
      const Row* sel = list_.selectedRow();
      ambient_.follow(e, sel ? sel->thumb : "", e.dp(34));
    }
    ambient_.draw(e, {0, 0, w, h});
    header(e, w, h);
    float top = float(e.dp(31));
    bool mini = miniPlayerShown();
    float bottom = mini ? float(e.dp(kMiniPlayer)) : 0;
    if (shelves) {
      shelves_.layout(e, w, h - top - bottom);
      shelves_.draw(e, 0, top);
    } else {
      list_.layout(e, w, h - top - bottom);
      list_.draw(e, 0, top);
    }
    if (mini) miniPlayer(e, {0, h - bottom, w, bottom});
    e.c.layer(0);
  }
  bool step(float dt) override { return TabbedListScreen::step(dt) | shelves_.step(dt); }
  // Home's shelves take the wheel; up past the first card, the pills do
  bool onAction(Action a, int steps) override {
    if (focused_ < 0 && showingShelves()) {
      switch (a) {
        case Action::Down: return shelves_.down(steps);
        case Action::Up:
          if (!shelves_.up(steps)) focused_ = active_, shelves_.active = false;
          return true;
        case Action::Select: return shelves_.select();
        case Action::LongSelect: return shelves_.longSelect();
        default: return TabbedListScreen::onAction(a, steps);
      }
    }
    bool used = TabbedListScreen::onAction(a, steps);
    if (focused_ < 0) shelves_.active = true;  // a pill chosen: the page has the wheel again
    return used;
  }

 private:
  // Library only when the source has an account (Accounts.kt); signed out it's Home and search
  std::vector<std::string> wantedTabs() const override {
    std::vector<std::string> t{"Home"};
    if (g_prefs.accountMusic) t.push_back("Library");
    return t;
  }
  void openSearch() override { g_app->push(makeMusicSearch()); }
  // Home's Downloads says what's on the card now (a download made since it loaded)
  void onShow() override {
    const std::vector<Shelf>& all = shelves_.shelves();
    if (all.empty() || !all[0].banner || all[0].cards.empty() || all[0].cards[0].tile != &tiles::downloads) return;
    Shelf now = downloadsShelf();
    if (now.cards[0].subtitle == all[0].cards[0].subtitle) return;
    std::vector<Shelf> copy = all;
    copy[0] = now;
    shelves_.setShelves(std::move(copy), true);
  }
  bool showingShelves() const { return active_ == 0 && !shelves_.empty(); }
  static Shelf downloadsShelf() {
    Row r = musicDownloadsRow();
    Shelf s;
    s.banner = true;
    Card c;
    c.title = r.title, c.subtitle = r.subtitle, c.tile = r.tile, c.onSelect = r.onSelect;
    s.cards.push_back(std::move(c));
    return s;
  }
  // the sections as shelves, each card's queue its own section (as musicRows' default)
  std::vector<Shelf> sectionShelves(const std::vector<Section>& sections) {
    std::vector<Shelf> out;
    for (const Section& s : sections) {
      if (s.items.empty()) continue;
      for (const Item& it : s.items) items_.push_back(it);
      auto section = std::make_shared<const std::vector<Item>>(s.items);
      Shelf sh;
      sh.title = s.title;
      sh.cards = musicCards(s.items, [section] { return *section; });
      out.push_back(std::move(sh));
    }
    return out;
  }
  // the header: the logo, "Music", a pill per tab, the search button, the battery
  void header(Env& e, float w, float h) {
    int H = e.dp(31);
    float x = float(e.dp(10));
    e.c.layer(4);
    int logo = e.dp(18);
    e.icons.musicLogo(e.c, {x, float((H - logo) / 2), float(logo), float(logo)});
    x += logo + e.dp(6);
    Label name = label(e, "Music", 15, true);
    drawLabel(e, name, x, float((H - name.h) / 2), white);
    x += name.w + e.dp(10);
    float pillsEnd = drawPills(e, x, H, h);
    // battery only: the pills leave no room for Wi-Fi (the date goes too when they leave none for it)
    statusIconsFit(e, pillsEnd + e.dp(10), w - e.dp(10), H, e.p.darkMuted, false);
  }
  void load(int tab) override {
    active_ = tab;
    requests_.cancelAll();
    items_.clear();
    next_.clear();
    loading_ = false;
    list_.setMessage("Loading\xe2\x80\xa6");
    shelves_.clear();
    shelves_.active = focused_ < 0;
    std::string path = tab == 0 ? "/music/home" : "/music/library";
    std::string name = tabs_[size_t(tab)];
    requests_.get(path, 120000, [this, tab, path, name](int status, const std::string& body) {
      if (tab != active_) return;
      if (status != 200) {
        std::string problem = offlineOr("Couldn't load " + name + ": " + httpProblem(status, path));
        if (tab != 0) return list_.setMessage(problem);
        // Home offline: its Downloads still there, to play what's on the card
        Shelf msg;
        msg.banner = true;
        Card c;
        c.title = problem, c.subtitle = "Center tries again", c.tile = &tiles::rescan;
        c.onSelect = [this] { load(0); };
        msg.cards.push_back(std::move(c));
        shelves_.setShelves({downloadsShelf(), msg});
        return;
      }
      if (tab == 0) {
        SectionsPage page;
        parseSections(body, page);
        std::vector<Shelf> shelves{downloadsShelf()};
        for (Shelf& s : sectionShelves(page.sections)) shelves.push_back(std::move(s));
        shelves_.setShelves(std::move(shelves));
        list_.setMessage("");
        next_ = page.next;
      } else {
        ItemsPage page;  // {playlists: [...]}: the same shape as a page of items
        parseItems(replaceKey(body, "\"playlists\"", "\"items\""), page);
        Row h;
        h.header = true;
        h.title = "Playlists";
        std::vector<Row> rows{h};
        items_ = page.items;
        for (Row& r : musicRows(page.items, [this] { return items_; })) rows.push_back(std::move(r));
        list_.setRows(std::move(rows));
      }
    });
  }
  static std::string replaceKey(std::string s, const std::string& from, const std::string& to) {
    size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
  }
  // Pager: the next page of Home, when the selection is 5 rows from the end; a page that fails is
  // tried again on the next turn; an expired token (410) ends the list
  void more() {
    if (active_ != 0 || next_.empty() || loading_) return;
    // (Home's next page comes as shelves: shelves_.onNearEnd)
    loading_ = true;
    std::string path = "/music/home?next=" + Api::escape(next_);
    requests_.get(path, 0, [this](int status, const std::string& body) {
      loading_ = false;
      if (status == 410) next_.clear();
      if (status != 200) return;
      SectionsPage page;
      parseSections(body, page);
      next_ = page.next;
      shelves_.appendShelves(sectionShelves(page.sections));
    });
  }

  std::vector<Item> items_;        // what is loaded so far: the queue around a chosen song
  ShelfView shelves_;              // Home
  std::string next_;
  bool loading_ = false;
  Requests requests_;
};

}  // namespace

std::unique_ptr<Screen> makeMusic() { return std::unique_ptr<Screen>(new MusicScreen()); }

}  // namespace rondo::shell
