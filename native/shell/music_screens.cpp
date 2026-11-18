// The YouTube Music screens behind a row (CollectionScreens.kt and SearchScreen.kt in 1.0.x), ported
// 1:1: a playlist or an album (paged), an artist, a song's menu, and the search on the TV keyboard.
#include <algorithm>
#include <random>

#include "../core/api.h"
#include "../ui/keyboard.h"
#include "downloads.h"
#include "music_list.h"

namespace rondo::shell {

namespace {

constexpr Color white = 0xffffffff;

bool queueIsStreams() {
  PlayerState s = g_player.state();
  return s.index >= 0 && isStream(s.path);
}

// SongMenuScreen: hold center on a song: what the YouTube Music app offers in its menu.
class SongMenuScreen : public MusicListScreen {
 public:
  explicit SongMenuScreen(Item song) : MusicListScreen(song.title), song_(std::move(song)) { render(); }

 private:
  void render() {
    std::vector<Row> rows;
    auto add = [&](const char* title, const Tile& tile, std::function<void()> f) {
      Row r;
      r.title = title;
      r.tile = &tile;
      r.onSelect = std::move(f);
      rows.push_back(std::move(r));
      return &rows.back();
    };
    add("Play now", tiles::playNow, [this] { openMusicItem(song_, {song_}); });
    add("Play next", tiles::playNext, [this] { enqueue(true); });
    add("Add to queue", tiles::addToQueue, [this] { enqueue(false); });
    add("Start radio", tiles::startRadio, [this] { startRadio(); })->subtitle = "Songs like this one";
    // Download: saved to the card (Music/YouTube Music), to play with no connection
    std::string file = downloadedSong(song_.id);
    bool going = false;
    bool paused = false;
    for (const DownloadJob& j : downloadJobs())
      if (j.id == song_.id && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running || j.state == DownloadJob::Paused))
        going = true, paused = j.state == DownloadJob::Paused;
    if (!file.empty()) {
      add("On the card", tiles::onCard, [this, file] { playFromCard(file); })->subtitle = "Downloaded: center plays it from the card";
    } else if (going) {
      add(paused ? "Download paused" : "Downloading", tiles::downloads, [] { g_app->push(makeMusicDownloads()); })->subtitle =
          "YouTube Music \xe2\x80\xba Downloads";
    } else {
      add("Download", tiles::downloads, [this] {
        downloadSong(song_);
        g_app->pop();
      })->subtitle = "Save it to the card, to play offline";
    }
    list_.setRows(std::move(rows));
  }
  void playFromCard(const std::string& file) {
    g_player.playQueue({file}, 0);
    g_app->pop();
    openNowPlaying();
  }
  void enqueue(bool next) {
    if (g_player.active() && !queueIsStreams())
      return list_.setMessage("Online songs can't join a queue from the card.\nUse Play now to start a new one.");
    if (!g_player.active()) {
      playStreams({song_}, 0);
    } else {
      playStreamsAppend(song_, next);
    }
    g_app->pop();
  }
  void startRadio() {
    list_.setMessage("Starting radio\xe2\x80\xa6");
    std::string path = "/music/next/" + Api::escape(song_.id);
    requests_.get(path, 0, [this, path](int status, const std::string& body) {
      if (status != 200) return list_.setMessage(offlineOr("Couldn't start radio: " + httpProblem(status, path)));
      ItemsPage page;
      parseItems(body, page);
      std::vector<Item> q{song_};
      q.insert(q.end(), page.items.begin(), page.items.end());
      playStreams(q, 0);
      g_app->pop();  // this screen is gone after this line
      openNowPlaying();
    });
  }
  Item song_;
  Requests requests_;
};

// YouTube Music › Downloads: Pause downloads (or Resume), the downloaded playlists (center opens it in
// Library, hold center: update or delete it), the songs on their way (center: pause, resume, cancel),
// then those on the card (center plays them from there, from the one chosen; hold center deletes one).
// Library › Music has them too, in Music/YouTube Music. A long queue (a playlist's 500 songs) is cheap:
// the rows are made again only when one comes or goes; a percent is drawn from the job (fill).
class MusicDownloadsScreen : public MusicListScreen {
 public:
  MusicDownloadsScreen() : MusicListScreen("Downloads") { render(); }
  bool waitsForContent() const override { return false; }
  void onShow() override {
    MusicListScreen::onShow();
    render();
  }
  bool tick(int64_t now) override {
    bool mini = MusicListScreen::tick(now);
    uint32_t s = downloadsSerial();
    if (s == serial_) return mini;
    serial_ = s;
    if (key() != key_) render();
    return true;  // a percent at least: the rows on their way are drawn again
  }

 private:
  // what decides the rows (not the percents)
  static std::string key() {
    std::string k = downloadsHeld() + (downloadsGoing() ? "g" : "") + (downloadsPaused() ? "p" : "") + "|";
    for (const DownloadJob& j : downloadJobs())
      if (j.song) k += j.id + char('0' + j.state) + "|";
    for (const PlaylistDownload& p : playlistDownloads())
      k += p.id + (p.loading ? "l" : "") + std::to_string(p.total) + "/" + std::to_string(p.onCard) + "/" + std::to_string(p.going) + "/" + std::to_string(p.paused) + "|";
    return k;
  }
  void render() {
    key_ = key();
    serial_ = downloadsSerial();
    std::vector<Row> rows;
    ids_.clear();
    auto add = [&](Row r, const std::string& job = "") {
      rows.push_back(std::move(r));
      ids_.push_back(job);
    };
    auto header = [&](const char* title) {
      Row h;
      h.header = true;
      h.title = title;
      add(h);
    };
    bool going = false;
    for (const DownloadJob& j : downloadJobs()) going = going || (j.song && j.state != DownloadJob::Done);
    std::vector<PlaylistDownload> lists = playlistDownloads();
    std::vector<Item> songs = downloadedSongs();
    int sections = int(going) + int(!lists.empty()) + int(!songs.empty());
    if (downloadsGoing() || downloadsPaused()) add(downloadsPauseRow());
    if (!lists.empty()) {
      if (sections > 1) header("Playlists");
      for (const PlaylistDownload& p : lists) {
        Row r;
        r.title = p.title;
        r.tile = &tiles::playlists;
        std::string n = std::to_string(p.total) + (p.total == 1 ? " song" : " songs");
        r.subtitle = p.loading     ? std::string("Getting its songs\xe2\x80\xa6")
                     : p.going && p.paused == p.going ? "Paused \xc2\xb7 " + std::to_string(p.onCard) + " of " + n
                     : p.going     ? "Downloading \xc2\xb7 " + std::to_string(p.onCard) + " of " + n
                     : p.onCard < p.total ? std::to_string(p.onCard) + " of " + n + " on the card"
                                     : n + (p.mix ? " \xc2\xb7 a mix" : "");
        std::string name = p.name;
        bool there = p.onCard > 0 && !name.empty();
        r.onSelect = [name, there] {
          if (there) g_app->push(makePlaylist(name));
          else g_app->toast("Its songs are on their way");
        };
        PlaylistDownload copy = p;
        r.onLongSelect = [copy] { openPlaylistDownloadMenu(copy); };
        add(r);
      }
    }
    if (going) {
      if (sections > 1) header("On the way");
      for (const DownloadJob& j : downloadJobs()) {
        if (!j.song || j.state == DownloadJob::Done) continue;
        Row r;
        r.title = j.title;
        r.thumb = j.thumb;
        std::string id = j.id;
        r.onSelect = [id] {
          for (const DownloadJob& d : downloadJobs())
            if (d.id == id) return openDownloadMenu(d);
        };
        add(r, j.id);
      }
    }
    if (!songs.empty()) {
      if (sections > 1) header("On the card");
      auto files = std::make_shared<std::vector<std::string>>();
      for (const Item& it : songs) files->push_back(it.id);
      for (size_t k = 0; k < songs.size(); k++) {
        const Item& it = songs[k];
        Row r;
        r.title = it.title, r.subtitle = it.subtitle, r.thumb = it.thumb;
        r.trailing = it.durationSec > 0 ? formatTime(it.durationSec) : "";
        int at = int(k);
        r.onSelect = [files, at] {
          g_player.playQueue(*files, at);
          openNowPlaying();
        };
        std::string file = it.id;
        r.onLongSelect = [file] {
          g_app->push(makeChoice("Delete download", {{"Keep it", "keep"}, {"Delete it", "delete"}}, [] { return std::string("keep"); },
                                 [file](const std::string& v) {
                                   if (v == "delete" && deleteSong(file)) g_app->toast("Deleted from the card");
                                 }));
        };
        add(r);
      }
    }
    // the rows on their way say what they're doing now, as they're drawn
    list_.fill = [this](int i, Row& r) {
      if (i < 0 || i >= int(ids_.size()) || ids_[size_t(i)].empty()) return;
      for (const DownloadJob& j : downloadJobs())
        if (j.id == ids_[size_t(i)]) r.subtitle = downloadRowState(j, "");
    };
    if (rows.empty()) list_.setMessage("Nothing downloaded yet: hold center on a song and choose Download, or open a playlist and choose Download playlist");
    else list_.setRows(std::move(rows), true);
  }
  // a downloaded playlist's menu: update it, or delete it (its songs too, or not)
  static void openPlaylistDownloadMenu(const PlaylistDownload& p) {
    std::string id = p.id, title = p.title;
    g_app->push(makeChoice(p.title, {{"Update it", "update"}, {"Delete it, keep its songs", "keep"}, {"Delete it and its songs", "songs"}, {"Leave it", "leave"}},
                           [] { return std::string("leave"); },
                           [id, title](const std::string& v) {
                             if (v == "update") downloadPlaylist(id, title);
                             else if ((v == "keep" || v == "songs") && deletePlaylistDownload(id, v == "songs"))
                               g_app->toast(v == "songs" ? "Deleted with its songs" : "Deleted: its songs stay on the card");
                           },
                           [](const std::string& v) {
                             // short: they sit right of the choice
                             return std::string(v == "update" ? "Gets its new songs" : v == "songs" ? "Keeps ones used elsewhere" : "");
                           }));
  }
  std::string key_;
  uint32_t serial_ = 0;
  std::vector<std::string> ids_;  // each row's download on its way ("" none)
};

// CollectionScreen: a playlist (pages of 100 appended as you scroll) or an album
class CollectionScreen : public MusicListScreen {
 public:
  CollectionScreen(std::string id, std::string title, bool album) : MusicListScreen(std::move(title)), id_(std::move(id)), album_(album) {
    list_.onNearEnd = [this] { more(); };
    load();
  }

 private:
  std::string path() const { return (album_ ? "/music/album/" : "/music/playlist/") + Api::escape(id_); }
  void load() {
    list_.setMessage("Loading\xe2\x80\xa6");
    std::string p = path();
    requests_.get(p, 120000, [this, p](int status, const std::string& body) {
      if (status != 200) return list_.setMessage(offlineOr("Couldn't load: " + httpProblem(status, p)));
      ItemsPage page;  // CollectionPage: title, subtitle, thumb, items, next
      parseItems(body, page);
      songs_ = page.items;
      next_ = page.next;
      std::vector<Row> rows;
      if (songs_.size() > 1) {
        Row all;
        all.title = "Shuffle all";
        all.tile = &tiles::shuffle;  // the row beside covers: a tile, not an empty square (as local playlists)
        all.subtitle = std::to_string(songs_.size()) + (next_.empty() ? " songs" : "+ songs");
        all.onSelect = [this] {
          std::vector<Item> s = songs_;
          std::shuffle(s.begin(), s.end(), std::mt19937(std::random_device{}()));
          playStreams(s, 0);
          openNowPlaying();
        };
        rows.push_back(all);
      }
      if (!songs_.empty() && album_) {
        Row dl;
        dl.title = songs_.size() > 1 ? "Download all" : "Download";
        dl.tile = &tiles::downloads;
        dl.subtitle = "To the card, to play offline";
        dl.onSelect = [this] { downloadSongs(songs_); };
        rows.push_back(dl);
      } else if (!songs_.empty()) {
        downloadAt_ = int(rows.size());
        rows.push_back(downloadRow());
      }
      for (Row& r : musicRows(page.items, [this] { return songs_; })) rows.push_back(std::move(r));
      list_.setRows(std::move(rows));
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
      ItemsPage page;
      parseItems(body, page);
      next_ = page.next;
      songs_.insert(songs_.end(), page.items.begin(), page.items.end());
      list_.appendRows(musicRows(page.items, [this] { return songs_; }));
    });
  }
  // a playlist's: the whole of it saved as a playlist in Library (downloads.h), or brought up to date
  Row downloadRow() {
    Row dl;
    dl.tile = &tiles::downloads;
    PlaylistDownload p;
    bool had = playlistDownload(id_, p);
    std::string id = id_, title = title_;
    if (had && (p.loading || p.going)) {
      dl.title = p.going && p.paused == p.going ? "Download paused" : "Downloading playlist";
      dl.subtitle = p.loading ? std::string("Getting its songs\xe2\x80\xa6") : std::to_string(p.onCard) + " of " + std::to_string(p.total) + " songs on the card";
      dl.onSelect = [] { g_app->push(makeMusicDownloads()); };
    } else if (had) {
      dl.title = "Update download";
      dl.subtitle = std::to_string(p.onCard) + (p.onCard == 1 ? " song" : " songs") + " on the card \xc2\xb7 gets what's new";
      dl.onSelect = [this, id, title] {
        downloadPlaylist(id, title);
        refreshDownloadRow();
      };
    } else {
      dl.title = "Download playlist";
      dl.subtitle = isMixId(id_) ? "Its " + std::to_string(songs_.size()) + " songs now, as a playlist in Library"
                                 : std::string("All its songs, as a playlist in Library");
      dl.onSelect = [this, id, title] {
        downloadPlaylist(id, title);
        refreshDownloadRow();
      };
    }
    return dl;
  }
  void refreshDownloadRow() {
    if (downloadAt_ < 0 || downloadAt_ >= int(list_.rows().size())) return;
    std::vector<Row> rows = list_.rows();
    rows[size_t(downloadAt_)] = downloadRow();
    list_.setRows(std::move(rows), true);
  }
  void onShow() override {
    MusicListScreen::onShow();
    refreshDownloadRow();
  }
  bool tick(int64_t now) override {
    bool mini = MusicListScreen::tick(now);
    if (downloadAt_ < 0 || downloadsSerial() == serial_) return mini;
    serial_ = downloadsSerial();
    const std::vector<Row>& rows = list_.rows();
    if (downloadAt_ >= int(rows.size())) return mini;
    Row fresh = downloadRow();
    if (fresh.title == rows[size_t(downloadAt_)].title && fresh.subtitle == rows[size_t(downloadAt_)].subtitle) return mini;
    refreshDownloadRow();
    return true;
  }
  std::string id_;
  bool album_;
  std::vector<Item> songs_;
  std::string next_;
  bool loading_ = false;
  int downloadAt_ = -1;  // the Download playlist row's place
  uint32_t serial_ = 0;
  Requests requests_;
};

class ArtistScreen : public MusicListScreen {
 public:
  ArtistScreen(std::string id, std::string title) : MusicListScreen(std::move(title)), id_(std::move(id)) {
    list_.setMessage("Loading\xe2\x80\xa6");
    std::string p = "/music/artist/" + Api::escape(id_);
    requests_.get(p, 120000, [this, p](int status, const std::string& body) {
      if (status != 200) return list_.setMessage(offlineOr("Couldn't load: " + httpProblem(status, p)));
      SectionsPage page;
      parseSections(body, page);
      std::vector<Row> rows;
      for (const Section& s : page.sections) {
        Row h;
        h.header = true;
        h.title = s.title;
        rows.push_back(h);
        sections_.push_back(s.items);
        size_t k = sections_.size() - 1;
        for (Row& r : musicRows(s.items, [this, k] { return sections_[k]; })) rows.push_back(std::move(r));
      }
      list_.setRows(std::move(rows));
    });
  }

 private:
  std::string id_;
  std::vector<std::vector<Item>> sections_;
  Requests requests_;
};

// SearchScreen: YouTube Music songs typed on the TV keyboard. Keyboard: Menu/Play/Prev/Next are the
// arrows, the wheel walks the keys, center types. Results: the wheel picks a row, center plays it,
// Menu goes back to the keyboard.
class MusicSearchScreen : public Screen {
 public:
  explicit MusicSearchScreen(bool video) : video_(video), kb_("Search") {
    results_.style = video ? RowStyle::Video : RowStyle::Music;
    results_.onNearEnd = [this] { more(); };
  }
  std::string title() const override { return video_ ? "Search YouTube" : "Search YouTube Music"; }
  bool statusBar() const override { return false; }
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
    switch (a) {  // TvKeyboard.handle(): it takes every control; Menu on its top row leaves
      case Action::Down: kb_.nextKey(); return true;
      case Action::Up: kb_.prevKey(); return true;
      case Action::Next:
      case Action::LongNext: kb_.right(); return true;
      case Action::Prev:
      case Action::LongPrev: kb_.left(); return true;
      case Action::PlayPause: kb_.down(); return true;
      case Action::Back: return kb_.up();
      case Action::Select: {
        Keyboard::Result r = kb_.press();
        if (r == Keyboard::Result::Action) search();
        else if (r == Keyboard::Result::Typed) kb_.keepAtMost(60);
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return showing_ ? results_.step(dt) : kb_.step(dt); }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.dark);
    darkHeader(e, w, title(), video_ ? HeaderLogo::Video : HeaderLogo::Music, white, 28);
    // the bar: 28 units, 8 in from the sides, 2 under the header, 5 above what follows, radius 14
    float bx = float(e.dp(8)), by = float(e.dp(28) + e.dp(2)), bw = w - 2 * bx, bh = float(e.dp(28));
    e.c.layer(2);
    e.c.shape({bx, by, bw, bh}, e.dpf(14), p.raised);
    e.c.layer(4);
    int icon = e.dp(12);
    float ix = bx + e.dp(10);
    e.icons.draw(e.c, Icon::Search, {ix, by + float((int(bh) - icon) / 2), float(icon), float(icon)}, p.darkMuted);
    float fx = ix + icon + e.dp(8), fw = bx + bw - e.dp(10) - fx;
    Label field = label(e, showing_ ? kb_.typed() : kb_.typed() + "|", 14, false, fw);
    drawLabel(e, field, fx, by + float((int(bh) - field.h) / 2), white);
    float area = by + bh + e.dp(5);
    if (showing_) {
      results_.layout(e, w, h - area);
      results_.draw(e, 0, area);
      e.c.layer(0);
      return;
    }
    // words, not arrow glyphs (1.0.x: Android 4.2's font may not have them)
    Label hint = label(e, "Menu up \xc2\xb7 Play down \xc2\xb7 Prev left \xc2\xb7 Next right \xc2\xb7 wheel: next key \xc2\xb7 center: type", 9,
                       false, w);
    float kh = h - area - (e.dp(3) + hint.h + e.dp(4));
    e.c.layer(1);
    kb_.draw(e.c, e.text, p, {float(e.dp(5)), area, w - 2 * e.dp(5), kh}, e.dp(15), e.dp(12));
    e.c.layer(4);
    drawLabel(e, hint, centerText(0, w, hint.w), area + kh + e.dp(3), p.darkMuted);
    e.c.layer(0);
  }

 private:
  void search() {
    std::string q = kb_.typed();
    size_t a = q.find_first_not_of(' '), b = q.find_last_not_of(' ');
    if (a == std::string::npos) return;
    q = q.substr(a, b - a + 1);
    showing_ = true;
    requests_.cancelAll();
    loading_ = false;  // a page that was loading went with the cancel
    next_.clear();
    results_.setMessage("Searching \xe2\x80\x9c" + q + "\xe2\x80\x9d\xe2\x80\xa6");
    std::string path = (video_ ? "/video/search?q=" : "/music/search?type=song&q=") + Api::escape(q);
    requests_.get(path, 0, [this, q, path](int status, const std::string& body) {
      if (status != 200) return results_.setMessage(offlineOr("Search failed: " + httpProblem(status, path)));
      ItemsPage page;
      parseItems(body, page);
      found_ = page.items;
      next_ = page.next;
      if (found_.empty()) return results_.setMessage("Nothing found for \xe2\x80\x9c" + q + "\xe2\x80\x9d");
      results_.setRows(rowsFor(page.items));
    });
  }
  void more() {
    if (next_.empty() || loading_) return;
    loading_ = true;
    requests_.get(std::string(video_ ? "/video/search" : "/music/search") + "?next=" + Api::escape(next_), 0,
                  [this](int status, const std::string& body) {
      loading_ = false;
      if (status == 410) next_.clear();
      if (status != 200) return;
      ItemsPage page;
      parseItems(body, page);
      next_ = page.next;
      found_.insert(found_.end(), page.items.begin(), page.items.end());
      results_.appendRows(rowsFor(page.items));
    });
  }
  std::vector<Row> rowsFor(const std::vector<Item>& items) {
    return video_ ? videoRows(items) : musicRows(items, [this] { return found_; });
  }
  bool video_;
  Keyboard kb_;
  RowList results_;
  bool showing_ = false, loading_ = false;
  std::vector<Item> found_;
  std::string next_;
  Requests requests_;
};

}  // namespace

std::unique_ptr<Screen> makeMusicSearch() { return std::unique_ptr<Screen>(new MusicSearchScreen(false)); }
std::unique_ptr<Screen> makeVideoSearch() { return std::unique_ptr<Screen>(new MusicSearchScreen(true)); }

void openMusicContainer(const Item& item) {
  if (item.kind == "playlist") g_app->push(std::unique_ptr<Screen>(new CollectionScreen(item.id, item.title, false)));
  else if (item.kind == "album") g_app->push(std::unique_ptr<Screen>(new CollectionScreen(item.id, item.title, true)));
  else if (item.kind == "artist") g_app->push(std::unique_ptr<Screen>(new ArtistScreen(item.id, item.title)));
}

void openSongMenu(const Item& song) { g_app->push(std::unique_ptr<Screen>(new SongMenuScreen(song))); }

std::unique_ptr<Screen> makeMusicDownloads() { return std::unique_ptr<Screen>(new MusicDownloadsScreen()); }

Row downloadsPauseRow() {
  Row r;
  bool going = downloadsGoing();
  r.title = going ? "Pause downloads" : "Resume downloads";
  r.subtitle = going ? "Videos and songs: each keeps what it has" : "Each goes on where it stopped";
  r.tile = going ? &tiles::pauseAll : &tiles::resumeAll;
  r.onSelect = [going] { going ? pauseDownloads() : resumeDownloads(); };
  return r;
}

std::string downloadRowState(const DownloadJob& j, const std::string& what) {
  std::string tail = what.empty() ? "" : " \xc2\xb7 " + what;
  // the percent only while a part comes in (Getting subtitles and Saving kept the last part's 100 %)
  bool part = j.stage.empty() || j.stage.compare(0, 11, "Downloading") == 0;
  std::string pct = std::to_string(j.percent) + "%";
  switch (j.state) {
    case DownloadJob::Running: return (j.stage.empty() ? "Downloading" : j.stage) + (part ? " \xc2\xb7 " + pct : "") + tail;
    case DownloadJob::Waiting: {
      std::string held = downloadsHeld();
      return (held.empty() ? "Waiting" : held) + (j.percent > 0 ? " \xc2\xb7 " + pct : "") + tail;
    }
    case DownloadJob::Paused: return "Paused" + (j.percent > 0 ? " \xc2\xb7 " + pct : "") + tail;
    default: return "Didn't download: " + j.error;
  }
}

void openDownloadMenu(const DownloadJob& j) {
  std::string id = j.id;
  if (j.state == DownloadJob::Failed)
    g_app->push(makeChoice("Download", {{"Try again", "again"}, {"Leave it", "leave"}, {"Take it off the list", "remove"}},
                           [] { return std::string("again"); },
                           [id](const std::string& v) {
                             if (v == "again") retryDownload(id);
                             else if (v == "remove") cancelDownload(id);
                           }));
  else if (j.state == DownloadJob::Paused)
    g_app->push(makeChoice("Download", {{"Resume it", "resume"}, {"Keep it paused", "keep"}, {"Cancel it", "cancel"}}, [] { return std::string("resume"); },
                           [id](const std::string& v) {
                             if (v == "resume") resumeDownload(id);
                             else if (v == "cancel") cancelDownload(id);
                           },
                           [](const std::string& v) { return std::string(v == "resume" ? "From where it stopped" : ""); }));
  else
    g_app->push(makeChoice("Download", {{"Keep downloading", "keep"}, {"Pause it", "pause"}, {"Cancel it", "cancel"}}, [] { return std::string("keep"); },
                           [id](const std::string& v) {
                             if (v == "pause") pauseDownload(id);
                             else if (v == "cancel") cancelDownload(id);
                           },
                           [](const std::string& v) { return std::string(v == "pause" ? "It keeps what it has" : ""); }));
}

Row musicDownloadsRow() {
  Row d;
  d.title = "Downloads";
  int on = int(downloadedSongs().size()), going = 0, paused = 0;
  for (const DownloadJob& j : downloadJobs()) {
    going += j.song && (j.state == DownloadJob::Waiting || j.state == DownloadJob::Running);
    paused += j.song && j.state == DownloadJob::Paused;
  }
  std::string cardPart = on ? " \xc2\xb7 " + std::to_string(on) + " on the card" : "";
  std::string held = downloadsHeld();  // why they wait: Wi-Fi, the card
  d.subtitle = going    ? (held.empty() ? std::to_string(going) + " downloading" : held) + cardPart
               : paused ? std::to_string(paused) + " paused" + cardPart
               : on     ? std::to_string(on) + (on == 1 ? " song" : " songs") + " on the card, to play offline"
                        : "Songs and playlists saved to play offline";
  d.tile = &tiles::downloads;
  d.onSelect = [] { g_app->push(makeMusicDownloads()); };
  return d;
}

}  // namespace rondo::shell
