// YouTube (VideoScreens.kt's VideoScreen in 1.0.x, VideoHome.dc.html), ported 1:1: pill tabs Home /
// Subscriptions / History and the search button, thumbnail rows on a background tinted by the selected
// thumbnail; every feed scrolls forever. The pills are reached by turning the wheel up past the first row.
// Home starts with Shorts and, since 2.0, Downloads (there even with no connection).
#include "../core/api.h"
#include "downloads.h"
#include "music_list.h"

namespace rondo::shell {
namespace {

// YouTube › Downloads: those on their way (center: cancel), then the videos on the card (center plays,
// hold center deletes); Library › Videos has the same files among the others
class DownloadsScreen : public MusicListScreen {
 public:
  DownloadsScreen() : MusicListScreen("Downloads") {
    list_.style = RowStyle::Video;
    render();
  }
  bool tick(int64_t now) override {
    now_ = now;
    if (key() == key_) return false;
    render();
    return true;
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    const Row* sel = list_.selectedRow();
    ambient_.follow(e, sel ? sel->thumb : "", e.dp(104), e.dp(58));
    ambient_.draw(e, {0, 0, w, h});
    darkHeader(e, w, "Downloads", HeaderLogo::Video);
    float top = float(e.dp(31));
    list_.layout(e, w, h - top);
    list_.draw(e, 0, top);
    e.c.layer(0);
  }

 private:
  static std::string key() {
    std::string k;
    for (const DownloadJob& j : downloadJobs())
      if (!j.song) k += j.id + char('0' + j.state) + std::to_string(j.percent) + j.stage + "|";
    for (const Item& it : downloadedVideos()) k += it.id + "|";
    return k;
  }
  void render() {
    key_ = key();
    std::vector<Row> rows;
    for (const DownloadJob& j : downloadJobs()) {
      if (j.song || j.state == DownloadJob::Done) continue;  // a song's are YouTube Music's
      Row r;
      r.title = j.title;
      std::string q = std::to_string(j.quality) + "p";
      // the percent only while a part comes in (Getting subtitles and Saving kept the last part's 100 %)
      bool part = j.stage.empty() || j.stage.compare(0, 11, "Downloading") == 0;
      std::string pct = part ? std::to_string(j.percent) + "% \xc2\xb7 " : "";
      r.subtitle = j.state == DownloadJob::Running ? (j.stage.empty() ? "Downloading" : j.stage) + " \xc2\xb7 " + pct + q
                   : j.state == DownloadJob::Waiting ? "Waiting \xc2\xb7 " + q
                                                     : "Didn't download: " + j.error;
      r.thumb = j.thumb;
      std::string id = j.id;
      bool failed = j.state == DownloadJob::Failed;
      Item item;
      item.kind = "video", item.id = j.id, item.title = j.title, item.subtitle = j.channel, item.thumb = j.thumb;
      int quality = j.quality;
      r.onSelect = [id, failed, item, quality] {
        if (failed) {
          g_app->push(makeChoice("Download", {{"Try again", "again"}, {"Leave it", "leave"}}, [] { return std::string("again"); },
                                 [item, quality](const std::string& v) {
                                   if (v == "again") downloadVideo(item, quality);
                                 }));
        } else {
          g_app->push(makeChoice("Download", {{"Keep downloading", "keep"}, {"Cancel it", "cancel"}}, [] { return std::string("keep"); },
                                 [id](const std::string& v) {
                                   if (v == "cancel") cancelDownload(id);
                                 }));
        }
      };
      rows.push_back(std::move(r));
    }
    for (const Item& it : downloadedVideos()) {
      Row r;
      r.title = it.title, r.subtitle = it.subtitle, r.thumb = it.thumb;
      Item item = it;
      r.onSelect = [item] { openVideo(item); };
      std::string file = it.id;
      r.onLongSelect = [file] {
        g_app->push(makeChoice("Delete download", {{"Keep it", "keep"}, {"Delete it", "delete"}}, [] { return std::string("keep"); },
                               [file](const std::string& v) {
                                 if (v == "delete" && deleteDownload(file)) g_app->toast("Deleted from the card");
                               },
                               [](const std::string& v) { return std::string(v == "delete" ? "and its subtitles" : ""); }));
      };
      rows.push_back(std::move(r));
    }
    if (rows.empty()) list_.setMessage("Nothing downloaded yet: hold center on a video to save it");
    else list_.setRows(std::move(rows), true);
  }
  std::string key_;
};

// Home's second row
Row downloadsRow() {
  Row d;
  d.title = "Downloads";
  int on = int(downloadedVideos().size()), going = 0;
  for (const DownloadJob& j : downloadJobs()) going += !j.song && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running);
  d.subtitle = going ? std::to_string(going) + " downloading" + (on ? " \xc2\xb7 " + std::to_string(on) + " on the card" : "")
               : on  ? std::to_string(on) + (on == 1 ? " video" : " videos") + " on the card, to watch offline"
                     : "Videos saved to watch offline";
  d.tile = &tiles::downloads;
  d.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new DownloadsScreen())); };
  return d;
}


class VideoScreen : public TabbedListScreen {
 public:
  // no mini player here: a song playing behind it doesn't redraw the list
  bool tick(int64_t now) override {
    now_ = now;
    return syncTabs();
  }
  // Home's Downloads row says what's on the card now (a download made since it loaded)
  void onShow() override {
    const std::vector<Row>& rows = list_.rows();
    for (size_t i = 0; i < rows.size() && i < 2; i++) {
      if (rows[i].title != "Downloads" || rows[i].tile != &tiles::downloads) continue;
      Row now = downloadsRow();
      if (now.subtitle == rows[i].subtitle) return;
      std::vector<Row> copy = rows;
      copy[i] = now;
      list_.setRows(std::move(copy), true);
      return;
    }
  }
  VideoScreen() : TabbedListScreen("YouTube") {
    list_.style = RowStyle::Video;
    tabs_ = wantedTabs();
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
  // Subscriptions and History only when the source has an account (Accounts.kt)
  std::vector<std::string> wantedTabs() const override {
    std::vector<std::string> t{"Home"};
    if (g_prefs.accountVideos) t.insert(t.end(), {"Subscriptions", "History"});
    return t;
  }
  void openSearch() override { g_app->push(makeVideoSearch()); }
  void header(Env& e, float w, float h) {
    int H = e.dp(31);
    float x = float(e.dp(10));
    e.c.layer(4);
    int lw = e.dp(22), lh = e.dp(15);
    e.icons.videoLogo(e.c, {x, float((H - lh) / 2), float(lw), float(lh)});
    x += lw + e.dp(9);
    float pillsEnd = drawPills(e, x, H, h);
    // the battery alone: a clock beside three pills ran into the search button
    (void)pillsEnd;
    batteryIcon(e, w - e.dp(10), H, e.p.darkMuted);
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
      if (status != 200) {
        std::string problem = offlineOr("Couldn't load " + name + ": " + httpProblem(status, path));
        if (tab != 0) return list_.setMessage(problem);
        // Home: the downloads are there without a connection
        Row msg;
        msg.title = problem;
        msg.subtitle = "Center tries again";
        msg.onSelect = [this] { load(0); };
        list_.setRows({downloadsRow(), msg});
        return;
      }
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
        rows.push_back(downloadsRow());
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
