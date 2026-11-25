// YouTube (VideoScreens.kt's VideoScreen in 1.0.x, VideoHome.dc.html), ported 1:1: pill tabs Home /
// Subscriptions / History and the search button, thumbnail rows on a background tinted by the selected
// thumbnail; every feed scrolls forever. The pills are reached by turning the wheel up past the first row.
// Home starts with Shorts and, since 2.0, Downloads (there even with no connection).
#include "../core/api.h"
#include "downloads.h"
#include "music_list.h"
#include "video_places.h"

namespace rondo::shell {
namespace {

// YouTube › Downloads: Pause downloads (or Resume), those on their way (center: pause, resume, cancel),
// then the videos on the card (center plays, hold center deletes); Library › Videos has the same files
// among the others
class DownloadsScreen : public MusicListScreen {
 public:
  DownloadsScreen() : MusicListScreen(tr("downloads.title")) {
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
    darkHeader(e, w, tr("downloads.title"), HeaderLogo::Video);
    float top = float(e.dp(31));
    list_.layout(e, w, h - top);
    list_.draw(e, 0, top);
    e.c.layer(0);
  }

 private:
  static std::string key() {
    std::string k = downloadsHeld() + (downloadsGoing() ? "g" : "") + (downloadsPaused() ? "p" : "") + "|";
    for (const DownloadJob& j : downloadJobs())
      if (!j.song) k += j.id + char('0' + j.state) + std::to_string(j.percent) + j.stage + "|";
    for (const Item& it : downloadedVideos()) k += it.id + "|";
    return k;
  }
  void render() {
    key_ = key();
    std::vector<Row> rows;
    bool any = false;
    for (const DownloadJob& j : downloadJobs()) any = any || (!j.song && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running || j.state == DownloadJob::Paused));
    if (any) rows.push_back(downloadsPauseRow());  // all of them: videos and songs are one queue
    for (const DownloadJob& j : downloadJobs()) {
      if (j.song || j.state == DownloadJob::Done) continue;  // a song's are YouTube Music's
      Row r;
      r.title = j.title;
      r.subtitle = downloadRowState(j, std::to_string(j.quality) + "p");
      r.thumb = j.thumb;
      std::string id = j.id;
      r.onSelect = [id] {
        for (const DownloadJob& d : downloadJobs())
          if (d.id == id) return openDownloadMenu(d);
      };
      rows.push_back(std::move(r));
    }
    for (const Item& it : downloadedVideos()) {
      Row r;
      r.title = it.title, r.subtitle = it.subtitle, r.thumb = it.thumb;
      r.place = videoPlaceKey(it), r.placeLen = it.durationSec;
      Item item = it;
      r.onSelect = [item] { openVideo(item); };
      std::string file = it.id;
      r.onLongSelect = [file] {
        g_app->push(makeChoice(tr("downloads.deleteTitle"), {{tr("downloads.keepIt"), "keep"}, {tr("downloads.deleteIt"), "delete"}}, [] { return std::string("keep"); },
                               [file](const std::string& v) {
                                 if (v == "delete" && deleteDownload(file)) g_app->toast(tr("downloads.deleted"));
                               },
                               [](const std::string& v) { return v == "delete" ? tr("downloads.andSubtitles") : std::string(); }));
      };
      rows.push_back(std::move(r));
    }
    if (rows.empty()) list_.setMessage(tr("downloads.videosEmpty"));
    else list_.setRows(std::move(rows), true);
  }
  std::string key_;
};

// YouTube › Continue watching: the YouTube videos started on the Y1 (video_places.h), the last first; a
// downloaded one plays from the card
class ContinueWatchingScreen : public MusicListScreen {
 public:
  ContinueWatchingScreen() : MusicListScreen(tr("video.continueWatching")) {
    list_.style = RowStyle::Video;
    render();
  }
  void onShow() override { render(); }  // one watched to the end leaves, one watched further moves up
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    const Row* sel = list_.selectedRow();
    ambient_.follow(e, sel ? sel->thumb : "", e.dp(104), e.dp(58));
    ambient_.draw(e, {0, 0, w, h});
    darkHeader(e, w, title_, HeaderLogo::Video);
    float top = float(e.dp(31));
    list_.layout(e, w, h - top);
    list_.draw(e, 0, top);
    e.c.layer(0);
  }

 private:
  void render() {
    std::vector<Item> v = continueWatchingYouTube();
    if (v.empty()) return list_.setMessage(tr("video.continueNone"));
    list_.setRows(videoRows(v), true);
  }
};

// Home's third row, once a YouTube video has a place: Continue watching (none: no row)
bool continueRow(Row& r) {
  std::vector<Item> v = continueWatchingYouTube();
  if (v.empty()) return false;
  r = Row();
  r.title = tr("video.continueWatching");
  r.subtitle = tr("video.continueCount", {{"count", int(v.size())}});
  r.tile = &tiles::continueWatching;
  r.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new ContinueWatchingScreen())); };
  return true;
}

// Home's second row
Row downloadsRow() {
  Row d;
  d.title = tr("downloads.title");
  int on = int(downloadedVideos().size()), going = 0, paused = 0;
  for (const DownloadJob& j : downloadJobs()) {
    going += !j.song && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running);
    paused += !j.song && j.state == DownloadJob::Paused;
  }
  std::string cardPart = on ? " \xc2\xb7 " + tr("downloads.onCard", {{"count", on}}) : "";
  std::string held = downloadsHeld();  // why they wait: Wi-Fi, the card
  d.subtitle = going    ? (held.empty() ? tr("downloads.downloadingCount", {{"count", going}}) : held) + cardPart
               : paused ? tr("downloads.pausedCount", {{"count", paused}}) + cardPart
               : on     ? tr("downloads.videosOnCard", {{"count", on}})
                        : tr("downloads.videosRowEmpty");
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
  // Home's Downloads row says what's on the card now (a download made since it loaded), and its
  // Continue watching row what was watched since (there once a video has a place, gone when none has)
  void onShow() override {
    const std::vector<Row>& rows = list_.rows();
    for (size_t i = 0; i < rows.size() && i < 2; i++) {
      if (rows[i].title != tr("downloads.title") || rows[i].tile != &tiles::downloads) continue;
      std::vector<Row> copy = rows;
      copy[i] = downloadsRow();
      Row cw;
      bool want = continueRow(cw);
      bool has = i + 1 < copy.size() && copy[i + 1].tile == &tiles::continueWatching;
      if (want && has) copy[i + 1] = cw;
      else if (want) copy.insert(copy.begin() + long(i) + 1, cw);
      else if (has) copy.erase(copy.begin() + long(i) + 1);
      bool same = copy.size() == rows.size();
      for (size_t k = 0; same && k < copy.size(); k++) same = copy[k].title == rows[k].title && copy[k].subtitle == rows[k].subtitle;
      if (!same) list_.setRows(std::move(copy), true);
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
    std::vector<std::string> t{tr("video.tabHome")};
    if (g_prefs.accountVideos) t.insert(t.end(), {tr("video.tabSubscriptions"), tr("video.tabHistory")});
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
    list_.setMessage(tr("common.loading"));
    std::string path = feeds[tab], name = tabs_[size_t(tab)];
    requests_.get(path, 120000, [this, tab, path, name](int status, const std::string& body) {
      if (tab != active_) return;
      if (status != 200) {
        std::string problem = offlineOr(tr("video.couldntLoad", {{"name", name}, {"problem", httpProblem(status, path)}}));
        if (tab != 0) return list_.setMessage(problem);
        // Home: the downloads are there without a connection
        Row msg;
        msg.title = problem;
        msg.subtitle = tr("video.centerRetries");
        msg.onSelect = [this] { load(0); };
        std::vector<Row> offline{downloadsRow()};
        Row cw;
        if (continueRow(cw)) offline.push_back(cw);
        offline.push_back(msg);
        list_.setRows(std::move(offline));
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
        s.subtitle = tr("video.shortsSubtitle");
        s.tile = &tiles::shorts;
        s.onSelect = [] { g_app->push(makeShorts()); };
        rows.push_back(s);
        // the Shorts list now (kept 2 minutes): the server starts resolving its first shorts, so they
        // play at once when Shorts is opened
        requests_.get("/video/shorts", 120000, [](int, const std::string&) {});
        rows.push_back(downloadsRow());
        Row cw;
        if (continueRow(cw)) rows.push_back(cw);
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
