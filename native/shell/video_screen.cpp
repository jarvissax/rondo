// YouTube (VideoScreens.kt's VideoScreen in 1.0.x, VideoHome.dc.html), ported 1:1: pill tabs Home /
// Subscriptions / History and the search button, thumbnail rows on a background tinted by the selected
// thumbnail; every feed scrolls forever. The pills are reached by turning the wheel up past the first row.
#include "../core/api.h"
#include "music_list.h"

namespace rondo::shell {
namespace {

constexpr Color white = 0xffffffff;

class VideoScreen : public MusicListScreen {
 public:
  VideoScreen() : MusicListScreen("YouTube") {
    list_.style = RowStyle::Video;
    // Subscriptions and History only when the server has an account (Accounts.kt)
    tabs_ = {"Home"};
    if (g_prefs.accountVideos) tabs_.insert(tabs_.end(), {"Subscriptions", "History"});
    list_.onTopReached = [this] {
      focused_ = active_;
      list_.active = false;
      return true;
    };
    list_.onNearEnd = [this] { more(); };
    load(0);
  }
  bool onAction(Action a, int steps) override {
    if (focused_ < 0) {
      if (list_.rows().empty() && a == Action::Select) {
        load(active_);
        return true;
      }
      return MusicListScreen::onAction(a, steps);
    }
    int search = int(tabs_.size());
    switch (a) {
      case Action::Up: focused_ = (focused_ + search) % (search + 1); return true;
      case Action::Down: focused_ = (focused_ + 1) % (search + 1); return true;
      case Action::Select: {
        int f = focused_;
        focused_ = -1;
        list_.active = true;
        if (f == search) g_app->push(makeVideoSearch());
        else load(f);
        return true;
      }
      default: return false;
    }
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    // the selected row's artwork, also while the pills have the focus (RowList.selectedEntry() in 1.0.x)
    const Row* sel = list_.selectedRow();
    ambient_.follow(e, sel ? sel->thumb : "", e.dp(104));
    ambient_.draw(e, {0, 0, w, h});
    header(e, w);
    float top = float(e.dp(31));
    list_.layout(e, w, h - top);
    list_.draw(e, 0, top);
    e.c.layer(0);
  }

 private:
  void header(Env& e, float w) {
    int H = e.dp(31);
    float x = float(e.dp(10));
    e.c.layer(4);
    int lw = e.dp(22), lh = e.dp(15);
    e.icons.videoLogo(e.c, {x, float((H - lh) / 2), float(lw), float(lh)});
    x += lw + e.dp(9);
    int ph = e.dp(21), py = (H - ph) / 2;
    for (int i = 0; i < int(tabs_.size()); i++) {
      bool active = i == active_, ring = i == focused_;
      Label l = label(e, tabs_[size_t(i)], 12.5f, active);
      int pw = l.w + 2 * e.dp(10);
      pill(e, {x, float(py), float(pw), float(ph)}, active ? white : 0x3cffffff, ring);
      e.c.layer(4);
      drawLabel(e, l, centerText(x, float(pw), l.w), float(py + ((ph - l.h) >> 1)), active ? e.p.dark : white);
      x += pw + e.dp(5);
    }
    pill(e, {x, float(py), float(ph), float(ph)}, 0x3cffffff, focused_ == int(tabs_.size()));
    e.c.layer(4);
    int s = e.dp(11);
    e.icons.draw(e.c, Icon::Search, {x + float((ph - s) / 2), float(py + (ph - s) / 2), float(s), float(s)}, white);
    // battery only; with three pills the percentage goes too when there is no room (StatusIcons.onLayout)
    float right = w - e.dp(10), searchEnd = x + ph + e.dp(10);
    statusIcons(e, right, H, e.p.darkMuted, false, right - statusIconsWidth(e, false) < searchEnd);
  }
  void pill(Env& e, Rect r, Color fill, bool ring) {
    float radius = std::min(e.dpf(12), r.h / 2);
    e.c.layer(2);
    if (ring) {
      int t = e.dp(2);
      e.c.shape(r, radius, e.p.red);
      e.c.shape({r.x + t, r.y + t, r.w - 2 * t, r.h - 2 * t}, std::max(0.f, radius - t), (fill >> 24) == 0xff ? fill : 0xff474747);
    } else {
      e.c.shape(r, radius, fill);
    }
  }
  void load(int tab) {
    static const char* feeds[] = {"/video/home", "/video/subscriptions", "/video/history"};
    active_ = tab;
    requests_.cancelAll();
    next_.clear();
    loading_ = false;
    list_.setMessage("Loading\xe2\x80\xa6");
    std::string path = feeds[tab], name = tabs_[size_t(tab)];
    requests_.get(path, 120000, [this, tab, path, name](int status, const std::string& body) {
      if (tab != active_) return;
      if (status != 200) return list_.setMessage(offlineOr("Couldn't load " + name + ": " + httpProblem(status, path)));
      ItemsPage page;
      parseItems(body, page);
      next_ = page.next;
      std::vector<Row> rows;
      if (tab == 0) {
        // Home starts with Shorts, like the app's Shorts shelf (the header has no room for a tab)
        Row s;
        s.title = "Shorts";
        s.subtitle = "Short videos, one after another";
        s.tile = &tiles::shorts;
        s.onSelect = [] { g_app->push(makeShorts()); };
        rows.push_back(s);
        // the Shorts list now (kept 2 minutes): the server starts resolving its first shorts, so they
        // play at once when Shorts is opened
        requests_.get("/video/shorts", 120000, [](int, const std::string&) {});
      }
      for (Row& r : videoRows(page.items)) rows.push_back(std::move(r));
      list_.setRows(std::move(rows));
    });
  }
  void more() {
    static const char* feeds[] = {"/video/home", "/video/subscriptions", "/video/history"};
    if (next_.empty() || loading_) return;
    loading_ = true;
    int tab = active_;
    requests_.get(std::string(feeds[tab]) + "?next=" + Api::escape(next_), 0, [this, tab](int status, const std::string& body) {
      loading_ = false;
      if (tab != active_) return;
      if (status == 410) next_.clear();
      if (status != 200) return;
      ItemsPage page;
      parseItems(body, page);
      next_ = page.next;
      list_.appendRows(videoRows(page.items));
    });
  }

  std::vector<std::string> tabs_;
  int active_ = 0, focused_ = -1;
  std::string next_;
  bool loading_ = false;
  Requests requests_;
};

}  // namespace

std::unique_ptr<Screen> makeVideo() { return std::unique_ptr<Screen>(new VideoScreen()); }


}  // namespace rondo::shell
