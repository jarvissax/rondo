// YouTube Music's podcasts on the Y1 (podcasts.h): a show's screen, an episode's menu, the lists of
// episodes (Episodes for Later, New episodes, Downloaded, Episodes up next), and the Podcasts pill's
// rows. The YouTube Music section's dark look (MusicListScreen); an episode's row says where it is
// (Played, "1 h 20 min left", YouTube's length) as it's drawn, so a place moving shows at once.
#include <yyjson.h>

#include <algorithm>

#include "books.h"
#include "downloads.h"
#include "music_list.h"
#include "podcasts.h"

namespace rondo::shell {
namespace {

std::string sval(yyjson_val* o, const char* k) {
  yyjson_val* v = yyjson_obj_get(o, k);
  return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string();
}

void playNow(const Episode& e) {
  playEpisode(e);
  openNowPlaying();
}

Row episodeRow(const Episode& e) {
  Row r;
  r.title = e.title;
  r.thumb = e.thumb;
  r.ahead = e.id;
  Episode copy = e;
  r.onSelect = [copy] { playNow(copy); };
  r.onLongSelect = [copy] { openEpisodeMenu(copy); };
  return r;
}

// A list whose rows end in episodes: head_ rows first (a show's Save and Speed), then one row an
// episode; the episodes' words are filled in when drawn
class EpisodeListScreen : public MusicListScreen {
 public:
  EpisodeListScreen(std::string title, bool withShow) : MusicListScreen(std::move(title)), withShow_(withShow) {
    list_.fill = [this](int i, Row& r) {
      int k = i - first_;
      if (k < 0 || k >= int(eps_.size())) return;
      const Episode& e = eps_[size_t(k)];
      r.subtitle = episodeSubtitle(e, withShow_);
      r.dot = episodeNew(e);
      const BookPlay* b = bookNow();
      r.trailing = b && b->key == episodeKey(e.id) ? tr("songList.playing") : downloadedEpisode(e.id).empty() ? "" : tr("podcasts.onCardMark");
    };
  }
  bool tick(int64_t now) override {
    bool mini = MusicListScreen::tick(now);
    if (g_library.bookChanges() == changes_) return mini;
    changes_ = g_library.bookChanges();
    return true;  // a place, a speed or a mark changed: the rows say so
  }

 protected:
  void setEpisodes(std::vector<Row> head, std::vector<Episode> eps, bool keep = false) {
    first_ = int(head.size());
    eps_ = std::move(eps);
    std::vector<Row> rows = std::move(head);
    for (const Episode& e : eps_) rows.push_back(episodeRow(e));
    list_.setRows(std::move(rows), keep);
  }
  void appendEpisodes(const std::vector<Episode>& more) {
    std::vector<Row> rows;
    for (const Episode& e : more) {
      eps_.push_back(e);
      rows.push_back(episodeRow(e));
    }
    list_.appendRows(std::move(rows));
  }
  std::vector<Episode> eps_;
  int first_ = 0;
  bool withShow_;
  uint64_t changes_ = 0;
  Requests requests_;
};

// A paged list of episodes from the source: Episodes for Later, New episodes, a search's episodes
class PagedEpisodes : public EpisodeListScreen {
 public:
  PagedEpisodes(std::string title, std::string path, std::string empty) : EpisodeListScreen(std::move(title), true), path_(std::move(path)), empty_(std::move(empty)) {
    list_.onNearEnd = [this] { more(); };
    load();
  }
  void onShow() override {
    MusicListScreen::onShow();
    if (stale_) load();  // one removed in its menu: the list as YouTube has it now
  }

 private:
  void load() {
    stale_ = false;
    list_.setMessage(tr("common.loading"));
    std::string p = path_;
    requests_.get(p, podcastCacheMs(), [this, p](int status, const std::string& body) {
      if (status != 200) return list_.setMessage(offlineOr(tr("music.cantLoad", {{"problem", httpProblem(status, p)}})));
      std::vector<Episode> eps;
      parseEpisodes(body, eps, nullptr, &next_);
      noteEpisodes(eps);
      if (eps.empty()) return list_.setMessage(empty_);
      setEpisodes({}, eps);
    });
  }
  void more() {
    if (next_.empty() || loading_) return;
    loading_ = true;
    std::string p = path_ + (path_.find('?') == std::string::npos ? "?next=" : "&next=") + Api::escape(next_);
    requests_.get(p, 0, [this](int status, const std::string& body) {
      loading_ = false;
      if (status == 410) next_.clear();
      if (status != 200) return;
      std::vector<Episode> eps;
      parseEpisodes(body, eps, nullptr, &next_);
      noteEpisodes(eps);
      appendEpisodes(eps);
    });
  }
  std::string path_, empty_, next_;
  bool loading_ = false;

 public:
  bool stale_ = false;
};

// A show: Save to library (signed in), its speed, then its episodes newest first (paged, as YouTube
// pages them: hundreds load as the wheel goes down)
class PodcastScreen : public EpisodeListScreen {
 public:
  PodcastScreen(std::string id, std::string title) : EpisodeListScreen(std::move(title), false), id_(std::move(id)) {
    list_.onNearEnd = [this] { more(); };
    load();
  }
  void onShow() override {
    MusicListScreen::onShow();
    if (loaded_) setEpisodes(head(), eps_, true);  // its speed, if it was just chosen
  }

 private:
  std::string path() const { return "/music/podcast/" + Api::escape(id_); }
  void load() {
    list_.setMessage(tr("common.loading"));
    std::string p = path();
    requests_.get(p, podcastCacheMs(), [this, p](int status, const std::string& body) {
      if (status == 404) return list_.setMessage(tr("podcasts.needNewerServer"));
      if (status != 200) return list_.setMessage(offlineOr(tr("music.cantLoad", {{"problem", httpProblem(status, p)}})));
      if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
        yyjson_val* r = yyjson_doc_get_root(d);
        if (!sval(r, "title").empty()) title_ = sval(r, "title");
        by_ = sval(r, "subtitle");
        thumb_ = sval(r, "thumb");
        yyjson_val* s = yyjson_obj_get(r, "saved");
        saved_ = yyjson_is_bool(s) ? (yyjson_get_bool(s) ? 1 : 0) : -1;
        yyjson_doc_free(d);
      }
      std::vector<Episode> eps;
      parseEpisodes(body, eps, nullptr, &next_);
      for (Episode& e : eps) {
        if (e.show.empty()) e.show = title_;
        if (e.showId.empty()) e.showId = id_;
      }
      noteEpisodes(eps);
      loaded_ = true;
      setEpisodes(head(), eps);
      if (eps.empty()) list_.appendRows({note(tr("podcasts.noEpisodes"))});
    });
  }
  static Row note(const std::string& text) {
    Row r;
    r.header = true;
    r.title = text;
    return r;
  }
  std::vector<Row> head() {
    std::vector<Row> rows;
    if (saved_ >= 0) {
      Row s;
      s.title = saved_ ? tr("podcasts.savedToLibrary") : tr("podcasts.saveToLibrary");
      s.subtitle = saved_ ? tr("podcasts.savedHint") : tr("podcasts.saveHint");
      s.tile = &tiles::saveShow;
      s.onSelect = [this] { toggleSaved(); };
      rows.push_back(s);
    }
    Row sp;
    sp.title = tr("book.speed");
    sp.subtitle = by_.empty() ? tr("podcasts.speedHint") : by_;
    sp.trailing = speedLabel(spokenSpeed(showSpeedKey(id_)));
    sp.tile = &tiles::speed;
    sp.chevron = true;
    std::string key = showSpeedKey(id_);
    sp.onSelect = [key] { g_app->push(makeBookSpeed(key)); };
    rows.push_back(sp);
    Row h;
    h.header = true;
    h.title = tr("podcasts.episodes");
    rows.push_back(h);
    return rows;
  }
  void toggleSaved() {
    int want = saved_ ? 0 : 1;
    saved_ = want;  // at once; back if YouTube says no
    setEpisodes(head(), eps_, true);
    saveShow(id_, want, [this, want](bool ok) {
      if (ok) return g_app->toast(want ? tr("podcasts.savedToast") : tr("podcasts.removedToast"));
      saved_ = !want;
      setEpisodes(head(), eps_, true);
      g_app->toast(tr("podcasts.accountFailed"));
    });
  }
  void more() {
    if (next_.empty() || loading_) return;
    loading_ = true;
    std::string p = path() + "?next=" + Api::escape(next_);
    requests_.get(p, 0, [this](int status, const std::string& body) {
      loading_ = false;
      if (status == 410) next_.clear();
      if (status != 200) return;
      std::vector<Episode> eps;
      parseEpisodes(body, eps, nullptr, &next_);
      for (Episode& e : eps) {
        if (e.show.empty()) e.show = title_;
        if (e.showId.empty()) e.showId = id_;
      }
      noteEpisodes(eps);
      appendEpisodes(eps);
    });
  }
  std::string id_, by_, thumb_, next_;
  int saved_ = -1;  // -1: signed out (YouTube can't say)
  bool loading_ = false, loaded_ = false;
};

// Downloaded: the episodes on the card (center plays, hold: its menu, Delete download in it)
class DownloadedScreen : public EpisodeListScreen {
 public:
  DownloadedScreen() : EpisodeListScreen(tr("podcasts.downloaded"), true) { render(); }
  bool waitsForContent() const override { return false; }
  void onShow() override {
    MusicListScreen::onShow();
    render();
  }
  bool tick(int64_t now) override {
    bool t = EpisodeListScreen::tick(now);
    if (downloadsSerial() == serial_) return t;
    serial_ = downloadsSerial();
    render();
    return true;
  }

 private:
  void render() {
    serial_ = downloadsSerial();
    std::vector<Row> head;
    // the ones on their way
    for (const DownloadJob& j : downloadJobs()) {
      if (!j.episode || j.state == DownloadJob::Done) continue;
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
    std::vector<Episode> eps = downloadedEpisodes();
    if (head.empty() && eps.empty()) return list_.setMessage(tr("podcasts.downloadedNone"));
    setEpisodes(std::move(head), eps, true);
  }
  uint32_t serial_ = 0;
};

// Episodes up next: center plays one now, hold takes it off
class UpNextScreen : public EpisodeListScreen {
 public:
  UpNextScreen() : EpisodeListScreen(tr("podcasts.upNext"), true) { render(); }
  bool waitsForContent() const override { return false; }
  void onShow() override {
    MusicListScreen::onShow();
    render();
  }

 private:
  void render() {
    std::vector<Episode> eps = episodesUpNext();
    if (eps.empty()) return list_.setMessage(tr("podcasts.upNextNone"));
    setEpisodes({}, eps, true);
    std::vector<Row> rows = list_.rows();
    for (size_t k = 0; k < eps.size(); k++) {
      Episode e = eps[k];
      rows[k].onSelect = [e] {
        removeFromUpNext(e.id);
        playNow(e);
      };
      rows[k].onLongSelect = [e] {
        g_app->push(makeChoice(e.title, {{tr("podcasts.keepInUpNext"), "keep"}, {tr("podcasts.takeOffUpNext"), "remove"}}, [] { return std::string("keep"); },
                               [e](const std::string& v) {
                                 if (v == "remove") removeFromUpNext(e.id);
                               }));
      };
    }
    list_.setRows(std::move(rows), true);
  }
};

// An episode's menu: Play (Resume), Start again, Play next, Add to queue, Download, Mark as played,
// Episodes for Later, Go to podcast. What YouTube knows of it (Episodes for Later, chapters, length)
// comes a moment after it opens.
class EpisodeMenuScreen : public MusicListScreen {
 public:
  EpisodeMenuScreen(Episode e, bool onItsShow) : MusicListScreen(e.title), e_(std::move(e)), onShow_(onItsShow) {
    render();
    std::string id = e_.id;
    requests_.get("/music/episode/" + Api::escape(id), podcastCacheMs() ? 600000 : 0, [this](int status, const std::string& body) {
      Episode got;
      if (status != 200 || !parseEpisode(body, got)) return;
      if (got.show.empty()) got.show = e_.show;
      if (got.showId.empty()) got.showId = e_.showId;
      if (got.thumb.empty()) got.thumb = e_.thumb;
      e_ = got;
      noteEpisodes({got});
      render();
    });
  }
  bool waitsForContent() const override { return false; }
  void onShow() override {
    MusicListScreen::onShow();
    render();
  }
  bool tick(int64_t now) override {
    bool mini = MusicListScreen::tick(now);
    if (downloadsSerial() == serial_ && g_library.bookChanges() == changes_) return mini;
    render();
    return true;
  }

 private:
  void render() {
    serial_ = downloadsSerial(), changes_ = g_library.bookChanges();
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile& tile, std::string subtitle, std::function<void()> f) {
      Row r;
      r.title = title, r.tile = &tile, r.subtitle = std::move(subtitle);
      r.onSelect = std::move(f);
      rows.push_back(std::move(r));
    };
    const BookState* st = g_library.bookState(episodeKey(e_.id));
    bool started = st && st->lastPlayed > 0 && !st->finished, finished = st ? st->finished : e_.played;
    const BookPlay* b = bookNow();
    bool playing = b && b->key == episodeKey(e_.id) && g_player.state().playing;
    Episode e = e_;
    if (playing) add(tr("menu.nowPlaying"), tiles::playNow, episodeSubtitle(e, false), [] { openNowPlaying(); });
    else
      add(started ? tr("book.resume") : tr("podcasts.play"), tiles::playNow, episodeSubtitle(e, false), [e] {
        g_app->pop();
        playNow(e);
      });
    if (started || finished)
      add(tr("book.startAgain"), tiles::startAgain, "", [e] {
        g_app->pop();
        playEpisodeFrom(e, 0);
        openNowPlaying();
      });
    add(tr("music.playNext"), tiles::playNext, "", [e] {
      g_app->pop();
      queueEpisode(e, true);
    });
    add(tr("music.addToQueue"), tiles::addToQueue, "", [e] {
      g_app->pop();
      queueEpisode(e, false);
    });
    // Download: the card's copy plays with no connection
    bool going = false, paused = false;
    for (const DownloadJob& j : downloadJobs())
      if (j.id == e.id && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running || j.state == DownloadJob::Paused))
        going = true, paused = j.state == DownloadJob::Paused;
    if (!downloadedEpisode(e.id).empty()) {
      add(tr("podcasts.deleteDownload"), tiles::downloads, tr("podcasts.deleteDownloadHint"), [e] {
        g_app->push(makeChoice(tr("music.deleteDownload"), {{tr("music.keepIt"), "keep"}, {tr("music.deleteIt"), "delete"}}, [] { return std::string("keep"); },
                               [e](const std::string& v) {
                                 if (v == "delete" && deleteEpisodeDownload(e.id)) g_app->toast(tr("music.deletedFromCard"));
                               }));
      });
    } else if (going) {
      add(paused ? tr("music.downloadPaused") : tr("music.downloading"), tiles::downloads, tr("podcasts.downloaded"),
          [] { g_app->push(makePodcastDownloads()); });
    } else {
      add(tr("music.download"), tiles::downloads, tr("podcasts.downloadHint"), [this, e] {
        downloadEpisode(e);
        render();
      });
    }
    add(finished ? tr("podcasts.markUnplayed") : tr("podcasts.markPlayed"), tiles::finished, g_prefs.accountMusic ? tr("podcasts.markHint") : "",
        [this, e, finished] {
          markEpisodePlayed(e, !finished);
          g_app->toast(finished ? tr("podcasts.markedUnplayed") : tr("podcasts.markedPlayed"));
          render();
        });
    if (g_prefs.accountMusic) {
      bool later = e_.later == 1;
      add(later ? tr("podcasts.removeFromLater") : tr("podcasts.saveForLater"), tiles::later, e_.later < 0 ? "" : tr("podcasts.laterHint"), [this, later] {
        Episode e = e_;
        e_.later = later ? 0 : 1;  // at once; back if YouTube says no
        render();
        setEpisodeLater(e, !later, [this, later](bool ok) {
          if (ok) return g_app->toast(later ? tr("podcasts.removedFromLater") : tr("podcasts.addedToLater"));
          e_.later = later ? 1 : 0;
          render();
          g_app->toast(tr("podcasts.accountFailed"));
        });
      });
    }
    if (!onShow_ && !e.showId.empty()) {
      std::string id = e.showId, show = e.show;
      add(tr("podcasts.goToPodcast"), tiles::podcasts, show, [id, show] { openPodcast(id, show); });
    }
    list_.setRows(std::move(rows), true);
  }
  Episode e_;
  bool onShow_;
  uint32_t serial_ = 0;
  uint64_t changes_ = 0;
  Requests requests_;
};

// the last part of YouTube's line for a list ("Auto playlist • 12 episodes": "12 episodes")
std::string countOf(const std::string& words) {
  size_t at = words.rfind("\xe2\x80\xa2");
  std::string s = at == std::string::npos ? words : words.substr(at + 3);
  size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

void openPodcast(const std::string& showId, const std::string& title) {
  g_app->push(std::unique_ptr<Screen>(new PodcastScreen(showId, title.empty() ? tr("music.podcasts") : title)));
}

void openEpisodeMenu(const Episode& e, bool onItsShow) { g_app->push(std::unique_ptr<Screen>(new EpisodeMenuScreen(e, onItsShow))); }

std::unique_ptr<Screen> makeEpisodesUpNext() { return std::unique_ptr<Screen>(new UpNextScreen()); }
std::unique_ptr<Screen> makePodcastDownloads() { return std::unique_ptr<Screen>(new DownloadedScreen()); }

std::vector<Row> episodeRows(const std::vector<Episode>& list, bool withShow) {
  std::vector<Row> rows;
  for (const Episode& e : list) {
    Row r = episodeRow(e);
    r.subtitle = episodeSubtitle(e, withShow);
    r.dot = episodeNew(e);
    rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<Row> podcastsTabRows(bool signedIn, int status, const std::string& body, const std::string& problem, std::function<void()> retry) {
  std::vector<Row> rows;
  auto header = [&](const std::string& t) {
    Row h;
    h.header = true;
    h.title = t;
    rows.push_back(h);
  };
  auto add = [&](const std::string& title, const Tile& tile, const std::string& subtitle, std::function<void()> f) {
    Row r;
    r.title = title, r.tile = &tile, r.subtitle = subtitle, r.chevron = true;
    r.onSelect = std::move(f);
    rows.push_back(std::move(r));
  };
  // Continue listening: the episodes started here and not finished, the last played first
  std::vector<Episode> going = episodesInProgress();
  if (going.size() > 3) going.resize(3);
  if (!going.empty()) {
    header(tr("books.continue"));
    for (Row& r : episodeRows(going, true)) rows.push_back(std::move(r));
    header(tr("music.podcasts"));
  }
  std::vector<Episode> next = episodesUpNext();
  if (!next.empty()) add(tr("podcasts.upNext"), tiles::upNext, tr("podcasts.episodeCount", {{"count", next.size()}}), [] { g_app->push(makeEpisodesUpNext()); });
  std::vector<Item> shows;
  std::string later, latest;
  if (signedIn && status == 200)
    if (yyjson_doc* d = yyjson_read(body.data(), body.size(), 0)) {
      yyjson_val* r = yyjson_doc_get_root(d);
      later = countOf(sval(r, "later"));
      latest = countOf(sval(r, "latest"));
      size_t i, n;
      yyjson_val* v;
      yyjson_arr_foreach(yyjson_obj_get(r, "items"), i, n, v) {
        Item it;
        it.kind = "podcast", it.id = sval(v, "id"), it.title = sval(v, "title"), it.subtitle = sval(v, "subtitle"), it.thumb = sval(v, "thumb");
        if (!it.id.empty()) shows.push_back(it);
      }
      yyjson_doc_free(d);
    }
  if (signedIn) {
    add(tr("podcasts.later"), tiles::later, later.empty() ? tr("podcasts.laterRowHint") : later, [] {
      g_app->push(std::unique_ptr<Screen>(new PagedEpisodes(tr("podcasts.later"), "/music/podcasts/later", tr("podcasts.laterNone"))));
    });
    add(tr("podcasts.newEpisodes"), tiles::newEpisodes, latest.empty() ? tr("podcasts.newRowHint") : latest, [] {
      g_app->push(std::unique_ptr<Screen>(new PagedEpisodes(tr("podcasts.newEpisodes"), "/music/podcasts/new", tr("podcasts.newNone"))));
    });
  }
  int onCard = int(downloadedEpisodes().size()), coming = 0;
  for (const DownloadJob& j : downloadJobs()) coming += j.episode && j.state != DownloadJob::Done && j.state != DownloadJob::Failed;
  add(tr("podcasts.downloaded"), tiles::downloads,
      coming ? tr("downloads.downloadingCount", {{"count", coming}}) : onCard ? tr("downloads.onCard", {{"count", onCard}}) : tr("podcasts.downloadedHint"),
      [] { g_app->push(makePodcastDownloads()); });
  add(tr("podcasts.search"), tiles::search, tr("podcasts.searchHint"), [] { g_app->push(makePodcastSearch()); });
  if (!signedIn) {
    header(tr("podcasts.yourShows"));
    Row m;
    m.title = tr("podcasts.signInForShows");
    m.subtitle = tr("podcasts.signInHint");
    m.enabled = false;
    rows.push_back(m);
    return rows;
  }
  header(tr("podcasts.yourShows"));
  if (status == 503) {
    Row m;
    m.title = tr("podcasts.needCookies");
    m.subtitle = tr("podcasts.needCookiesHint");
    m.enabled = false;
    rows.push_back(m);
  } else if (status == 404) {
    Row m;
    m.title = tr("podcasts.needNewerServer");
    m.enabled = false;
    rows.push_back(m);
  } else if (status != 200) {
    Row m;
    m.title = problem;
    m.subtitle = tr("music.centerTriesAgain");
    m.tile = &tiles::rescan;
    m.onSelect = retry;
    rows.push_back(m);
  } else if (shows.empty()) {
    Row m;
    m.title = tr("podcasts.noShows");
    m.subtitle = tr("podcasts.noShowsHint");
    m.enabled = false;
    rows.push_back(m);
  } else {
    for (Row& r : musicRows(shows, [shows] { return shows; })) rows.push_back(std::move(r));
  }
  return rows;
}

}  // namespace rondo::shell
