// The card library's screens (DeviceScreens.kt and LibraryScreens.kt in 1.0.x), ported 1:1.
#include <stdio.h>
#include <string.h>

#include <time.h>

#include <random>
#include <set>

#include "../core/fold.h"

#include "../ui/keyboard.h"
#include "app.h"
#include "screens.h"
#include "video_places.h"

namespace rondo::shell {

std::string subtitleOf(const Track& t) {
  std::string s = t.artist;
  if (!t.album.empty()) s += (s.empty() ? "" : " \xc2\xb7 ") + t.album;
  return s;
}


std::string scopeTitle(Scope s) {
  switch (s) {
    case Scope::All: return tr("library.search.all");
    case Scope::Songs: return tr("library.search.songs");
    case Scope::Artists: return tr("library.search.artists");
    case Scope::Albums: return tr("library.search.albums");
    case Scope::Playlists: return tr("library.search.playlists");
    default: return tr("library.search.videos");
  }
}
std::string scopeHint(Scope s) {
  switch (s) {
    case Scope::All: return tr("library.searchHint.all");
    case Scope::Songs: return tr("library.searchHint.songs");
    case Scope::Artists: return tr("library.searchHint.artists");
    case Scope::Albums: return tr("library.searchHint.albums");
    case Scope::Playlists: return tr("library.searchHint.playlists");
    default: return tr("library.searchHint.videos");
  }
}

// A group's name as a row shows it. The library names the songs without an artist "Unknown artist"
// (core/library.cpp): that stays the group's name (it opens, sorts and finds the group), shown in the
// language spoken.
std::string groupTitle(const std::string& name) { return name == "Unknown artist" ? tr("library.unknownArtist") : name; }


// the first row of a long list: search just that list
Row searchRow(Scope s) {
  Row r;
  r.title = scopeTitle(s);
  r.tile = &tiles::search;
  r.chevron = true;
  r.onSelect = [s] { g_app->push(makeSearch(s)); };
  return r;
}

class LocalSongsScreen : public LetterListScreen {
 public:
  LocalSongsScreen(std::string title, std::vector<const Track*> tracks, bool alphabetical, bool showArtist)
      : LetterListScreen(std::move(title)), tracks_(std::move(tracks)), alphabetical_(alphabetical), showArtist_(showArtist) {
    build();
  }
  // a song deleted meanwhile drops out
  void onShow() override {
    size_t before = tracks_.size();
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [](const Track* t) { return t->gone; }), tracks_.end());
    if (tracks_.size() != before) build();
  }

 private:
  void build() {
    bool alphabetical = alphabetical_;
    letters_.clear();
    std::vector<Row> rows;
    if (alphabetical) rows.push_back(searchRow(Scope::Songs));
    Row head;
    head.header = true;
    if (alphabetical && tracks_.size() > 30) head.title = tr("library.songsJump", {{"count", tracks_.size()}});
    else head.title = tr("library.songsHeader", {{"count", tracks_.size()}});
    rows.push_back(head);
    if (tracks_.size() > 1) {
      Row sh;
      sh.title = tr("library.shuffle");
      sh.tile = &tiles::shuffle;
      sh.onSelect = [this] { shuffleAll(); };
      rows.push_back(sh);
    }
    first_ = int(rows.size());
    rows.resize(rows.size() + tracks_.size());  // filled in when drawn
    for (size_t k = 0; k < tracks_.size(); k++) {  // Select plays them all from here; holding it, the song's menu
      rows[size_t(first_) + k].onSelect = [this, k] { playFrom(int(k)); };
      rows[size_t(first_) + k].onLongSelect = [this, k] { g_app->push(makeSongMenu(tracks_[k]->path)); };
    }
    list_.fill = [this](int i, Row& r) {
      if (i < first_) return;
      const Track* t = tracks_[size_t(i - first_)];
      r.title = t->title;
      std::string sub = subtitleOf(*t);
      if (!showArtist_) {  // substringAfter(" · ", ""): the album only
        size_t at = sub.find(" \xc2\xb7 ");
        sub = at == std::string::npos ? "" : sub.substr(at + 4);
      }
      r.subtitle = sub;
      r.trailing = t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      r.thumb = t->path;
    };
    list_.setRows(std::move(rows), true);
    if (alphabetical) {  // the letters from the titles themselves (the rows are empty until drawn)
      std::vector<uint32_t> seen;
      for (size_t k = 0; k < tracks_.size(); k++) {
        if (tracks_[k]->title.empty()) continue;
        uint32_t c = firstLetter(tracks_[k]->title);
        if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
        seen.push_back(c);
        letters_.push_back({first_ + int(k), c});
      }
    }
  }

  void playFrom(int k) {
    std::vector<std::string> paths;
    paths.reserve(tracks_.size());
    for (const Track* t : tracks_) paths.push_back(t->path);
    g_player.playQueue(paths, k);
    openNowPlaying();
  }
  void shuffleAll() {
    std::vector<std::string> paths;
    for (const Track* t : tracks_) paths.push_back(t->path);
    static std::mt19937 rng(uint32_t(time(nullptr)));
    std::shuffle(paths.begin(), paths.end(), rng);
    g_player.playQueue(paths, 0);
    openNowPlaying();
  }
  std::vector<const Track*> tracks_;
  bool alphabetical_, showArtist_;
  int first_ = 0;
};

class LocalGroupsScreen : public LetterListScreen {
 public:
  LocalGroupsScreen(std::string title, std::vector<Library::Group> groups, Scope scope) : LetterListScreen(std::move(title)) {
    std::vector<Row> rows{searchRow(scope)};
    groups_ = std::move(groups);
    for (size_t i = 0; i < groups_.size(); i++) {
      const auto& g = groups_[i];
      Row r;
      r.title = groupTitle(g.first);
      if (scope == Scope::Albums && !g.second.empty()) {  // its album artist (else artist) and year
        const Track* t = g.second.front();
        std::string by = !t->albumArtist.empty() ? t->albumArtist : t->artist;
        int year = Library::yearOf(g.second);
        r.subtitle = by + (year ? (by.empty() ? "" : " \xc2\xb7 ") + std::to_string(year) : "");
      }
      r.trailing = std::to_string(g.second.size());
      if (!g.second.empty()) r.thumb = g.second.front()->path;
      r.chevron = true;
      r.onSelect = [this, i] {
        g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(groupTitle(groups_[i].first), groups_[i].second, false, false)));
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    indexLetters();
  }

 private:
  std::vector<Library::Group> groups_;
};

// a video on the card as the player takes it (LocalLibrary.item in 1.0.x): its path is its id
Item cardVideo(const Track& t) {
  Item i;
  i.kind = "video";
  i.id = t.path, i.title = t.title, i.subtitle = subtitleOf(t), i.thumb = t.path;
  i.durationSec = t.durationSec;
  return i;
}

// a started card video's options (hold center on its row): Resume, Start again
class CardVideoMenuScreen : public ShellListScreen {
 public:
  explicit CardVideoMenuScreen(Item video) : ShellListScreen(video.title) {
    std::vector<Row> rows;
    std::string key = videoPlaceKey(video);
    Row resume;
    resume.title = tr("book.resume"), resume.tile = &tiles::resume, resume.subtitle = videoLeftText(key, video.durationSec);
    resume.onSelect = [video] {
      g_app->pop();
      openVideo(video);
    };
    rows.push_back(resume);
    Row again;
    again.title = tr("book.startAgain"), again.tile = &tiles::startAgain;
    again.onSelect = [video, key] {
      videoStartAgain(key);
      g_app->pop();
      openVideo(video);
    };
    rows.push_back(again);
    list_.setRows(std::move(rows));
  }
};

void cardVideoHold(const Item& item) {
  if (videoHasPlace(videoPlaceKey(item))) g_app->push(std::unique_ptr<Screen>(new CardVideoMenuScreen(item)));
}

// a card video's row: its length, or how much is left once it's started (video_places.h)
Row cardVideoRow(const Track& t) {
  Row r;
  r.title = t.title;
  r.subtitle = subtitleOf(t);
  Item item = cardVideo(t);
  std::string left = videoLeftText(videoPlaceKey(item), t.durationSec);
  r.trailing = !left.empty() ? left : t.durationSec >= 0 ? formatTime(t.durationSec) : "";
  r.thumb = t.path;
  r.onSelect = [item] { openVideo(item); };
  r.onLongSelect = [item] { cardVideoHold(item); };
  return r;
}

// a list of card videos (all of them, the newest, one folder's, Continue watching's); what's left of
// each is said again on coming back from one
class LocalVideosScreen : public ShellListScreen {
 public:
  LocalVideosScreen(std::string title, std::vector<const Track*> videos, bool search, bool continuing = false)
      : ShellListScreen(std::move(title)), videos_(std::move(videos)), search_(search), continuing_(continuing) {
    render(false);
  }
  void onShow() override { render(true); }

 private:
  void render(bool keep) {
    if (continuing_) {  // the last watched first; one watched to the end leaves
      videos_.clear();
      for (const Item& i : continueWatchingCard())
        if (const Track* t = g_library.find(i.id)) videos_.push_back(t);
    }
    std::vector<Row> rows;
    if (search_) rows.push_back(searchRow(Scope::Videos));
    for (const Track* t : videos_) rows.push_back(cardVideoRow(*t));
    if (rows.empty()) list_.setMessage(tr(continuing_ ? "video.continueNone" : "library.noVideosHere"));
    else list_.setRows(std::move(rows), keep);
  }
  std::vector<const Track*> videos_;
  bool search_, continuing_;
};

// Library › Videos: Continue watching (once one is started), all of them, the newest first, and each
// folder that has some
class VideosSectionScreen : public ShellListScreen {
 public:
  VideosSectionScreen() : ShellListScreen(tr("library.videos")) { render(false); }
  void onShow() override { render(true); }

 private:
  void render(bool keep) {
    std::vector<const Track*> all = g_library.videos();
    if (all.empty()) {
      list_.setMessage(tr("library.noVideos"));
      return;
    }
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile* tile, const std::string& thumb, std::string trailing, std::function<void()> on) {
      Row r;
      r.title = title, r.tile = tile, r.thumb = thumb, r.trailing = std::move(trailing), r.chevron = true;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    size_t started = continueWatchingCard().size();
    if (started)
      add(tr("video.continueWatching"), &tiles::continueWatching, "", std::to_string(started), [] {
        g_app->push(std::unique_ptr<Screen>(new LocalVideosScreen(tr("video.continueWatching"), {}, false, true)));
      });
    add(tr("library.allVideos"), &tiles::videos, "", std::to_string(all.size()), [all] {
      g_app->push(std::unique_ptr<Screen>(new LocalVideosScreen(tr("library.allVideos"), all, true)));
    });
    std::vector<const Track*> newest = all;
    std::stable_sort(newest.begin(), newest.end(), [](const Track* a, const Track* b) { return a->modified > b->modified; });
    add(tr("library.recentlyAdded"), &tiles::recent, "", "", [newest] {
      g_app->push(std::unique_ptr<Screen>(new LocalVideosScreen(tr("library.recentlyAdded"), newest, false)));
    });
    std::map<std::string, std::vector<const Track*>> byFolder;
    for (const Track* t : all) byFolder[t->path.substr(0, t->path.rfind('/'))].push_back(t);
    if (byFolder.size() > 1) {
      Row h;
      h.header = true;
      h.title = tr("library.folders");
      rows.push_back(h);
      for (auto& kv : byFolder) {
        std::string name = kv.first.substr(kv.first.rfind('/') + 1);
        std::vector<const Track*> v = kv.second;
        add(name, nullptr, v.front()->path, std::to_string(v.size()),
            [name, v] { g_app->push(std::unique_ptr<Screen>(new LocalVideosScreen(name, v, false))); });
      }
    }
    list_.setRows(std::move(rows), keep);
  }
};

std::vector<const Track*> recent() {
  std::vector<const Track*> v;
  for (const Track& t : g_library.tracks())
    if (!t.video && !t.gone && !t.book) v.push_back(&t);
  std::stable_sort(v.begin(), v.end(), [](const Track* a, const Track* b) { return a->modified > b->modified; });
  return v;
}

// LibrarySearchScreen: a search bar, a summary line, and the TV-remote keyboard; Show opens what was
// found as a list (Menu goes back to typing). With `addTo`, it picks songs for that playlist instead:
// center adds the song (or a whole artist or album).
class LibrarySearchScreen : public Screen {
 public:
  explicit LibrarySearchScreen(Scope scope, std::string addTo = "") : scope_(scope), addTo_(std::move(addTo)), kb_(tr("library.show"), Keyboard::Style::Shell) {
    onShow();
  }
  std::string title() const override { return addTo_.empty() ? scopeTitle(scope_) : tr("library.addTo", {{"playlist", addTo_}}); }
  void onShow() override {
    g_library.rescanIfStale();  // 1.0.x: Library's search opening
    allPlaylists_.clear();
    if (addTo_.empty() && (scope_ == Scope::All || scope_ == Scope::Playlists)) allPlaylists_ = g_playlists.all();
    search();
    if (showing_ && nothingFound()) showing_ = false;
    else if (showing_) results_.setRows(rows(), true);
  }
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
    Keyboard::Result r;  // TvKeyboard.handle()
    if (!keyboardAction(kb_, a, steps, &r)) return false;
    if (r == Keyboard::Result::Action) {
      if (!nothingFound()) {
        showing_ = true;
        results_.setRows(rows(), true);
      }
    } else if (r == Keyboard::Result::Typed || r == Keyboard::Result::Delete) {
      if (r == Keyboard::Result::Typed) kb_.keepAtMost(40);  // 1.0.x keeps 40
      search();
    }
    return true;
  }
  bool step(float dt) override { return showing_ ? results_.step(dt) : kb_.step(dt); }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    // the bar: 26 units tall, 8 in from the sides and 5 from the top, rounded 8 with a 1 px edge
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
    // the summary, centered, 3 units above and below
    float sy = by + bh + e.dp(3);
    Label sum = label(e, summary(), 10, false, w - 2 * e.dp(8));
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
  bool nothingFound() const {
    return found_.artists.empty() && found_.albums.empty() && found_.songs.empty() && found_.videos.empty() &&
           found_.books.empty() && playlists_.empty();
  }
  void search() {
    problem_.clear();
    found_ = g_library.search(kb_.typed());
    // playlists match on their names, every word
    playlists_.clear();
    std::vector<std::string> words;
    std::string q = fold(kb_.typed()), w;
    for (char c : q + " ") {
      if (c == ' ') {
        if (!w.empty()) words.push_back(w);
        w.clear();
      } else {
        w += c;
      }
    }
    if (!words.empty())
      for (const Playlist& p : allPlaylists_) {
        std::string n = fold(p.title());
        if (std::all_of(words.begin(), words.end(), [&](const std::string& x) { return n.find(x) != std::string::npos; }))
          playlists_.push_back(p);
      }
    if (scope_ != Scope::All) {  // cut down to the scope
      if (scope_ != Scope::Artists) found_.artists.clear();
      if (scope_ != Scope::Albums) found_.albums.clear();
      if (scope_ != Scope::Songs) found_.songs.clear();
      if (scope_ != Scope::Videos) found_.videos.clear();
      found_.books.clear();  // audiobooks in the whole library's search only
    }
    if (!addTo_.empty()) found_.books.clear();
  }
  std::string summary() const {
    if (!problem_.empty()) return problem_;
    const std::string& q = kb_.typed();
    if (q.find_first_not_of(' ') == std::string::npos) return addTo_.empty() ? scopeHint(scope_) : tr("library.searchToAdd");
    if (nothingFound()) return tr("library.nothingMatches");
    std::string s;
    auto part = [&](size_t n, const char* key) {
      if (!n) return;
      if (!s.empty()) s += " \xc2\xb7 ";
      s += tr(key, {{"count", n}});
    };
    part(found_.artists.size(), "library.found.artists");
    part(found_.albums.size(), "library.found.albums");
    part(found_.songs.size(), "library.found.songs");
    if (addTo_.empty()) {
      part(found_.videos.size(), "library.found.videos");
      part(found_.books.size(), "library.found.books");
      part(playlists_.size(), "library.found.playlists");
    }
    return s + " \xc2\xb7 " + (showing_ ? tr("library.found.backToTyping") : tr("library.found.showToSee"));
  }
  Row group(const Library::Group& g) {
    Row r;
    r.title = groupTitle(g.first);
    std::vector<std::string> paths;
    for (const Track* t : g.second) paths.push_back(t->path);
    bool all = !addTo_.empty() && std::all_of(paths.begin(), paths.end(), [&](const std::string& p) { return added_.count(p); });
    r.trailing = all ? tr("library.added") : !addTo_.empty() ? tr("library.addCount", {{"count", g.second.size()}}) : std::to_string(g.second.size());
    if (!g.second.empty()) r.thumb = g.second.front()->path;
    r.chevron = addTo_.empty();
    auto tracks = g.second;
    std::string name = groupTitle(g.first);
    if (!addTo_.empty()) r.onSelect = [this, paths] { add(paths); };
    else r.onSelect = [name, tracks] { g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(name, tracks, false, true))); };
    return r;
  }
  void add(const std::vector<std::string>& paths) {
    std::string problem = g_playlists.add(addTo_, paths);
    if (problem.empty()) added_.insert(paths.begin(), paths.end());
    problem_ = problem;  // shown in the summary line until the search changes (1.0.x)
    results_.setRows(rows(), true);
  }
  std::vector<Row> rows() {
    std::vector<Row> rows;
    auto header = [&](const std::string& t) {
      Row h;
      h.header = true;
      h.title = t;
      rows.push_back(h);
    };
    if (!found_.artists.empty()) header(tr("library.artists"));
    for (auto& g : found_.artists) rows.push_back(group(g));
    if (!found_.albums.empty()) header(tr("library.albums"));
    for (auto& g : found_.albums) rows.push_back(group(g));
    if (!found_.songs.empty()) header(tr("library.songs"));
    // one list for every song row to play from (a copy each grew with the square of the matches)
    auto songs = std::make_shared<std::vector<std::string>>();
    for (const Track* t : found_.songs) songs->push_back(t->path);
    for (size_t i = 0; i < found_.songs.size(); i++) {
      const Track* t = found_.songs[i];
      Row r;
      r.title = t->title;
      r.subtitle = subtitleOf(*t);
      r.trailing = !addTo_.empty() && added_.count(t->path) ? tr("library.added") : t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      r.thumb = t->path;
      std::string path = t->path;
      if (!addTo_.empty()) {
        r.onSelect = [this, path] { add({path}); };
      } else {
        r.onSelect = [songs, i] {
          g_player.playQueue(*songs, int(i));
          openNowPlaying();
        };
        r.onLongSelect = [path] { g_app->push(makeSongMenu(path)); };
      }
      rows.push_back(std::move(r));
    }
    if (addTo_.empty()) {  // videos aren't added to playlists
      if (!found_.videos.empty()) header(tr("library.videos"));
      for (const Track* t : found_.videos) {
        Row r = cardVideoRow(*t);
        r.subtitle.clear();  // a search's rows: the title alone
        rows.push_back(std::move(r));
      }
      if (!found_.books.empty()) header(tr("books.title"));
      for (const Book* b : found_.books) {  // a book's screen (Resume, Chapters…)
        Row r;
        r.title = b->title;
        r.subtitle = b->author;
        r.thumb = b->cover;
        r.chevron = true;
        std::string key = b->key;
        r.onSelect = [key] { g_app->push(makeBook(key)); };
        rows.push_back(std::move(r));
      }
    }
    if (!playlists_.empty()) header(tr("common.playlists"));
    for (const Playlist& p : playlists_) {
      Row r;
      r.title = p.title();
      if (p.youtube) r.subtitle = "YouTube Music";
      r.trailing = std::to_string(p.paths.size());
      r.tile = p.cover.empty() ? &tiles::playlists : nullptr;
      r.thumb = p.cover;
      r.chevron = true;
      std::string name = p.name;
      r.onSelect = [name] { g_app->push(makePlaylist(name)); };
      r.onLongSelect = [name] { g_app->push(makePlaylistMenu(name)); };
      rows.push_back(std::move(r));
    }
    return rows;
  }

  Scope scope_;
  std::string addTo_;
  Keyboard kb_;
  Library::Found found_;
  std::vector<Playlist> allPlaylists_, playlists_;  // the ones matching (not while adding to one)
  std::set<std::string> added_;
  std::string problem_;  // the last add that failed
  RowList results_;
  bool showing_ = false;
};

std::unique_ptr<Screen> makeSearch(Scope s) { return std::unique_ptr<Screen>(new LibrarySearchScreen(s)); }
std::unique_ptr<Screen> makeSearchToAdd(const std::string& playlist) {
  return std::unique_ptr<Screen>(new LibrarySearchScreen(Scope::All, playlist));
}

// Library › Music: the songs every way, in groups (Browse, Playlists, Listening, the card); the folders
// themselves are Library › Files'
class MusicSectionScreen : public ShellListScreen {
 public:
  MusicSectionScreen() : ShellListScreen(tr("library.music")) { render(); }

 private:
  void render() {
    // the counts only (copying the groups cost 3 ms of the opening)
    size_t songs = g_library.songCount();
    if (!songs && !scanning_) {
      list_.setMessage(tr("library.noMusic"));
      return;
    }
    size_t artists = g_library.artistCount(), albums = g_library.albumCount();
    std::vector<Row> rows;
    auto header = [&](const std::string& title) {
      Row h;
      h.header = true;
      h.title = title;
      rows.push_back(h);
    };
    auto add = [&](const std::string& title, const Tile& tile, std::string trailing, bool chevron, bool enabled, std::function<void()> on) {
      Row r;
      r.title = title, r.tile = &tile, r.trailing = std::move(trailing), r.chevron = chevron, r.enabled = enabled;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    add(tr("library.shuffleSongs"), tiles::shuffle, std::to_string(songs), false, songs != 0, [] {
      std::vector<std::string> paths;
      for (const Track* t : g_library.songs()) paths.push_back(t->path);
      static std::mt19937 rng(uint32_t(time(nullptr)));
      std::shuffle(paths.begin(), paths.end(), rng);
      g_player.playQueue(paths, 0);
      openNowPlaying();
    });
    add(tr("library.search.all"), tiles::search, "", true, true, [] { g_app->push(makeSearch(Scope::All)); });
    header(tr("library.browse"));
    add(tr("library.songs"), tiles::songs, std::to_string(songs), true, songs != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(tr("library.songs"), g_library.songs(), true, true)));
    });
    add(tr("library.artists"), tiles::artists, std::to_string(artists), true, artists != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalGroupsScreen(tr("library.artists"), g_library.artists(), Scope::Artists)));
    });
    size_t albumArtists = g_library.albumArtistCount(), genres = g_library.genreCount();
    add(tr("library.albumArtists"), tiles::albumArtists, std::to_string(albumArtists), true, albumArtists != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalGroupsScreen(tr("library.albumArtists"), g_library.albumArtists(), Scope::Artists)));
    });
    add(tr("library.albums"), tiles::albums, albums == 0 ? tr("library.readingTags") : std::to_string(albums), true, albums != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalGroupsScreen(tr("library.albums"), g_library.albums(), Scope::Albums)));
    });
    add("Cover Flow", tiles::coverFlow, "", true, albums != 0, [] { g_app->push(makeCoverFlow()); });
    add(tr("library.genres"), tiles::genres, std::to_string(genres), true, genres != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalGroupsScreen(tr("library.genres"), g_library.genres(), Scope::Songs)));
    });
    header(tr("common.playlists"));
    size_t playlists = g_playlists.all().size();
    add(tr("common.playlists"), tiles::playlists, playlists ? std::to_string(playlists) : "", true, true, [] { g_app->push(makePlaylists()); });
    header(tr("library.listening"));
    size_t most = g_library.mostPlayed(1).size(), rated = g_library.topRated().size();
    add(tr("library.recentlyAdded"), tiles::recent, "", true, songs != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(tr("library.recentlyAdded"), recent(), false, true)));
    });
    add(tr("library.mostPlayed"), tiles::mostPlayed, "", true, most != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(tr("library.mostPlayed"), g_library.mostPlayed(), false, true)));
    });
    add(tr("library.recentlyPlayed"), tiles::played, "", true, most != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(tr("library.recentlyPlayed"), g_library.recentlyPlayed(), false, true)));
    });
    add(tr("library.topRated"), tiles::topRated, rated ? std::to_string(rated) : "", true, rated != 0, [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(tr("library.topRated"), g_library.topRated(), false, true)));
    });
    header(tr("library.theCard"));
    Row rescan;
    rescan.title = tr("library.rescan");
    rescan.subtitle = tr("library.rescanNote");
    rescan.trailing = g_library.scanning() ? tr("library.scanning") : "";
    rescan.tile = &tiles::rescan;
    rescan.onSelect = [this] {
      if (g_library.scanning()) return;
      g_library.scanAsync();  // on the library's thread: the screen stays free
      scanning_ = true;
      render();
    };
    rows.push_back(std::move(rescan));
    Row organize;
    organize.title = tr("organize.title");
    organize.subtitle = tr("library.organizeNote");
    organize.tile = &tiles::organize;
    organize.chevron = true;
    organize.enabled = songs + g_library.videoCount() != 0;
    organize.onSelect = [] { g_app->push(makeOrganize()); };
    rows.push_back(std::move(organize));
    list_.setRows(std::move(rows), true);
  }
  // the card read again when Music opens and the last scan is over 30 s old (DeviceScreens.kt)
  void onShow() override {
    g_library.rescanIfStale();
    scanning_ = g_library.scanning();
    render();
  }
  bool tick(int64_t) override {
    if (scanning_ == g_library.scanning() && generation_ == g_library.generation()) return false;
    scanning_ = g_library.scanning();
    generation_ = g_library.generation();
    render();  // "Scanning…" and the counts
    return true;
  }
  int64_t wakeAt() const override { return scanning_ ? now_ns() + 100 * kMs : 0; }
  bool scanning_ = false;
  uint64_t generation_ = 0;
};

// DeviceScreen: Library, in five: Music, Videos, Audiobooks, Photos and the files themselves
class DeviceScreen : public ShellListScreen {
 public:
  DeviceScreen() : ShellListScreen(tr("common.library")) { render(); }

 private:
  void render() {
    size_t songs = g_library.songCount(), videos = g_library.videoCount();
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile& tile, std::string subtitle, std::string trailing, std::function<void()> on) {
      Row r;
      r.title = title, r.tile = &tile, r.subtitle = std::move(subtitle), r.trailing = std::move(trailing), r.chevron = true;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    add(tr("library.music"), tiles::musicSection, tr("library.musicNote"), songs ? std::to_string(songs) : "",
        [] { g_app->push(makeLibraryMusic()); });
    add(tr("library.videos"), tiles::videos, "", videos ? std::to_string(videos) : "",
        [] { g_app->push(std::unique_ptr<Screen>(new VideosSectionScreen())); });
    size_t books = g_library.bookCount();
    add(tr("books.title"), tiles::audiobooks, "", books ? std::to_string(books) : "", [] { g_app->push(makeAudiobooks()); });
    add(tr("photos.title"), tiles::photos, "", "", [] { g_app->push(makePhotos()); });
    add(tr("files.title"), tiles::files, tr("library.filesNote"), "", [] { g_app->push(makeFiles()); });
    list_.setRows(std::move(rows), true);
  }
  void onShow() override {
    g_library.rescanIfStale();
    render();
  }
  bool tick(int64_t) override {
    if (generation_ == g_library.generation()) return false;
    generation_ = g_library.generation();
    render();  // the counts
    return true;
  }
  uint64_t generation_ = 0;
};

std::unique_ptr<Screen> makeDevice() { return std::unique_ptr<Screen>(new DeviceScreen()); }
std::unique_ptr<Screen> makeLibraryMusic() { return std::unique_ptr<Screen>(new MusicSectionScreen()); }

}  // namespace rondo::shell

namespace rondo::shell {

// From the loop (every 250 ms or so): a card song counts as played once it's half over or 4 minutes
// in, once each time it starts (what Most and Recently played go by)
void playStatsTick() {
  static uint32_t counted = 0, serial = 0;
  static std::string path;
  PlayerState s = g_player.state();
  if (s.trackSerial != serial) {
    serial = s.trackSerial;
    path = s.path;
  }
  if (counted == serial || !s.playing || s.video || path.empty() || isStream(path)) return;
  int64_t need = s.durationMs > 0 ? std::min<int64_t>(s.durationMs / 2, 240000) : 240000;
  if (s.positionMs < need) return;
  counted = serial;
  g_library.notePlayed(path);
}

std::unique_ptr<Screen> makeSongList(const std::string& title, std::vector<const Track*> songs) {
  return std::unique_ptr<Screen>(new LocalSongsScreen(title, std::move(songs), false, true));
}

}  // namespace rondo::shell
