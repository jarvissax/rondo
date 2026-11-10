// YouTube Music (MusicScreen.kt in 1.0.x, MusicHome.dc.html), ported 1:1: the logo, the pill tabs and
// the search button, a background tinted by the selected artwork, the mini player. The pills are reached
// by turning the wheel up past the first row; there the wheel moves between them and center opens one.
// Home scrolls forever.
#include <math.h>

#include "../core/api.h"
#include "music_list.h"

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;

class MusicScreen : public TabbedListScreen {
 public:
  MusicScreen() : TabbedListScreen("YouTube Music") {
    // Library only when the server has an account (Accounts.kt); signed out it's Home and search
    tabs_ = {"Home"};
    if (g_prefs.accountMusic) tabs_.push_back("Library");
    focusTabsFromTop();
    list_.onNearEnd = [this] { more(); };
    load(0);
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    // the selected row's artwork, also while the pills have the focus (RowList.selectedEntry() in 1.0.x)
    const Row* sel = list_.selectedRow();
    ambient_.follow(e, sel ? sel->thumb : "", e.dp(34));
    ambient_.draw(e, {0, 0, w, h});
    header(e, w, h);
    float top = float(e.dp(31));
    bool mini = miniPlayerShown();
    float bottom = mini ? float(e.dp(kMiniPlayer)) : 0;
    list_.layout(e, w, h - top - bottom);
    list_.draw(e, 0, top);
    if (mini) miniPlayer(e, {0, h - bottom, w, bottom});
    e.c.layer(0);
  }

 private:
  void openSearch() override { g_app->push(makeMusicSearch()); }
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
  std::vector<Row> sectionRows(const std::vector<Section>& sections) {
    std::vector<Row> rows;
    for (const Section& s : sections) {
      Row h;
      h.header = true;
      h.title = s.title;
      rows.push_back(h);
      for (const Item& it : s.items) items_.push_back(it);
      // a song plays with its own section as the queue (1.0.x: musicRows' default)
      auto section = std::make_shared<const std::vector<Item>>(s.items);  // one copy for all its rows
      for (Row& r : musicRows(s.items, [section] { return *section; })) rows.push_back(std::move(r));
    }
    return rows;
  }
  void load(int tab) override {
    active_ = tab;
    requests_.cancelAll();
    items_.clear();
    next_.clear();
    loading_ = false;
    list_.setMessage("Loading\xe2\x80\xa6");
    std::string path = tab == 0 ? "/music/home" : "/music/library";
    std::string name = tabs_[size_t(tab)];
    requests_.get(path, 120000, [this, tab, path, name](int status, const std::string& body) {
      if (tab != active_) return;
      if (status != 200) return list_.setMessage(offlineOr("Couldn't load " + name + ": " + httpProblem(status, path)));
      if (tab == 0) {
        SectionsPage page;
        parseSections(body, page);
        list_.setRows(sectionRows(page.sections));
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
    loading_ = true;
    std::string path = "/music/home?next=" + Api::escape(next_);
    requests_.get(path, 0, [this](int status, const std::string& body) {
      loading_ = false;
      if (status == 410) next_.clear();
      if (status != 200) return;
      SectionsPage page;
      parseSections(body, page);
      next_ = page.next;
      list_.appendRows(sectionRows(page.sections));
    });
  }

  std::vector<Item> items_;        // what is loaded so far: the queue around a chosen song
  std::string next_;
  bool loading_ = false;
  Requests requests_;
};

}  // namespace

std::unique_ptr<Screen> makeMusic() { return std::unique_ptr<Screen>(new MusicScreen()); }

}  // namespace rondo::shell
