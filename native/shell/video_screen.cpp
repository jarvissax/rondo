// YouTube (VideoScreens.kt's VideoScreen in 1.0.x, VideoHome.dc.html), ported 1:1: pill tabs Home /
// Subscriptions / History and the search button, thumbnail rows on a background tinted by the selected
// thumbnail; every feed scrolls forever. The pills are reached by turning the wheel up past the first row.
#include "../core/api.h"
#include "music_list.h"

namespace rondo::shell {
namespace {


class VideoScreen : public TabbedListScreen {
 public:
  // no mini player here: a song playing behind it doesn't redraw the list
  bool tick(int64_t now) override {
    now_ = now;
    return false;
  }
  VideoScreen() : TabbedListScreen("YouTube") {
    list_.style = RowStyle::Video;
    // Subscriptions and History only when the server has an account (Accounts.kt)
    tabs_ = {"Home"};
    if (g_prefs.accountVideos) tabs_.insert(tabs_.end(), {"Subscriptions", "History"});
    focusTabsFromTop();
    list_.onNearEnd = [this] { more(); };
    load(0);
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    // the selected row's artwork, also while the pills have the focus (RowList.selectedEntry() in 1.0.x)
    const Row* sel = list_.selectedRow();
    // the color of the row's own 104 x 58 picture (RowList.rowArt in 1.0.x): no second thumbnail
    ambient_.follow(e, sel ? sel->thumb : "", e.dp(104), e.dp(58));
    ambient_.draw(e, {0, 0, w, h});
    header(e, w, h);
    float top = float(e.dp(31));
    list_.layout(e, w, h - top);
    list_.draw(e, 0, top);
    e.c.layer(0);
  }

 private:
  void openSearch() override { g_app->push(makeVideoSearch()); }
  void header(Env& e, float w, float h) {
    int H = e.dp(31);
    float x = float(e.dp(10));
    e.c.layer(4);
    int lw = e.dp(22), lh = e.dp(15);
    e.icons.videoLogo(e.c, {x, float((H - lh) / 2), float(lw), float(lh)});
    x += lw + e.dp(9);
    float pillsEnd = drawPills(e, x, H, h);
    // battery only; with three pills the percentage goes too when there is no room (StatusIcons.onLayout)
    float right = w - e.dp(10), searchEnd = pillsEnd + e.dp(10);
    statusIcons(e, right, H, e.p.darkMuted, false, right - statusIconsWidth(e, false) < searchEnd);
  }
  void load(int tab) override {
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

  std::string next_;
  bool loading_ = false;
  Requests requests_;
};

}  // namespace

std::unique_ptr<Screen> makeVideo() { return std::unique_ptr<Screen>(new VideoScreen()); }


}  // namespace rondo::shell
