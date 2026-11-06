// rondo: Rondo 2.0's native shell, a 1:1 port of 1.0.x's screens (private/native-plan.md, "parity
// first"). Runs on the Y1 with the Java layer and SurfaceFlinger stopped.
//
// Ported so far: the shell (ShellActivity: the screen stack, the status bar), the main menu with its
// preview pane (MainMenuScreen.kt), and the card library (DeviceScreens.kt: Library, Songs, Artists,
// Albums, Recently added, Videos). Everything else is a row that does nothing yet.
//
// What 2.0 adds is motion: screens slide like the iPod's (1.0.x replaces them), the list and its
// highlight glide to where 1.0.x puts them, the preview pane cross-fades.
//
//   rondo [--config path] [--library db] [--fonts dir] [--regular ttf] [--thumbs dir] [--csv path]
//         [--restore-android 1] [--shot path]
//
// SIGUSR1 saves a screenshot (--shot, default /data/local/tmp/rondo/shot.png). Holding Menu 2 s quits
// (a development affordance); with --restore-android 1, 1.0.x then comes back.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/library.h"
#include "../../platform/platform.h"
#include "../../shell/dispatch.h"
#include "../../shell/kit.h"
#include "../../ui/frame_log.h"
#include "../../ui/keyboard.h"

using namespace rondo;
using namespace rondo::ui;
using namespace rondo::shell;

namespace {

volatile sig_atomic_t g_stop = 0, g_shot = 0;
Config g_config;
Library g_library;

class App;

class Screen {
 public:
  virtual ~Screen() = default;
  virtual std::string title() const = 0;
  // An action the screen uses (true), or leaves to the shell's global meaning.
  virtual bool onAction(Action a, int steps) { return false; }
  virtual bool step(float dt) { return false; }
  // The content under the status bar, at the canvas's current offset.
  virtual void draw(Env& e, float w, float h) = 0;
  virtual void onShow() {}
};

// ShellActivity: one stack of screens; the main menu is its bottom and never closes.
class App {
 public:
  void push(std::unique_ptr<Screen> s) {
    if (s) {
      finishTransition();
      stack_.push_back(std::move(s));
      stack_.back()->onShow();
      transition_.begin(0, 1, now_, 250 * kMs);
      popping_ = false;
    }
  }
  void pop() {
    if (stack_.size() < 2) return;
    finishTransition();
    leaving_ = std::move(stack_.back());
    stack_.pop_back();
    stack_.back()->onShow();
    transition_.begin(0, 1, now_, 250 * kMs);
    popping_ = true;
  }
  void action(Action a, int steps) {
    if (stack_.back()->onAction(a, steps)) return;
    if (a == Action::Back) pop();
    // PlayPause, Next/Prev and the seeks go to the player (phase N3)
  }
  void setNow(int64_t now) { now_ = now; }
  bool step(float dt) {
    bool moving = false;
    for (auto& s : stack_) moving |= s->step(dt);
    if (leaving_) moving |= leaving_->step(dt);
    if (transition_.running(now_)) moving = true;
    else leaving_.reset();
    return moving;
  }
  void draw(Env& e, float W, float H, const Status& status) {
    float bar = float(e.dp(22));
    Screen* top = stack_.back().get();
    Screen* other = popping_ ? leaving_.get() : (stack_.size() > 1 ? stack_[stack_.size() - 2].get() : nullptr);
    bool sliding = transition_.running(now_) && other;
    float p = sliding ? transition_.at(now_) : 1;
    auto content = [&](Screen* s, float dx) {
      e.c.pushOffset(dx, bar);
      e.c.pushClip({0, 0, W, H - bar});
      s->draw(e, W, H - bar);
      e.c.popClip();
      e.c.popOffset();
    };
    if (sliding) {
      // push: the new screen comes in from the right and pushes the old one out; pop reverses it
      float off = popping_ ? W * p : -W * p;
      content(other, roundf(off));
      content(top, roundf(popping_ ? off - W : off + W));
    } else {
      content(top, 0);
    }
    e.c.layer(6);
    statusBar(e, W, status, sliding ? other->title() : top->title(), sliding ? top->title() : "", sliding ? p : 0);
    e.c.layer(0);
  }

 private:
  // A frame skipped (see the loop): what started moving starts one refresh later instead of jumping.
 public:
  void delay(int64_t by) { transition_.start += by; }

 private:
  void finishTransition() {
    transition_.duration = 0;
    leaving_.reset();
  }
  std::vector<std::unique_ptr<Screen>> stack_;
  std::unique_ptr<Screen> leaving_;
  Tween transition_;
  bool popping_ = false;
  int64_t now_ = 0;
};

App* g_app = nullptr;

// ShellListScreen: the status bar's title and a RowList 3 units below it.
class ShellListScreen : public Screen {
 public:
  explicit ShellListScreen(std::string title) : title_(std::move(title)) {}
  std::string title() const override { return title_; }
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
  void draw(Env& e, float w, float h) override {
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, e.p.shellBg);
    float pad = float(e.dp(3));
    list_.layout(e, w, h - pad);
    list_.draw(e, 0, pad);
  }

 protected:
  std::string title_;
  RowList list_;
};

// LetterListScreen: Next/Prev jump to the next or previous first letter, like the iPod's letter overlay.
class LetterListScreen : public ShellListScreen {
 public:
  using ShellListScreen::ShellListScreen;
  bool onAction(Action a, int steps) override {
    if (letters_.size() > 1 && (a == Action::Next || a == Action::Prev)) {
      int at = list_.selected();
      int target = -1;
      if (a == Action::Next) {
        for (auto& l : letters_)
          if (l.first > at) {
            target = l.first;
            break;
          }
      } else {
        for (auto& l : letters_)
          if (l.first < at) target = l.first;
      }
      if (target >= 0) list_.selectIndex(target);
      return true;
    }
    return ShellListScreen::onAction(a, steps);
  }

 protected:
  // the row index of each letter's first row (rows that aren't the search row); others go to '#'
  void indexLetters() {
    letters_.clear();
    std::vector<uint32_t> seen;
    const auto& rows = list_.rows();
    for (int i = 0; i < int(rows.size()); i++) {
      const Row& r = rows[i];
      if (r.header || r.tile == &tiles::search || r.title.empty()) continue;
      uint32_t c = firstLetter(r.title);
      if (std::find(seen.begin(), seen.end(), c) != seen.end()) continue;
      seen.push_back(c);
      letters_.push_back({i, c});
    }
  }
  static uint32_t firstLetter(const std::string& s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return isalpha(c) ? uint32_t(toupper(c)) : '#';
    // a letter outside ASCII: its UTF-8 lead bytes are enough to tell letters apart
    uint32_t cp = c;
    for (size_t i = 1; i < s.size() && i < 4 && ((unsigned char)s[i] & 0xc0) == 0x80; i++) cp = cp << 8 | (unsigned char)s[i];
    return cp;
  }
  std::vector<std::pair<int, uint32_t>> letters_;
};

std::string subtitleOf(const Track& t) {
  std::string s = t.artist;
  if (!t.album.empty()) s += (s.empty() ? "" : " \xc2\xb7 ") + t.album;
  return s;
}

enum class Scope { All, Songs, Artists, Albums, Videos };
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
std::unique_ptr<Screen> makeSearch(Scope s);

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
    rows.resize(rows.size() + tracks_.size());  // filled in when drawn (Select plays the queue: phase N3)
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
    add("Shuffle songs", tiles::shuffle, std::to_string(songs.size()), false, !songs.empty(), nullptr);
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

// MainMenuScreen: the menu on the left (190 units), a preview of the selected entry on the right.
class MainMenuScreen : public Screen {
 public:
  MainMenuScreen() {
    struct E {
      const char* id;
      const char* label;
      const Tile* tile;
    };
    static const E kEntries[] = {{"music", "YouTube Music", &tiles::music}, {"videos", "YouTube", &tiles::video},
                                 {"device", "Library", &tiles::device},     {"nowPlaying", "Now Playing", &tiles::nowPlaying},
                                 {"radio", "FM Radio", &tiles::radio},      {"settings", "Settings", &tiles::settings}};
    std::vector<Row> rows;
    for (const MenuEntry& m : g_config.theme.menu) {
      if (!m.visible) continue;
      for (const E& e : kEntries) {
        if (m.id != e.id) continue;
        Row r;
        r.title = e.label;
        r.tile = e.tile;
        r.chevron = true;
        if (m.id == "device") r.onSelect = [] { g_app->push(std::unique_ptr<Screen>(new DeviceScreen())); };
        rows.push_back(std::move(r));
      }
    }
    list_.onSelectionChanged = [this] {
      const Row* r = list_.selectedRow();
      if (!r) return;
      if (shown_ != r->tile) {
        from_ = shown_;
        shown_ = r->tile;
        fade_.begin(0, 1, now_, 180 * kMs, easeInOutCubic);
      }
    };
    list_.setRows(std::move(rows));
    from_ = shown_;
  }
  std::string title() const override { return "Rondo"; }
  bool onAction(Action a, int steps) override {
    switch (a) {
      case Action::Down: return list_.down(steps);
      case Action::Up: return list_.up(steps);
      case Action::Select: return list_.select();
      case Action::Back: return true;  // the launcher: nothing below it
      default: return false;
    }
  }
  bool step(float dt) override {
    bool a = list_.step(dt);
    return a || fade_.running(now_);
  }
  void draw(Env& e, float w, float h) override {
    now_ = e.now;
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    float lw = float(e.dp(190)), pad = float(e.dp(3));
    list_.layout(e, lw, h - pad);
    list_.draw(e, 0, pad);
    e.c.layer(0);
    e.c.fill({lw, 0, 1, h}, p.barLine);
    Rect pane{lw + 1, 0, w - lw - 1, h};
    float t = fade_.at(e.now);
    if (from_ && t < 1) drawPane(e, pane, from_, 1);
    if (shown_) {
      e.c.pushAlpha(from_ && t < 1 ? t : 1);
      drawPane(e, pane, shown_, 1);
      e.c.popAlpha();
    }
  }

 private:
  // renderPane() for an entry that isn't playing: its tile's colors corner to corner, its glyph on a
  // translucent rounded square (the logos alone), and its name
  void drawPane(Env& e, Rect pane, const Tile* tile, float) {
    auto darker = [](Color c, float f) {
      return rgb(uint32_t(((c >> 16) & 0xff) * f) << 16 | uint32_t(((c >> 8) & 0xff) * f) << 8 | uint32_t((c & 0xff) * f));
    };
    e.c.layer(0);
    fillDiagonal(e.c, pane, tile->top, tile->bottom, darker(tile->bottom, 0.7f));
    int padX = e.dp(10), padY = e.dp(8);
    float ix = pane.x + padX, iy = pane.y + padY, iw = pane.w - 2 * padX, ih = pane.h - 2 * padY;
    int box = e.dp(98);
    const char* name = tile == &tiles::music       ? "YouTube Music"
                       : tile == &tiles::video      ? "YouTube"
                       : tile == &tiles::device     ? "Library"
                       : tile == &tiles::nowPlaying ? "Nothing playing"
                       : tile == &tiles::radio      ? "FM Radio"
                                                    : "Settings";
    Label title = label(e, name, 14, true, iw);
    int total = box + e.dp(9) + title.h;
    float top = iy + float((int(ih) - total) / 2);
    float bx = ix + float((int(iw) - box) / 2);
    Rect vis{bx, top, float(box), float(box)};
    e.c.layer(2);
    if (!tile->brand) {
      e.c.shape(vis, e.dpf(22), 0x2dffffff);
      int g = e.dp(70);
      e.c.layer(4);
      e.icons.draw(e.c, tile->kind, {bx + float((box - g) / 2), top + float((box - g) / 2), float(g), float(g)}, rgb(0xffffff));
    } else {
      e.c.layer(4);
      int gw = tile->brand == 1 ? e.dp(80) : e.dp(70), gh = tile->brand == 1 ? e.dp(56) : e.dp(70);
      Rect lr{bx + float((box - gw) / 2), top + float((box - gh) / 2), float(gw), float(gh)};
      if (tile->brand == 1) e.icons.videoLogo(e.c, lr);
      else e.icons.musicLogo(e.c, lr);
    }
    e.c.layer(4);
    drawLabel(e, title, centerText(ix, iw, title.w), top + box + e.dp(9), rgb(0xffffff));
  }

  RowList list_;
  const Tile* shown_ = nullptr;
  const Tile* from_ = nullptr;
  Tween fade_;
  int64_t now_ = 0;
};

float readBattery() {
  int v = 100;
  if (FILE* f = fopen("/sys/class/power_supply/battery/capacity", "r")) {
    if (fscanf(f, "%d", &v) != 1) v = 100;
    fclose(f);
  }
  return std::clamp(v / 100.0f, 0.0f, 1.0f);
}

// online: a default route exists (ConnectivityManager's answer, without Java)
bool readOnline() {
  FILE* f = fopen("/proc/net/route", "r");
  if (!f) return false;
  char line[256];
  bool online = false;
  while (fgets(line, sizeof(line), f)) {
    char iface[32];
    unsigned dest = 1;
    if (sscanf(line, "%31s %x", iface, &dest) == 2 && dest == 0) online = true;
  }
  fclose(f);
  return online;
}

void onSignal(int sig) {
  if (sig == SIGUSR1) g_shot = 1;
  else g_stop = 1;
}

}  // namespace

int main(int argc, char** argv) {
  const char* configPath = "/data/data/com.jarvissax.rondo/files/config.json";
  const char* libraryDb = "/data/local/tmp/rondo/library.db";
  const char* indexJson = "/data/data/com.jarvissax.rondo/files/local-index.json";
  const char* fonts = "/system/fonts";
  const char* regular = "/data/local/tmp/rondo/fonts/Roboto-Regular.ttf";
  const char* thumbs = "/data/local/tmp/rondo/thumbs";
  const char* shotPath = "/data/local/tmp/rondo/shot.png";
  const char* advances = "/data/local/tmp/rondo/advances.bin";
  const char* csv = nullptr;
  bool restoreAndroid = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--config")) configPath = argv[i + 1];
    else if (!strcmp(argv[i], "--library")) libraryDb = argv[i + 1];
    else if (!strcmp(argv[i], "--fonts")) fonts = argv[i + 1];
    else if (!strcmp(argv[i], "--regular")) regular = argv[i + 1];
    else if (!strcmp(argv[i], "--thumbs")) thumbs = argv[i + 1];
    else if (!strcmp(argv[i], "--shot")) shotPath = argv[i + 1];
    else if (!strcmp(argv[i], "--csv")) csv = argv[i + 1];
    else if (!strcmp(argv[i], "--restore-android")) restoreAndroid = atoi(argv[i + 1]) != 0;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGUSR1, onSignal);
  signal(SIGHUP, SIG_IGN);
  setpriority(PRIO_PROCESS, 0, -10);
  if (FILE* pf = fopen("/data/local/tmp/rondo/rondo.pid", "w")) {
    fprintf(pf, "%d\n", getpid());
    fclose(pf);
  }
  // first, while this is the only thread: it forks the decoding process
  ImageLoader loader;
  if (!loader.start(2, thumbs)) return 1;
  int64_t t0 = now_ns();
  g_config = loadConfig(configPath);
  Palette pal = paletteFor(g_config.theme);
  if (g_library.open(libraryDb) && g_library.tracks().empty()) g_library.importIndexJson(indexJson);
  g_library.scan();
  g_library.readTags();
  g_library.save();
  g_library.artists();  // builds the groups now, not when Library opens
  int64_t tLib = now_ns();

  auto display = openDisplay();
  if (!display) return 1;
  auto input = openInput();
  if (!input) return 1;
  Canvas canvas;
  if (!canvas.init(display->width(), display->height())) return 1;
  Text text;
  if (!text.init(fonts, access(regular, R_OK) == 0 ? regular : nullptr)) return 1;
  if (!text.loadAdvances(advances)) fprintf(stderr, "rondo: no advance table at %s: text widths will differ from 1.0.x\n", advances);
  text.loadKerning("/data/local/tmp/rondo/kerning.bin");
  Icons icons;
  icons.init(text);
  Covers covers(loader);
  covers.reserve(int(floorf(26 * pal.scale + 0.5f)), int(floorf(26 * pal.scale + 0.5f)), 96);
  {  // the list thumbnails, decoded once into the cache while nothing moves
    int t = int(floorf(26 * pal.scale + 0.5f));
    for (const Track& tr : g_library.tracks()) covers.prefetch(tr.path, t, t, 0);
  }
  App app;
  g_app = &app;
  app.push(std::unique_ptr<Screen>(new MainMenuScreen()));
  Dispatcher dispatch;
  dispatch.acceleration = g_config.behavior.wheelAcceleration;
  dispatch.emit = [&](Action a, int steps) { app.action(a, steps); };
  Status status{false, readOnline(), readBattery()};
  printf("rondo: %zu files in the library (%.0f ms), ready at %.0f ms\n", g_library.tracks().size(), (tLib - t0) / 1e6,
         (now_ns() - t0) / 1e6);

  FrameLog log(display->period());
  const int64_t period = display->period();
  bool moving = true, wasMoving = false;
  int64_t lastPresent = 0, lastStatus = now_ns(), menuDown = 0;
  // Glyphs and icons are rasterized while idle: adding to the atlas while a frame still uses it makes the
  // next frame wait a whole refresh for the GPU (N5 log), so a screen's first frame must find them ready.
  std::vector<int> warmFonts;
  for (float sp : {15.f, 13.5f, 11.f, 12.f, 14.f, 10.f, 9.f})
    for (bool bold : {false, true}) warmFonts.push_back(text.font(bold ? Weight::Bold : Weight::Regular, sp * pal.scale));
  for (int px : {int(floorf(15 * pal.scale + 0.5f)), int(floorf(12 * pal.scale + 0.5f))})  // the keyboard's labels
    for (bool bold : {false, true}) warmFonts.push_back(text.font(bold ? Weight::Bold : Weight::Regular, float(px)));
  static const char kWarmChars[] =
      " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
      "ÀÁÂÄÇÈÉÊÍÑÓÖÚÜ"
      "àáâãäåçèéêëíîïñóôöøúûü"
      "ĞğİıŞşƏə·–—‘’“”•…";
  bool warmed = false;
  size_t warmIcon = 0;
  auto warmIcons = [&] {  // every icon at the sizes the shell draws it, a few per idle slice
    auto d = [&](float v) { return int(floorf(v * pal.scale + 0.5f)); };
    struct I { Icon icon; int w, h; };
    static std::vector<I> list;
    if (list.empty()) {
      for (Icon k : {Icon::Note, Icon::Play, Icon::Folder, Icon::Bars, Icon::Gear, Icon::Wifi, Icon::Bluetooth, Icon::Cloud,
                     Icon::Info, Icon::Person, Icon::Disc, Icon::Screen, Icon::Sun, Icon::Timer, Icon::Radio, Icon::Volume,
                     Icon::Shuffle, Icon::Power, Icon::Refresh, Icon::Theme, Icon::Playlist, Icon::Plus, Icon::Search,
                     Icon::Trash, Icon::Shorts})
        list.push_back({k, d(16), d(16)});
      for (Icon k : {Icon::Bars, Icon::Folder, Icon::Radio, Icon::Gear}) list.push_back({k, d(70), d(70)});
      list.push_back({Icon::Chevron, d(8), d(12)});
      list.push_back({Icon::Wifi, d(14), d(14)});
      list.push_back({Icon::BatteryFrame, d(24), d(14)});
      list.push_back({Icon::Play, d(12), d(12)});
      list.push_back({Icon::Search, d(11), d(11)});
      for (Icon k : {Icon::MusicLogoDisc, Icon::MusicLogoMarks}) list.push_back({k, d(19), d(19)}), list.push_back({k, d(70), d(70)});
      for (Icon k : {Icon::VideoLogoButton, Icon::VideoLogoPlay}) list.push_back({k, d(19), d(14)}), list.push_back({k, d(80), d(56)});
    }
    int64_t until = now_ns() + 3 * kMs;
    while (warmIcon < list.size() && now_ns() < until) icons.mask(list[warmIcon].icon, list[warmIcon].w, list[warmIcon].h), warmIcon++;
    return warmIcon >= list.size();
  };
  while (!g_stop) {
    if (!moving && !g_shot) {
      pollfd p[2] = {{input->waitFd(), POLLIN, 0}, {loader.waitFd(), POLLIN, 0}};
      if (!warmed) {
        warmed = warmIcons() && text.warm(warmFonts, kWarmChars, 3 * kMs);
        text.flush();  // to the GPU now, not in a moving frame
        pollfd q[1] = {{input->waitFd(), POLLIN, 0}};
        if (poll(q, 1, 0) == 0) continue;
      }
      bool prefetching = covers.sendPrefetches();
      int64_t deadline = dispatch.nextDeadline();
      int timeout = prefetching ? 50 : 1000;
      if (deadline) timeout = std::max(0, std::min(timeout, int((deadline - now_ns()) / kMs) + 1));
      int r = poll(p, 2, timeout);
      if (r < 0 && errno == EINTR) continue;
      if (r == 0) {
        dispatch.tick(now_ns());
        if (now_ns() - lastStatus > 2 * kSec) {  // 1.0.x refreshes on its 500 ms tick
          lastStatus = now_ns();
          Status s{false, readOnline(), readBattery()};
          if (s.online != status.online || fabsf(s.battery - status.battery) > 0.001f) status = s, moving = true;
        }
        if (!moving) continue;
      }
    }
    int64_t vs = display->waitVsync();
    int64_t present = vs + period;
    app.setNow(present);
    int64_t a = now_ns();  // the frame's work starts with the input: a screen opening counts
    input->acknowledge();
    InputEvent ev;
    int64_t oldest = -1;
    while (input->poll(ev)) {
      if (oldest < 0) oldest = vs - ev.time;
      dispatch.event(ev);
      if (ev.key == Key::Menu) {
        if (ev.down) menuDown = ev.time;
        else if (menuDown && ev.time - menuDown >= 2 * kSec) g_stop = 1;
      }
    }
    dispatch.tick(now_ns());
    // Opening a screen can take 10 ms. A frame finished that late reaches the panel a refresh late and holds
    // up the next one (N5 log), so such a refresh draws nothing: the slide starts cleanly on the next.
    if (now_ns() - a > 6 * kMs) {
      app.delay(period);
      lastPresent = present;
      wasMoving = moving = true;
      continue;
    }
    float dt = wasMoving && lastPresent ? std::min(0.1f, float(present - lastPresent) / 1e9f) : float(period) / 1e9f;
    bool anim = app.step(dt);
    canvas.begin(pal.shellBg);
    Env env{canvas, text, icons, covers, pal, present};
    app.draw(env, float(display->width()), float(display->height()), status);
    int64_t b = now_ns();
    text.flush();
    bool more = covers.pump(present, 2);
    canvas.end();
    int64_t c = now_ns();
    if (g_shot) {
      g_shot = 0;
      printf("rondo: screenshot %s\n", saveScreenshot(shotPath, display->width(), display->height()) ? "saved" : "failed");
    }
    display->present();
    int64_t d = now_ns();
    FrameLog::Frame f{};
    f.vsync = vs, f.buildNs = int32_t(b - a), f.submitNs = int32_t(c - b), f.swapNs = int32_t(d - c);
    f.inputNs = int32_t(oldest), f.draws = int16_t(canvas.drawCalls()), f.quads = canvas.quads();
    f.glyphs = int16_t(text.glyphsRasterizedLastFrame()), f.continued = wasMoving, f.uploads = int16_t(covers.uploadedLast());
    log.add(f);
    text.frameDone();
    lastPresent = present;
    moving = anim || more || dispatch.nextDeadline() != 0;
    if (moving) loader.active(present);
    wasMoving = moving;
  }
  log.report(stdout, csv);
  if (restoreAndroid) {
    display.reset();
    system("start surfaceflinger; sleep 1; start zygote");
  }
  fflush(stdout);
  _exit(0);
}
