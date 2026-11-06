// The card library's screens (DeviceScreens.kt and LibraryScreens.kt in 1.0.x), ported 1:1.
#include <stdio.h>
#include <string.h>

#include <time.h>

#include <random>

#include "../ui/keyboard.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {

std::string subtitleOf(const Track& t) {
  std::string s = t.artist;
  if (!t.album.empty()) s += (s.empty() ? "" : " \xc2\xb7 ") + t.album;
  return s;
}


const char* scopeTitle(Scope s) {
  switch (s) {
    case Scope::All: return "Search";
    case Scope::Songs: return "Search songs";
    case Scope::Artists: return "Search artists";
    case Scope::Albums: return "Search albums";
    default: return "Search videos";
  }
}
const char* scopeHint(Scope s) {
  switch (s) {
    case Scope::All: return "Songs, artists, albums, videos and playlists";
    case Scope::Songs: return "Songs by title, artist or album";
    case Scope::Artists: return "The artists on the card";
    case Scope::Albums: return "The albums on the card";
    default: return "The videos on the card";
  }
}


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
      : LetterListScreen(std::move(title)), tracks_(std::move(tracks)), showArtist_(showArtist) {
    std::vector<Row> rows;
    if (alphabetical) rows.push_back(searchRow(Scope::Songs));
    Row head;
    head.header = true;
    char h[96];
    if (alphabetical && tracks_.size() > 30) snprintf(h, sizeof(h), "%zu songs \xc2\xb7 Next/Prev jump A\xe2\x80\x93Z", tracks_.size());
    else snprintf(h, sizeof(h), "%zu songs", tracks_.size());
    head.title = h;
    rows.push_back(head);
    first_ = int(rows.size());
    rows.resize(rows.size() + tracks_.size());  // filled in when drawn
    for (size_t k = 0; k < tracks_.size(); k++)  // Select plays them all from here
      rows[size_t(first_) + k].onSelect = [this, k] { playFrom(int(k)); };
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
    list_.setRows(std::move(rows));
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

 private:
  void playFrom(int k) {
    std::vector<std::string> paths;
    paths.reserve(tracks_.size());
    for (const Track* t : tracks_) paths.push_back(t->path);
    g_player.playQueue(paths, k);
    openNowPlaying();
  }
  std::vector<const Track*> tracks_;
  bool showArtist_;
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
      r.title = g.first;
      r.trailing = std::to_string(g.second.size());
      if (!g.second.empty()) r.thumb = g.second.front()->path;
      r.chevron = true;
      r.onSelect = [this, i] {
        g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(groups_[i].first, groups_[i].second, false, false)));
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    indexLetters();
  }

 private:
  std::vector<Library::Group> groups_;
};

class LocalVideosScreen : public ShellListScreen {
 public:
  LocalVideosScreen() : ShellListScreen("Videos") {
    std::vector<Row> rows{searchRow(Scope::Videos)};
    for (const Track* t : g_library.videos()) {
      Row r;
      r.title = t->title;
      r.subtitle = subtitleOf(*t);
      r.trailing = t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      r.thumb = t->path;
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
  }
};

std::vector<const Track*> recent() {
  std::vector<const Track*> v;
  for (const Track& t : g_library.tracks())
    if (!t.video) v.push_back(&t);
  std::stable_sort(v.begin(), v.end(), [](const Track* a, const Track* b) { return a->modified > b->modified; });
  return v;
}

// LibrarySearchScreen: a search bar, a summary line, and the TV-remote keyboard; Show opens what was
// found as a list (Menu goes back to typing).
class LibrarySearchScreen : public Screen {
 public:
  explicit LibrarySearchScreen(Scope scope) : scope_(scope), kb_("Show", true) { search(); }
  std::string title() const override { return scopeTitle(scope_); }
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
    switch (a) {  // TvKeyboard.handle()
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
        if (r == Keyboard::Result::Action) {
          if (!nothingFound()) {
            showing_ = true;
            results_.setRows(rows(), true);
          }
        } else if (r == Keyboard::Result::Typed || r == Keyboard::Result::Delete) {
          if (r == Keyboard::Result::Typed && codepoints(kb_.typed()) > 40) {  // 1.0.x keeps 40
            std::string t = kb_.typed();
            while (!t.empty() && (uint8_t(t.back()) & 0xc0) == 0x80) t.pop_back();
            if (!t.empty()) t.pop_back();
            kb_.setTyped(t);
          }
          search();
        }
        return true;
      }
      default: return false;
    }
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
    Label hint = label(e, "wheel: next key \xc2\xb7 center: type \xc2\xb7 Menu up \xc2\xb7 Play down \xc2\xb7 Prev/Next left/right", 9,
                       false, w);
    float kh = h - area - (e.dp(3) + hint.h + e.dp(4));
    e.c.layer(1);
    kb_.draw(e.c, e.text, p, {float(e.dp(5)), area, w - 2 * e.dp(5), kh}, e.dp(15), e.dp(12));
    e.c.layer(4);
    drawLabel(e, hint, centerText(0, w, hint.w), area + kh + e.dp(3), p.muted);
  }

 private:
  static size_t codepoints(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += (uint8_t(c) & 0xc0) != 0x80;
    return n;
  }
  bool nothingFound() const {
    return found_.artists.empty() && found_.albums.empty() && found_.songs.empty() && found_.videos.empty();
  }
  void search() {
    found_ = g_library.search(kb_.typed());
    if (scope_ != Scope::All) {  // cut down to the scope
      if (scope_ != Scope::Artists) found_.artists.clear();
      if (scope_ != Scope::Albums) found_.albums.clear();
      if (scope_ != Scope::Songs) found_.songs.clear();
      if (scope_ != Scope::Videos) found_.videos.clear();
    }
  }
  std::string summary() const {
    const std::string& q = kb_.typed();
    if (q.find_first_not_of(' ') == std::string::npos) return scopeHint(scope_);
    if (nothingFound()) return "Nothing on the card matches";
    std::string s;
    auto part = [&](size_t n, const char* word) {
      if (!n) return;
      if (!s.empty()) s += " \xc2\xb7 ";
      s += std::to_string(n) + " " + word + (n == 1 ? "" : "s");
    };
    part(found_.artists.size(), "artist");
    part(found_.albums.size(), "album");
    part(found_.songs.size(), "song");
    part(found_.videos.size(), "video");
    return s + (showing_ ? " \xc2\xb7 Menu: back to typing" : " \xc2\xb7 Show to see them");
  }
  Row group(const Library::Group& g) {
    Row r;
    r.title = g.first;
    r.trailing = std::to_string(g.second.size());
    if (!g.second.empty()) r.thumb = g.second.front()->path;
    r.chevron = true;
    auto tracks = g.second;
    std::string name = g.first;
    r.onSelect = [name, tracks] { g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen(name, tracks, false, true))); };
    return r;
  }
  std::vector<Row> rows() {
    std::vector<Row> rows;
    auto header = [&](const char* t) {
      Row h;
      h.header = true;
      h.title = t;
      rows.push_back(h);
    };
    if (!found_.artists.empty()) header("Artists");
    for (auto& g : found_.artists) rows.push_back(group(g));
    if (!found_.albums.empty()) header("Albums");
    for (auto& g : found_.albums) rows.push_back(group(g));
    if (!found_.songs.empty()) header("Songs");
    for (const Track* t : found_.songs) {
      Row r;
      r.title = t->title;
      r.subtitle = subtitleOf(*t);
      r.trailing = t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      r.thumb = t->path;
      rows.push_back(std::move(r));
    }
    if (!found_.videos.empty()) header("Videos");
    for (const Track* t : found_.videos) {
      Row r;
      r.title = t->title;
      r.trailing = t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      r.thumb = t->path;
      rows.push_back(std::move(r));
    }
    return rows;
  }

  Scope scope_;
  Keyboard kb_;
  Library::Found found_;
  RowList results_;
  bool showing_ = false;
};

std::unique_ptr<Screen> makeSearch(Scope s) { return std::unique_ptr<Screen>(new LibrarySearchScreen(s)); }

// DeviceScreen: the card, in the iPod's own words.
class DeviceScreen : public ShellListScreen {
 public:
  DeviceScreen() : ShellListScreen("Library") { render(); }
  void onShow() override { render(); }

 private:
  void render() {
    auto songs = g_library.songs();
    auto videos = g_library.videos();
    if (songs.empty() && videos.empty() && !scanning_) {
      list_.setMessage("No music or videos on the card.\nCopy files to the card over USB.");
      return;
    }
    auto artists = g_library.artists();
    auto albums = g_library.albums();
    std::vector<Row> rows;
    auto add = [&](const char* title, const Tile& tile, std::string trailing, bool chevron, bool enabled,
                   std::function<void()> on) {
      Row r;
      r.title = title, r.tile = &tile, r.trailing = std::move(trailing), r.chevron = chevron, r.enabled = enabled;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    add("Shuffle songs", tiles::shuffle, std::to_string(songs.size()), false, !songs.empty(), [] {
      std::vector<std::string> paths;
      for (const Track* t : g_library.songs()) paths.push_back(t->path);
      static std::mt19937 rng(uint32_t(time(nullptr)));
      std::shuffle(paths.begin(), paths.end(), rng);
      g_player.playQueue(paths, 0);
      openNowPlaying();
    });
    add("Search", tiles::search, "", true, true, [] { g_app->push(makeSearch(Scope::All)); });
    add("Playlists", tiles::playlists, "", true, true, nullptr);
    add("Songs", tiles::songs, std::to_string(songs.size()), true, !songs.empty(), [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen("Songs", g_library.songs(), true, true)));
    });
    add("Artists", tiles::artists, std::to_string(artists.size()), true, !artists.empty(), [] {
      g_app->push(std::unique_ptr<Screen>(new LocalGroupsScreen("Artists", g_library.artists(), Scope::Artists)));
    });
    add("Albums", tiles::albums, albums.empty() ? "reading tags\xe2\x80\xa6" : std::to_string(albums.size()), true,
        !albums.empty(), [] {
          g_app->push(std::unique_ptr<Screen>(new LocalGroupsScreen("Albums", g_library.albums(), Scope::Albums)));
        });
    add("Recently added", tiles::recent, "", true, !songs.empty(), [] {
      g_app->push(std::unique_ptr<Screen>(new LocalSongsScreen("Recently added", recent(), false, true)));
    });
    add("Videos", tiles::videos, std::to_string(videos.size()), true, !videos.empty(),
        [] { g_app->push(std::unique_ptr<Screen>(new LocalVideosScreen())); });
    Row rescan;
    rescan.title = "Rescan the card";
    rescan.subtitle = "After copying files to it";
    rescan.trailing = scanning_ ? "Scanning\xe2\x80\xa6" : "";
    rescan.tile = &tiles::rescan;
    rescan.onSelect = [this] {
      scanning_ = true;
      render();
      g_library.scan();
      g_library.readTags();
      g_library.save();
      scanning_ = false;
      render();
    };
    rows.push_back(std::move(rescan));
    list_.setRows(std::move(rows), true);
  }
  bool scanning_ = false;
};

std::unique_ptr<Screen> makeDevice() { return std::unique_ptr<Screen>(new DeviceScreen()); }

}  // namespace rondo::shell
