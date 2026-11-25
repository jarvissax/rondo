// The main menu's Podcasts (rss.h), in the iPod's light look: Continue listening, New episodes, Your
// shows, Save for later, Top shows, Downloaded and Search; a show's page (its cover and what it is,
// Subscribe, its speed, Download new episodes, its episodes); an episode's options; its show notes;
// Podcast settings. An episode's row says where it is (its dot, "35 min left", Played) whenever
// it's drawn, so a place moving shows at once. Its settings (the country, a look for new episodes) are its
// own last row.
#include <stdio.h>
#include <time.h>

#include <algorithm>

#include "app.h"
#include "books.h"
#include "downloads.h"
#include "rss.h"
#include "screens.h"

namespace rondo::shell {
namespace {

void playNow(const RssEpisode& e) {
  playRss(e);
  if (!e.video()) openNowPlaying();
}

// what a row on its way says (downloads.cpp's words), or "" when it isn't
const DownloadJob* jobOf(const std::string& id) {
  for (const DownloadJob& j : downloadJobs())
    if (j.rss && j.id == id && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running || j.state == DownloadJob::Paused)) return &j;
  return nullptr;
}

void fillRow(const RssEpisode& e, bool withShow, Row& r) {
  r.subtitle = rssSubtitle(e, withShow);
  r.dot = rssNew(e);
  const BookPlay* b = bookNow();
  if (b && b->key == rssKey(e.id)) r.trailing = tr("songList.playing");
  else if (const DownloadJob* j = jobOf(e.id)) r.trailing = j->state == DownloadJob::Paused ? tr("downloads.state.paused") : std::to_string(j->percent) + "%";
  else r.trailing = downloadedRss(e.id).empty() ? "" : tr("podcasts.onCardMark");
}

Row episodeRow(const RssEpisode& e, bool withShow, bool picture) {
  Row r;
  r.title = e.title;
  if (picture) {
    const RssShow* s = rssShow(e.show);
    r.thumb = rssArt(s && !s->art.empty() ? s->art : e.image);
    if (r.thumb.empty()) r.tile = &tiles::podcasts;
  }
  RssEpisode copy = e;
  r.onSelect = [copy] { playNow(copy); };
  r.onLongSelect = [copy, withShow] { openRssEpisodeMenu(copy, !withShow); };
  fillRow(e, withShow, r);
  return r;
}

Row note(const std::string& title, const std::string& subtitle, const Tile* tile = nullptr) {
  Row r;
  r.title = title, r.subtitle = subtitle, r.tile = tile;
  r.enabled = false;
  return r;
}

Row header(const std::string& title) {
  Row h;
  h.header = true;
  h.title = title;
  return h;
}

// what a show's row says: "3 new · Nov 22", "Updated Nov 22", its author
std::string showSubtitle(const RssShow& s, bool subscribed) {
  std::string updated;
  if (s.updated > 0) {
    time_t t = time_t(s.updated), now = time(nullptr);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    updated = a.tm_year == b.tm_year ? clockDate(t, false) : tr("rss.date.withYear", {{"date", clockDate(t, false)}, {"year", std::to_string(a.tm_year + 1900)}});
  }
  int fresh = subscribed ? rssNewCount(s.id) : 0;
  std::string out;
  if (fresh) out = tr("rss.newCount", {{"count", fresh}});
  else if (!s.author.empty() && !subscribed) out = s.author;
  if (!updated.empty()) out += (out.empty() ? "" : " \xc2\xb7 ") + (fresh || !subscribed ? updated : tr("rss.updated", {{"date", updated}}));
  if (!subscribed && s.count > 0) out += (out.empty() ? "" : " \xc2\xb7 ") + tr("podcasts.episodeCount", {{"count", s.count}});
  return out;
}

Row showRow(const RssShow& s) {
  Row r;
  r.title = s.title.empty() ? s.feed : s.title;
  r.thumb = rssArt(s.art);
  if (r.thumb.empty()) r.tile = &tiles::podcasts;
  r.subtitle = showSubtitle(s, rssSubscribed(s.id));
  r.chevron = true;
  RssShow copy = s;
  r.onSelect = [copy] { openRssShow(copy); };
  return r;
}

// a list of episodes from several shows (each row with its show's cover): New episodes, Save for later,
// Downloaded (and the ones on their way), Up next
class EpisodesScreen : public ShellListScreen {
 public:
  enum class Kind { New, Later, Downloaded, UpNext };
  EpisodesScreen(std::string title, Kind kind) : ShellListScreen(std::move(title)), kind_(kind) {
    list_.fill = [this](int i, Row& r) {
      int k = i - first_;
      if (k >= 0 && k < int(eps_.size())) fillRow(eps_[size_t(k)], true, r);
    };
    render();
  }
  void onShow() override { render(); }
  bool tick(int64_t) override {
    if (serial_ == rssSerial() && changes_ == g_library.bookChanges() && downloads_ == downloadsSerial() && checking_ == rssChecking()) return false;
    render();
    return true;
  }

 private:
  void render() {
    serial_ = rssSerial(), changes_ = g_library.bookChanges(), downloads_ = downloadsSerial(), checking_ = rssChecking();
    std::vector<Row> head;
    if (kind_ == Kind::New) eps_ = rssNewEpisodes();
    else if (kind_ == Kind::Later) eps_ = rssLater();
    else if (kind_ == Kind::UpNext) eps_ = rssUpNext();
    else {
      eps_ = rssDownloaded();
      // the ones on their way first
      for (const DownloadJob& j : downloadJobs()) {
        if (!j.rss || j.state == DownloadJob::Done) continue;
        Row r;
        r.title = j.title;
        r.thumb = j.thumb;
        r.subtitle = downloadRowState(j, "");
        std::string id = j.id;
        r.onSelect = [id] {
          for (const DownloadJob& d : downloadJobs())
            if (d.id == id) return openDownloadMenu(d);
        };
        head.push_back(r);
      }
      if (!head.empty() && downloadsGoing() + downloadsPaused()) head.insert(head.begin(), downloadsPauseRow());
    }
    if (head.empty() && eps_.empty()) {
      const char* empty = kind_ == Kind::New ? (checking_ ? "rss.checking" : rssShows().empty() ? "rss.newNoShows" : "rss.newNone")
                          : kind_ == Kind::Later ? "podcasts.laterNone" : kind_ == Kind::UpNext ? "podcasts.upNextNone" : "podcasts.downloadedNone";
      return list_.setMessage(tr(empty));
    }
    first_ = int(head.size());
    std::vector<Row> rows = std::move(head);
    for (const RssEpisode& e : eps_) {
      Row r = episodeRow(e, true, true);
      if (kind_ == Kind::UpNext) {  // center plays it now; held: off the list
        RssEpisode copy = e;
        r.onSelect = [copy] {
          rssRemoveFromUpNext(copy.id);
          playNow(copy);
        };
        r.onLongSelect = [copy] {
          g_app->push(makeChoice(copy.title, {{tr("podcasts.keepInUpNext"), "keep"}, {tr("podcasts.takeOffUpNext"), "remove"}}, [] { return std::string("keep"); },
                                 [copy](const std::string& v) {
                                   if (v == "remove") rssRemoveFromUpNext(copy.id);
                                 }));
        };
      }
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows), true);
  }
  Kind kind_;
  std::vector<RssEpisode> eps_;
  int first_ = 0;
  uint32_t serial_ = 0, downloads_ = 0;
  uint64_t changes_ = 0;
  bool checking_ = false;
};

// Your shows: the latest updated first
class ShowsScreen : public ShellListScreen {
 public:
  ShowsScreen() : ShellListScreen(tr("podcasts.yourShows")) { render(); }
  void onShow() override { render(); }
  bool tick(int64_t) override {
    if (serial_ == rssSerial()) return false;
    render();
    return true;
  }

 private:
  void render() {
    serial_ = rssSerial();
    std::vector<RssShow> shows = rssShows();
    if (shows.empty()) return list_.setMessage(tr("rss.noShows") + "\n" + tr("rss.noShowsHint"));
    std::vector<Row> rows;
    for (const RssShow& s : shows) rows.push_back(showRow(s));
    list_.setRows(std::move(rows), true);
  }
  uint32_t serial_ = 0;
};

// A list of shows from the directory: a search's answer, Apple's charts
class FoundShowsScreen : public ShellListScreen {
 public:
  FoundShowsScreen(std::string title, std::string path, bool ranked, bool genres)
      : ShellListScreen(std::move(title)), path_(std::move(path)), ranked_(ranked), genres_(genres) {
    load();
  }
  bool waitsForContent() const override { return true; }
  void onShow() override {
    if (loaded_) render();
  }

 private:
  void load() {
    list_.setMessage(tr("common.loading"));
    std::string p = path_ + (genre_ ? "&genre=" + std::to_string(genre_) : "");
    requests_.get(p, 600000, [this](int status, const std::string& body) {
      if (status == 0) return list_.setMessage(tr("rss.offline"));
      if (status == 429) return list_.setMessage(tr("rss.busy"));
      if (status != 200) return list_.setMessage(tr("rss.cantLoad"));
      shows_ = parseRssShows(body);
      loaded_ = true;
      render();
    });
  }
  void render() {
    std::vector<Row> rows;
    if (genres_) {  // Top shows' category
      Row g;
      std::string name;
      for (int i = 0; i < kRssGenreCount; i++)
        if (kRssGenres[i].id == genre_) name = tr(kRssGenres[i].key);
      g.title = tr("rss.category");
      g.trailing = name;
      g.tile = &tiles::folders;
      g.chevron = true;
      g.onSelect = [this] {
        std::vector<std::pair<std::string, std::string>> c;
        for (int i = 0; i < kRssGenreCount; i++) c.push_back({tr(kRssGenres[i].key), std::to_string(kRssGenres[i].id)});
        g_app->push(makeChoice(tr("rss.category"), c, [this] { return std::to_string(genre_); }, [this](const std::string& v) {
          int g = atoi(v.c_str());
          if (g == genre_) return;
          genre_ = g;
          load();
        }));
      };
      rows.push_back(g);
      rows.push_back(header(tr("rss.topIn", {{"country", rssCountryName(rssCountry())}})));
    }
    if (shows_.empty()) rows.push_back(note(tr("rss.nothingFound"), ""));
    int n = 0;
    for (const RssShow& s : shows_) {
      Row r = showRow(s);
      if (ranked_) r.trailing = std::to_string(++n);
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows), true);
  }
  std::string path_;
  bool ranked_, genres_;
  int genre_ = 0;
  bool loaded_ = false;
  std::vector<RssShow> shows_;
  Requests requests_;
};

// Search: a show by its name, typed on the wheel's keyboard (Apple's directory, and Podcast Index's
// with a key); the results under it
class SearchScreen : public Screen {
 public:
  SearchScreen() : kb_(tr("keyboard.search"), Keyboard::Style::Shell) {}
  std::string title() const override { return tr("podcasts.search"); }
  bool onAction(Action a, int steps) override {
    if (showing_) {
      if (a == Action::Back) {
        showing_ = false;
        return true;
      }
      switch (a) {
        case Action::Down: return results_.down(steps);
        case Action::Up: return results_.up(steps);
        case Action::Select: return results_.select();
        case Action::LongSelect: return results_.longSelect();
        default: return false;
      }
    }
    Keyboard::Result r;
    if (!keyboardAction(kb_, a, steps, &r)) return false;
    if (r == Keyboard::Result::Action) search();
    else if (r == Keyboard::Result::Typed) kb_.keepAtMost(60);
    return true;
  }
  bool step(float dt) override { return showing_ ? results_.step(dt) : kb_.step(dt); }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    float bx = float(e.dp(8)), by = float(e.dp(5)), bw = w - 2 * bx, bh = float(e.dp(26));
    e.c.shape({bx, by, bw, bh}, e.dpf(8), p.rowLine);
    e.c.shape({bx + 1, by + 1, bw - 2, bh - 2}, e.dpf(8) - 1, p.header);
    int icon = e.dp(11);
    float ix = bx + e.dp(10);
    e.c.layer(3);
    e.icons.draw(e.c, Icon::Search, {ix, by + float((int(bh) - icon) / 2), float(icon), float(icon)}, p.muted);
    float fx = ix + icon + e.dp(7), fw = bx + bw - e.dp(10) - fx;
    Label field = label(e, showing_ ? kb_.typed() : kb_.typed() + "|", 14, false, fw);
    drawLabel(e, field, fx, by + float((int(bh) - field.h) / 2), p.text);
    float sy = by + bh + e.dp(3);
    Label sum = label(e, summary_.empty() ? tr("rss.searchHint") : summary_, 10, false, w - 2 * e.dp(8));
    drawLabel(e, sum, centerText(float(e.dp(8)), w - 2 * e.dp(8), sum.w), sy, p.muted);
    float area = sy + sum.h + e.dp(3);
    if (showing_) {
      results_.layout(e, w, h - area);
      results_.draw(e, 0, area);
      return;
    }
    drawKeyboard(e, kb_, area, w, h, tr("library.keyboardHint"), p.muted, p.text);
  }

 private:
  void search() {
    std::string q = kb_.typed();
    if (q.find_first_not_of(' ') == std::string::npos) return;
    summary_ = tr("rss.searching");
    std::string path = std::string(Api::kLocal) + "rss/search?country=" + rssCountry() + "&q=" + Api::escape(q);
    requests_.get(path, 600000, [this](int status, const std::string& body) {
      if (status != 200) {
        summary_ = status == 0 ? tr("rss.offline") : status == 429 ? tr("rss.busy") : tr("rss.cantLoad");
        return;
      }
      std::vector<RssShow> shows = parseRssShows(body);
      if (shows.empty()) {
        summary_ = tr("rss.nothingFound");
        return;
      }
      summary_ = tr("podcasts.showCount", {{"count", shows.size()}});
      std::vector<Row> rows;
      for (const RssShow& s : shows) rows.push_back(showRow(s));
      results_.setRows(std::move(rows));
      showing_ = true;
    });
  }
  Keyboard kb_;
  RowList results_;
  bool showing_ = false;
  std::string summary_;
  Requests requests_;
};

// A show's page: its cover and what it is above; Subscribe, Speed, Download new episodes, About, then
// its episodes (no pictures: its cover is above), newest first or a story's first first
class ShowScreen : public Screen {
 public:
  explicit ShowScreen(RssShow s) : show_(std::move(s)) {
    rssNoteFeed(show_, {});  // what the search or the chart said of it (Apple's picture), kept a while
    list_.fill = [this](int i, Row& r) {
      int k = i - first_;
      if (k >= 0 && k < int(eps_.size())) fillRow(eps_[size_t(k)], false, r);
    };
    if (const RssShow* known = rssShow(show_.id)) merge(*known);
    render();
    load();
  }
  std::string title() const override { return show_.title.empty() ? tr("menu.podcasts") : show_.title; }
  bool waitsForContent() const override { return true; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.down(steps);
      case Action::Up: return list_.up(steps);
      case Action::Select: return list_.select();
      case Action::LongSelect: return list_.longSelect();
      default: return false;
    }
  }
  bool step(float dt) override { return list_.step(dt); }
  void onShow() override { render(); }
  bool tick(int64_t now) override {
    if (retryAt_ && now >= retryAt_) retryAt_ = 0, load();
    if (serial_ == rssSerial() && changes_ == g_library.bookChanges() && downloads_ == downloadsSerial()) return false;
    serial_ = rssSerial(), changes_ = g_library.bookChanges(), downloads_ = downloadsSerial();
    if (subscribed_ != rssSubscribed(show_.id)) render();
    return true;  // the rows say so as they're drawn
  }
  int64_t wakeAt() const override { return retryAt_; }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    int pad = e.dp(10), art = e.dp(64);
    float headH = float(pad + art + e.dp(8));
    Rect ar{float(pad), float(pad), float(art), float(art)};
    e.c.layer(2);
    float a = 1;
    std::string thumb = rssArt(show_.art);
    const Texture* tex = thumb.empty() ? nullptr : e.covers.get(thumb, art, art, 0, e.now, &a);
    if (!tex || a < 1) drawTile(e, tiles::podcasts, ar, 1);
    e.c.layer(3);
    if (tex) e.c.image(*tex, ar, {0, 0, 1, 1}, e.dpf(5), withAlpha(0xffffffff, a));
    e.c.layer(4);
    float tx = ar.x + art + e.dp(10), tw = w - tx - pad, y = ar.y;
    for (const Label& l : wrap(e, show_.title.empty() ? show_.feed : show_.title, 15, true, tw, 2)) {
      drawLabel(e, l, tx, y, p.text);
      y += l.h;
    }
    auto line = [&](const std::string& s, float sp, Color c) {
      if (s.empty()) return;
      Label l = label(e, s, sp, false, tw);
      y += e.dp(1);
      drawLabel(e, l, tx, y, c);
      y += l.h;
    };
    line(show_.author, 12, p.text);
    std::string count = loaded_ ? tr("podcasts.episodeCount", {{"count", eps_.size()}}) : show_.count ? tr("podcasts.episodeCount", {{"count", show_.count}}) : "";
    if (!show_.genre.empty()) count += (count.empty() ? "" : " \xc2\xb7 ") + show_.genre;
    line(count, 11, p.muted);
    e.c.layer(0);
    e.c.fill({0, headH - 1, w, 1}, p.rowLine);
    list_.layout(e, w, h - headH);
    list_.draw(e, 0, headH);
  }

 private:
  void merge(const RssShow& k) {
    if (show_.title.empty()) show_.title = k.title;
    if (show_.author.empty()) show_.author = k.author;
    if (!k.art.empty() && (show_.art.empty() || k.art.find("mzstatic.com/") != std::string::npos)) show_.art = k.art;
    if (show_.feed.empty()) show_.feed = k.feed;
    if (k.priv) show_.priv = true;
    if (show_.description.empty()) show_.description = k.description;
    if (show_.genre.empty()) show_.genre = k.genre;
  }
  void load() {
    requests_.get(rssFeedPath(show_, false, 0, false), 0, [this](int status, const std::string& body) {
      if (status == 202 && tries_++ < 45) {  // still being read: asked again in 2 s
        retryAt_ = now_ns() + 2 * kSec;
        if (!loaded_) message_ = tr("rss.reading");
        return render();
      }
      RssShow got;
      std::vector<RssEpisode> eps;
      if (status != 200 || !parseRssFeed(body, got, eps)) {
        message_ = status == 0 || status == 504 ? tr("rss.offline") : status == 422 ? tr("rss.add.notFeed") : tr("rss.cantLoad");
        return render();
      }
      got.id = show_.id;  // the address it was found by is its name, even if the feed moved
      got.priv = show_.priv;
      if (show_.art.find("mzstatic.com/") != std::string::npos) got.art = show_.art;  // Apple's, sized for the Y1
      if (got.genre.empty()) got.genre = show_.genre;
      if (!got.apple) got.apple = show_.apple;
      show_ = got;
      sortRssEpisodes(eps, show_.serial);
      for (RssEpisode& e : eps) e.show = show_.id, e.showTitle = show_.title;
      rssNoteFeed(show_, eps);
      eps_ = std::move(eps);
      loaded_ = true;
      message_.clear();
      render();
    });
  }
  void render() {
    subscribed_ = rssSubscribed(show_.id);
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile& tile, std::string subtitle, std::string trailing, bool chevron, std::function<void()> on) {
      Row r;
      r.title = title, r.tile = &tile, r.subtitle = std::move(subtitle), r.trailing = std::move(trailing), r.chevron = chevron;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    if (loaded_ || subscribed_)
      add(subscribed_ ? tr("rss.subscribed") : tr("rss.subscribe"), tiles::saveShow, subscribed_ ? tr("rss.subscribedHint") : tr("rss.subscribeHint"), "",
          false, [this] { toggle(); });
    std::string key = rssShowSpeedKey(show_.id);
    add(tr("book.speed"), tiles::speed, "", speedLabel(spokenSpeed(key)), true, [key] { g_app->push(makeBookSpeed(key)); });
    if (subscribed_) {
      RssAuto a = rssAuto(show_.id);
      std::string id = show_.id;
      add(tr("rss.autoDownload"), tiles::downloads, a.on ? tr("rss.autoKeep", {{"count", a.keep}}) : tr("rss.autoHint"), a.on ? tr("common.on") : tr("common.off"),
          true, [id] { g_app->push(makeAuto(id)); });
    }
    if (!show_.description.empty()) {
      std::string title = show_.title, text = show_.description;
      add(tr("rss.about"), tiles::about, "", "", true, [title, text] { g_app->push(makeText(title, text)); });
    }
    first_ = int(rows.size()) + 1;
    if (!loaded_) {
      rows.push_back(header(tr("podcasts.episodes")));
      rows.push_back(note(message_.empty() ? tr("common.loading") : message_, "", nullptr));
      first_ = int(rows.size());
    } else {
      rows.push_back(header(show_.serial ? tr("rss.episodesInOrder") : tr("podcasts.episodes")));
      if (eps_.empty()) rows.push_back(note(tr("podcasts.noEpisodes"), ""));
      for (const RssEpisode& e : eps_) rows.push_back(episodeRow(e, false, false));
    }
    list_.setRows(std::move(rows), true);
  }
  void toggle() {
    if (!subscribed_) {
      rssSubscribe(show_, eps_);
      g_app->toast(tr("rss.subscribedToast"));
      return render();
    }
    bool any = false;
    for (const RssEpisode& e : rssDownloaded()) any = any || e.show == show_.id;
    std::string id = show_.id;
    if (!any) {
      rssUnsubscribe(id, false);
      g_app->toast(tr("rss.unsubscribedToast"));
      return render();
    }
    // its downloads: kept or deleted with it
    g_app->push(makeChoice(tr("rss.unsubscribe"), {{tr("rss.keepDownloads"), "keep"}, {tr("rss.deleteDownloads"), "delete"}, {tr("rss.stay"), "stay"}},
                           [] { return std::string(); },
                           [id](const std::string& v) {
                             if (v == "stay") return;
                             rssUnsubscribe(id, v == "delete");
                             g_app->toast(tr("rss.unsubscribedToast"));
                           }));
  }

 public:
  static std::unique_ptr<Screen> makeAuto(const std::string& id);
  static std::unique_ptr<Screen> makeText(const std::string& title, const std::string& text);

 private:
  RssShow show_;
  std::vector<RssEpisode> eps_;
  RowList list_;
  int first_ = 0, tries_ = 0;
  bool loaded_ = false, subscribed_ = false;
  std::string message_;
  int64_t retryAt_ = 0;
  uint32_t serial_ = 0, downloads_ = 0;
  uint64_t changes_ = 0;
  Requests requests_;
};

// A show's "Download new episodes": on or off, how many kept, played ones deleted or not
class AutoScreen : public ShellListScreen {
 public:
  explicit AutoScreen(std::string id) : ShellListScreen(tr("rss.autoDownload")), id_(std::move(id)) { render(); }
  void onShow() override { render(); }

 private:
  void render() {
    RssAuto a = rssAuto(id_);
    std::vector<Row> rows;
    std::string id = id_;
    Row on;
    on.title = tr("rss.autoDownload");
    on.subtitle = tr("rss.autoOnHint");
    on.trailing = a.on ? tr("common.on") : tr("common.off");
    on.tile = &tiles::downloads;
    on.onSelect = [this, id] {
      RssAuto now = rssAuto(id);
      now.on = !now.on;
      setRssAuto(id, now);
      render();
    };
    rows.push_back(on);
    if (a.on) {
      Row keep;
      keep.title = tr("rss.keep");
      keep.trailing = tr("rss.keepLatest", {{"count", a.keep}});
      keep.tile = &tiles::folders;
      keep.chevron = true;
      keep.onSelect = [id] {
        std::vector<std::pair<std::string, std::string>> c;
        for (int k : kRssKeeps) c.push_back({tr("rss.keepLatest", {{"count", k}}), std::to_string(k)});
        g_app->push(makeChoice(tr("rss.keep"), c, [id] { return std::to_string(rssAuto(id).keep); }, [id](const std::string& v) {
          RssAuto now = rssAuto(id);
          now.keep = atoi(v.c_str());
          setRssAuto(id, now);
        }));
      };
      rows.push_back(keep);
      Row del;
      del.title = tr("rss.deletePlayed");
      del.subtitle = tr("rss.deletePlayedHint");
      del.trailing = a.deletePlayed ? tr("common.on") : tr("common.off");
      del.tile = &tiles::del;
      del.onSelect = [this, id] {
        RssAuto now = rssAuto(id);
        now.deletePlayed = !now.deletePlayed;
        setRssAuto(id, now);
        render();
      };
      rows.push_back(del);
    }
    list_.setRows(std::move(rows), true);
  }
  std::string id_;
};

// Text to read (show notes, a show's description): the wheel scrolls it
class TextScreen : public Screen {
 public:
  TextScreen(std::string title, std::string text) : title_(std::move(title)), text_(std::move(text)) {}
  std::string title() const override { return title_; }
  bool onAction(Action a, int steps) override {
    if (a != Action::Down && a != Action::Up) return false;
    float by = float(std::max(1, steps)) * lineH_ * 2;
    target_ = std::max(0.f, std::min(maxScroll_, target_ + (a == Action::Down ? by : -by)));
    return true;
  }
  bool step(float dt) override { return scroll_.step(target_, dt, 22); }
  void setText(std::string t) {
    text_ = std::move(t);
    laidFor_ = -1;
  }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    int pad = e.dp(12);
    float tw = w - 2 * pad;
    if (laidFor_ != w) {  // laid out once for the width
      laidFor_ = w;
      lines_.clear();
      std::string para;
      for (size_t i = 0; i <= text_.size(); i++) {
        if (i < text_.size() && text_[i] != '\n') {
          para += text_[i];
          continue;
        }
        if (para.empty()) lines_.push_back("");
        else
          for (const WrappedLine& l : wrapText(e, para, 13, false, tw, 400)) lines_.push_back(l.text);
        para.clear();
      }
      lineH_ = float(label(e, "Ag", 13, false).h + e.dp(2));
      maxScroll_ = std::max(0.f, lines_.size() * lineH_ + 2 * pad - h);
    }
    e.c.layer(3);
    float y = float(pad) - roundf(scroll_.x);
    for (const std::string& s : lines_) {
      if (y + lineH_ >= 0 && y < h && !s.empty()) drawLabel(e, label(e, s, 13, false, tw), float(pad), y, p.text);
      y += lineH_;
    }
  }

 private:
  std::string title_, text_;
  std::vector<std::string> lines_;
  float laidFor_ = -1, lineH_ = 18, maxScroll_ = 0, target_ = 0;
  Spring scroll_;
};

std::unique_ptr<Screen> ShowScreen::makeAuto(const std::string& id) { return std::unique_ptr<Screen>(new AutoScreen(id)); }
std::unique_ptr<Screen> ShowScreen::makeText(const std::string& title, const std::string& text) {
  return std::unique_ptr<Screen>(new TextScreen(title, text));
}

// An episode's options: Play (Resume), Listen only (a video), Start again, Play next, Add to queue,
// Download, Save for later, Mark as played, Show notes, Go to show
class EpisodeMenuScreen : public ShellListScreen {
 public:
  EpisodeMenuScreen(RssEpisode e, bool onItsShow) : ShellListScreen(e.title), e_(std::move(e)), onShow_(onItsShow) { render(); }
  void onShow() override { render(); }
  bool tick(int64_t) override {
    if (serial_ == rssSerial() && changes_ == g_library.bookChanges() && downloads_ == downloadsSerial()) return false;
    render();
    return true;
  }

 private:
  void render() {
    serial_ = rssSerial(), changes_ = g_library.bookChanges(), downloads_ = downloadsSerial();
    if (const RssEpisode* k = rssKept(e_.id)) {
      std::string url = e_.url.empty() ? k->url : e_.url;
      e_ = *k;
      if (!url.empty()) e_.url = url;
    }
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile& tile, std::string subtitle, std::function<void()> f) {
      Row r;
      r.title = title, r.tile = &tile, r.subtitle = std::move(subtitle);
      r.onSelect = std::move(f);
      rows.push_back(std::move(r));
    };
    bool finished = false;
    bool started = rssStarted(e_, &finished);
    const BookPlay* b = bookNow();
    bool playing = b && b->key == rssKey(e_.id) && g_player.state().playing;
    RssEpisode e = e_;
    if (playing) {
      add(tr("menu.nowPlaying"), tiles::playNow, rssSubtitle(e, false), [] { openNowPlaying(); });
    } else {
      add(started ? tr("book.resume") : e.video() ? tr("rss.watch") : tr("podcasts.play"), tiles::playNow, rssSubtitle(e, false), [e] {
        g_app->pop();
        playNow(e);
      });
    }
    if (e.video() && !playing)
      add(tr("rss.listenOnly"), tiles::listenOnly, tr("rss.listenOnlyNote"), [e] {
        g_app->pop();
        listenRss(e);
        openNowPlaying();
      });
    if (started || finished)
      add(tr("book.startAgain"), tiles::startAgain, "", [e] {
        g_app->pop();
        playRssFrom(e, 0);
        if (!e.video()) openNowPlaying();
      });
    if (!e.video()) {
      add(tr("music.playNext"), tiles::playNext, "", [e] {
        g_app->pop();
        queueRss(e, true);
      });
      add(tr("music.addToQueue"), tiles::addToQueue, "", [e] {
        g_app->pop();
        queueRss(e, false);
      });
    }
    // Download: the card's copy plays with no connection
    if (!downloadedRss(e.id).empty()) {
      add(tr("podcasts.deleteDownload"), tiles::downloads, tr("podcasts.deleteDownloadHint"), [e] {
        g_app->push(makeChoice(tr("music.deleteDownload"), {{tr("music.keepIt"), "keep"}, {tr("music.deleteIt"), "delete"}}, [] { return std::string("keep"); },
                               [e](const std::string& v) {
                                 if (v == "delete" && deleteRssDownload(e.id)) g_app->toast(tr("music.deletedFromCard"));
                               }));
      });
    } else if (const DownloadJob* j = jobOf(e.id)) {
      add(j->state == DownloadJob::Paused ? tr("music.downloadPaused") : tr("music.downloading"), tiles::downloads, downloadRowState(*j, ""),
          [] { g_app->push(makeRssDownloaded()); });
    } else {
      std::string size = e.size > 0 ? bytes(uint64_t(e.size)) : "";
      add(tr("music.download"), tiles::downloads, size.empty() ? tr("podcasts.downloadHint") : tr("rss.downloadSize", {{"size", size}}), [this, e] {
        downloadRss(e);
        render();
      });
    }
    bool later = rssIsLater(e.id);
    add(later ? tr("podcasts.removeFromLater") : tr("podcasts.saveForLater"), tiles::later, later ? "" : tr("rss.laterHint"), [this, e, later] {
      setRssLater(e, !later);
      g_app->toast(later ? tr("podcasts.removedFromLater") : tr("podcasts.addedToLater"));
      render();
    });
    add(finished ? tr("podcasts.markUnplayed") : tr("podcasts.markPlayed"), tiles::finished, "", [this, e, finished] {
      markRssPlayed(e, !finished);
      g_app->toast(finished ? tr("podcasts.markedUnplayed") : tr("podcasts.markedPlayed"));
      render();
    });
    add(tr("rss.showNotes"), tiles::showNotes, "", [e] {
      auto screen = std::unique_ptr<TextScreen>(new TextScreen(e.title, tr("common.loading")));
      TextScreen* s = screen.get();
      auto alive = std::make_shared<bool>(true);
      g_app->push(std::move(screen));
      rssNotes(e, [s, alive, e](const std::string& notes) {
        // the screen may have closed: only while it's the one in front
        if (g_app->top() == s) s->setText(notes.empty() ? tr("rss.noNotes") : notes);
        (void)alive;
      });
    });
    if (!onShow_) {
      const RssShow* s = rssShow(e.show);
      if (s) {
        RssShow copy = *s;
        add(tr("rss.goToShow"), tiles::podcasts, copy.title, [copy] { openRssShow(copy); });
      }
    }
    list_.setRows(std::move(rows), true);
  }
  RssEpisode e_;
  bool onShow_;
  uint32_t serial_ = 0, downloads_ = 0;
  uint64_t changes_ = 0;
};

// The main menu's Podcasts
class HomeScreen : public ShellListScreen {
 public:
  HomeScreen() : ShellListScreen(tr("menu.podcasts")) {
    list_.fill = [this](int i, Row& r) {
      if (i >= 0 && i < int(going_.size()) + 1 && i >= 1) fillRow(going_[size_t(i - 1)], true, r);
    };
    render();
  }
  void onShow() override {
    rssCheck(true);  // new episodes, at most every 15 min
    render();
  }
  bool tick(int64_t) override {
    if (serial_ == rssSerial() && changes_ == g_library.bookChanges() && downloads_ == downloadsSerial() && checking_ == rssChecking()) return false;
    render();
    return true;
  }

 private:
  void render() {
    serial_ = rssSerial(), changes_ = g_library.bookChanges(), downloads_ = downloadsSerial(), checking_ = rssChecking();
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile& tile, const std::string& subtitle, std::function<void()> f) {
      Row r;
      r.title = title, r.tile = &tile, r.subtitle = subtitle, r.chevron = true;
      r.onSelect = std::move(f);
      rows.push_back(std::move(r));
    };
    // Continue listening: the started ones not finished, the last played first
    going_ = rssInProgress();
    if (going_.size() > 3) going_.resize(3);
    if (!going_.empty()) {
      rows.push_back(header(tr("books.continue")));
      for (const RssEpisode& e : going_) rows.push_back(episodeRow(e, true, true));
      rows.push_back(header(tr("menu.podcasts")));
    }
    int fresh = rssNewCount();
    add(tr("podcasts.newEpisodes"), tiles::newEpisodes,
        checking_ ? tr("rss.checking") : fresh ? tr("rss.newCount", {{"count", fresh}}) : rssShows().empty() ? tr("rss.newRowHint") : tr("rss.newNoneShort"),
        [] { g_app->push(std::unique_ptr<Screen>(new EpisodesScreen(tr("podcasts.newEpisodes"), EpisodesScreen::Kind::New))); });
    size_t shows = rssShows().size();
    add(tr("podcasts.yourShows"), tiles::podcasts, shows ? tr("podcasts.showCount", {{"count", shows}}) : tr("rss.yourShowsHint"),
        [] { g_app->push(std::unique_ptr<Screen>(new ShowsScreen())); });
    size_t later = rssLater().size();
    add(tr("podcasts.saveForLater"), tiles::later, later ? tr("podcasts.episodeCount", {{"count", later}}) : tr("podcasts.laterRowHint"),
        [] { g_app->push(std::unique_ptr<Screen>(new EpisodesScreen(tr("podcasts.saveForLater"), EpisodesScreen::Kind::Later))); });
    add(tr("rss.topShows"), tiles::topShows, tr("rss.topShowsHint", {{"country", rssCountryName(rssCountry())}}), [] {
      std::string path = std::string(Api::kLocal) + "rss/top?country=" + rssCountry();
      g_app->push(std::unique_ptr<Screen>(new FoundShowsScreen(tr("rss.topShows"), path, true, true)));
    });
    int onCard = int(rssDownloaded().size()), coming = 0;
    for (const DownloadJob& j : downloadJobs()) coming += j.rss && j.state != DownloadJob::Done && j.state != DownloadJob::Failed;
    add(tr("podcasts.downloaded"), tiles::downloads,
        coming ? tr("downloads.downloadingCount", {{"count", coming}}) : onCard ? tr("downloads.onCard", {{"count", onCard}}) : tr("podcasts.downloadedHint"),
        [] { g_app->push(makeRssDownloaded()); });
    add(tr("podcasts.search"), tiles::search, tr("rss.searchRowHint"), [] { g_app->push(std::unique_ptr<Screen>(new SearchScreen())); });
    // its own settings: here, not in Settings
    add(tr("rss.settings"), tiles::settings, rssCountryName(rssCountry()), [] { g_app->push(makeRssSettings()); });
    list_.setRows(std::move(rows), true);
  }
  std::vector<RssEpisode> going_;
  uint32_t serial_ = 0, downloads_ = 0;
  uint64_t changes_ = 0;
  bool checking_ = false;
};

// Podcasts › Podcast settings: the country whose directory and charts are asked; looking for new episodes now
class CountryScreen : public LetterListScreen {
 public:
  CountryScreen() : LetterListScreen(tr("rss.country")) {
    std::vector<std::pair<std::string, std::string>> all;
    for (int i = 0; i < kRssCountryCount; i++) all.push_back({rssCountryName(kRssCountries[i]), kRssCountries[i]});
    std::sort(all.begin(), all.end(), [](auto& a, auto& b) { return fold(a.first) < fold(b.first); });
    std::string now = rssCountry();
    std::vector<Row> rows;
    int at = 0;
    for (auto& [name, code] : all) {
      Row r;
      r.title = name;
      if (code == now) r.trailing = tr("common.current"), at = int(rows.size());
      std::string c = code;
      r.onSelect = [c] {
        setRssCountry(c);
        g_app->pop();
      };
      rows.push_back(r);
    }
    list_.setRows(std::move(rows));
    list_.selectIndex(at);
    indexLetters();
  }
};

class SettingsScreen : public ShellListScreen {
 public:
  SettingsScreen() : ShellListScreen(tr("rss.settings")) { render(); }
  void onShow() override { render(); }
  bool tick(int64_t) override {
    if (checking_ == rssChecking() && serial_ == rssSerial()) return false;
    render();
    return true;
  }

 private:
  void render() {
    checking_ = rssChecking(), serial_ = rssSerial();
    std::vector<Row> rows;
    Row c;
    c.title = tr("rss.country");
    c.subtitle = tr("rss.countryHint");
    c.trailing = rssCountryName(rssCountry());
    c.tile = &tiles::language;
    c.chevron = true;
    c.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new CountryScreen())); };
    rows.push_back(c);
    Row n;
    n.title = tr("rss.checkNow");
    int64_t at = rssCheckedAt();
    n.subtitle = checking_ ? tr("rss.checking")
                 : at       ? tr("rss.checkedAt", {{"time", clockDate(time_t(at / 1000), false) + ", " + clockTime(time_t(at / 1000), g_prefs.clock24)}})
                            : tr("rss.checkHint");
    n.tile = &tiles::rescan;
    n.onSelect = [] {
      rssCheckNow();
      if (!rssChecking()) g_app->toast(rssShows().empty() ? tr("rss.noShows") : tr("rss.offline"));
    };
    rows.push_back(n);
    list_.setRows(std::move(rows), true);
  }
  bool checking_ = false;
  uint32_t serial_ = 0;
};

}  // namespace

std::unique_ptr<Screen> makeRssHome() { return std::unique_ptr<Screen>(new HomeScreen()); }
void openRssShow(const RssShow& s) { g_app->push(std::unique_ptr<Screen>(new ShowScreen(s))); }
void openRssEpisodeMenu(const RssEpisode& e, bool onItsShow) { g_app->push(std::unique_ptr<Screen>(new EpisodeMenuScreen(e, onItsShow))); }
std::unique_ptr<Screen> makeRssUpNext() { return std::unique_ptr<Screen>(new EpisodesScreen(tr("podcasts.upNext"), EpisodesScreen::Kind::UpNext)); }
std::unique_ptr<Screen> makeRssDownloaded() { return std::unique_ptr<Screen>(new EpisodesScreen(tr("podcasts.downloaded"), EpisodesScreen::Kind::Downloaded)); }
std::unique_ptr<Screen> makeRssSettings() { return std::unique_ptr<Screen>(new SettingsScreen()); }

}  // namespace rondo::shell
