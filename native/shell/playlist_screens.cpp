// Playlists and the song menu (LibraryScreens.kt and TextEntryScreen.kt in 1.0.x), ported 1:1: the
// playlists on the card, one playlist, its menu, adding a song to one, the menu of a song on the card,
// the confirm question and one line of text typed on the wheel keyboard.
#include <stdio.h>

#include <algorithm>
#include <random>

#include "../ui/keyboard.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

std::string quoted(const std::string& s) { return "\xe2\x80\x9c" + s + "\xe2\x80\x9d"; }

std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void playNow(const std::vector<std::string>& paths, int start) {
  g_player.playQueue(paths, start);
  openNowPlaying();
}

// ShellListScreen with 1.0.x's render() on every show (a playlist changed behind it)
class RenderedList : public ShellListScreen {
 public:
  using ShellListScreen::ShellListScreen;
  void onShow() override { render(); }

 protected:
  virtual void render() = 0;
};

// ConfirmScreen: Are you sure? Cancel is first, so a stray center press keeps things.
class ConfirmScreen : public ShellListScreen {
 public:
  ConfirmScreen(std::string title, const std::string& question, const std::string& action, std::function<void()> onConfirm)
      : ShellListScreen(std::move(title)) {
    Row q;
    q.header = true;
    q.title = question;
    Row cancel;
    cancel.title = "Cancel";
    cancel.onSelect = [] { g_app->pop(); };
    Row act;
    act.title = action;
    act.tile = &tiles::del;
    act.onSelect = std::move(onConfirm);
    list_.setRows({q, cancel, act});
  }
};

}  // namespace

// TextEntryScreen: one line typed on the wheel keyboard, in the shell's look. onDone answers "" when it
// took the text (the screen closes, then `then` runs) or the problem to show above the keyboard, now or
// later (a server's answer: "Just a moment…" meanwhile, and the keys wait). Menu on the keyboard's top
// row leaves without saving.
class TextEntryScreen : public Screen {
 public:
  TextEntryScreen(std::string title, std::string initial, std::string action, std::string note, size_t maxLength,
                  std::function<std::string(const std::string&)> shown, TextAnswer onDone,
                  std::function<void(const std::string&)> then)
      : title_(std::move(title)), note_(std::move(note)), max_(maxLength), kb_(std::move(action), true),
        shown_(std::move(shown)), onDone_(std::move(onDone)), then_(std::move(then)) {
    kb_.setTyped(std::move(initial));
    line_ = note_;
  }
  ~TextEntryScreen() override { *alive_ = false; }
  std::string title() const override { return title_; }
  bool onAction(Action a, int steps) override {
    if (working_) return true;
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
          finish();
        } else if (r == Keyboard::Result::Typed) {
          kb_.keepAtMost(size_t(max_));
        }
        if (r == Keyboard::Result::Typed || r == Keyboard::Result::Delete) line_ = note_, problem_ = false;
        return true;
      }
      default: return false;
    }
  }
  bool step(float dt) override { return kb_.step(dt); }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    // the field: 26 units tall, 8 in from the sides and 5 from the top, rounded 8 with a 1 px edge
    float bx = float(e.dp(8)), by = float(e.dp(5)), bw = w - 2 * bx, bh = float(e.dp(26));
    e.c.shape({bx, by, bw, bh}, e.dpf(8), p.rowLine);
    e.c.shape({bx + 1, by + 1, bw - 2, bh - 2}, e.dpf(8) - 1, p.header);
    e.c.layer(3);
    float fx = bx + e.dp(10), fw = bw - 2 * e.dp(10);
    Label field = label(e, (shown_ ? shown_(kb_.typed()) : kb_.typed()) + "|", 14, false, fw);
    drawLabel(e, field, fx, by + float((int(bh) - field.h) / 2), p.text);
    // the note, or the problem in the accent color
    float sy = by + bh + e.dp(3);
    Label msg = label(e, line_, 10, false, w - 2 * e.dp(8));
    if (msg.h == 0 || line_.empty()) msg.h = label(e, " ", 10, false).h;
    drawLabel(e, msg, centerText(float(e.dp(8)), w - 2 * e.dp(8), msg.w), sy, problem_ && !working_ ? p.accent : p.muted);
    float area = sy + msg.h + e.dp(3);
    Label hint = label(e, "wheel: next key \xc2\xb7 center: type \xc2\xb7 Menu up \xc2\xb7 Play down \xc2\xb7 Prev/Next left/right", 9,
                       false, w);
    float kh = h - area - (e.dp(3) + hint.h + e.dp(4));
    e.c.layer(1);
    kb_.draw(e.c, e.text, p, {float(e.dp(5)), area, w - 2 * e.dp(5), kh}, e.dp(15), e.dp(12));
    e.c.layer(4);
    drawLabel(e, hint, centerText(0, w, hint.w), area + kh + e.dp(3), p.muted);
  }

 private:
  void finish() {
    std::string text = trimmed(kb_.typed());
    working_ = true;
    line_ = "Just a moment\xe2\x80\xa6";
    std::shared_ptr<bool> alive = alive_;
    // nothing of this screen is touched after onDone_: its answer may have closed it
    onDone_(text, [this, alive, text](const std::string& problem) {
      if (!*alive) return;
      working_ = false;
      if (problem.empty()) {
        auto then = then_;
        g_app->pop();  // this screen is gone after this line
        if (then) then(text);
        return;
      }
      line_ = problem;
      problem_ = true;
    });
  }
  std::string title_, note_, line_;
  size_t max_;
  Keyboard kb_;
  std::function<std::string(const std::string&)> shown_;
  TextAnswer onDone_;
  std::function<void(const std::string&)> then_;
  bool problem_ = false, working_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

std::unique_ptr<Screen> makeTextEntry(std::string title, std::string initial, std::string action, size_t maxLength,
                                      std::function<std::string(const std::string&)> onDone,
                                      std::function<void(const std::string&)> then) {
  auto now = [onDone](const std::string& text, std::function<void(const std::string&)> answer) { answer(onDone(text)); };
  return std::unique_ptr<Screen>(new TextEntryScreen(std::move(title), std::move(initial), std::move(action), "", maxLength,
                                                     nullptr, now, std::move(then)));
}

std::unique_ptr<Screen> makeTextEntry(std::string title, std::string initial, std::string action, std::string note,
                                      size_t maxLength, std::function<std::string(const std::string&)> shown,
                                      TextAnswer onDone, std::function<void(const std::string&)> then) {
  return std::unique_ptr<Screen>(new TextEntryScreen(std::move(title), std::move(initial), std::move(action), std::move(note),
                                                     maxLength, std::move(shown), std::move(onDone), std::move(then)));
}

namespace {

class PlaylistScreen;
class PlaylistMenuScreen;

// AddToPlaylistScreen: pick a playlist for the songs (or start a new one with them).
class AddToPlaylistScreen : public ShellListScreen {
 public:
  explicit AddToPlaylistScreen(std::vector<std::string> paths) : ShellListScreen("Add to playlist"), paths_(std::move(paths)) {
    std::vector<Row> rows;
    Row add;
    add.title = "New playlist";
    add.tile = &tiles::add;
    auto paths2 = paths_;
    add.onSelect = [paths2] {
      g_app->push(makeTextEntry("New playlist", "", "Create", 60,
                                [paths2](const std::string& name) { return g_playlists.create(name, paths2); },
                                [](const std::string&) { g_app->pop(); }));
    };
    rows.push_back(add);
    for (const Playlist& p : g_playlists.all()) {
      bool has = std::all_of(paths_.begin(), paths_.end(),
                             [&](const std::string& s) { return std::find(p.paths.begin(), p.paths.end(), s) != p.paths.end(); });
      Row r;
      r.title = p.name;
      r.trailing = has ? "Added" : std::to_string(p.paths.size());
      r.tile = &tiles::playlists;
      r.enabled = !has;
      std::string name = p.name;
      r.onSelect = [name, paths2] {
        g_playlists.add(name, paths2);
        g_app->pop();
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
  }

 private:
  std::vector<std::string> paths_;
};

// LocalSongMenuScreen: hold center on a song on the card: play it, queue it, put it in a playlist, or
// delete it. From a playlist it also moves the song or takes it out.
class LocalSongMenuScreen : public RenderedList {
 public:
  LocalSongMenuScreen(std::string path, std::string playlist)
      : RenderedList(songInfo(path).title), path_(std::move(path)), playlist_(std::move(playlist)) {
    render();
  }

 protected:
  void render() override {
    if (!note_.empty()) return list_.setMessage(note_);
    std::vector<Row> rows;
    auto entry = [&](const char* title, std::function<void()> f) {
      Row r;
      r.title = title;
      r.onSelect = std::move(f);
      rows.push_back(std::move(r));
      return &rows.back();
    };
    std::string path = path_;
    entry("Play now", [path] { playNow({path}, 0); });
    entry("Play next", [this] { enqueue(true); });
    entry("Add to queue", [this] { enqueue(false); });
    Row* add = entry("Add to playlist", [path] { g_app->push(makeAddToPlaylist({path})); });
    add->tile = &tiles::playlists;
    add->chevron = true;
    if (!playlist_.empty()) {
      Playlist p;
      std::vector<std::string> paths;
      if (g_playlists.get(playlist_, p)) paths = p.paths;
      int at = int(std::find(paths.begin(), paths.end(), path_) - paths.begin());
      if (at >= int(paths.size())) at = -1;
      Row h;
      h.header = true;
      h.title = "In " + quoted(playlist_);
      rows.push_back(h);
      std::string name = playlist_;
      Row* up = entry("Move up", [name, paths, at] { move(name, paths, at, at - 1); });
      up->enabled = at > 0;
      Row* down = entry("Move down", [name, paths, at] { move(name, paths, at, at + 1); });
      down->enabled = at >= 0 && at < int(paths.size()) - 1;
      entry("Remove from playlist", [name, paths, path] {
        std::vector<std::string> rest;
        for (auto& s : paths)
          if (s != path) rest.push_back(s);
        g_playlists.update(name, nullptr, &rest);
        g_app->pop();
      });
    }
    std::string title = songInfo(path_).title;
    Row* del = entry("Delete from card", [this, path, title] {
      g_app->push(std::unique_ptr<Screen>(new ConfirmScreen("Delete this song?", quoted(title) + " will be removed from the card.",
                                                            "Delete", [this, path] {
                                                              bool ok = g_library.deleteFile(path);
                                                              if (ok) g_playlists.forget(path);
                                                              g_app->pop();  // the question
                                                              if (ok) g_app->pop();  // this menu
                                                              else note_ = "Couldn't delete it", render();
                                                            })));
    });
    del->subtitle = "Can't be undone";
    del->tile = &tiles::del;
    list_.setRows(std::move(rows), true);
  }

 private:
  static void move(const std::string& name, std::vector<std::string> paths, int from, int to) {
    if (from < 0 || to < 0 || to >= int(paths.size())) return;
    std::string s = paths[size_t(from)];
    paths.erase(paths.begin() + from);
    paths.insert(paths.begin() + to, s);
    g_playlists.update(name, nullptr, &paths);
    g_app->pop();
  }
  void enqueue(bool next) {
    PlayerState s = g_player.state();
    if (g_player.active() && s.index >= 0 && isStream(s.path)) {
      note_ = "Songs on the card can't join an online queue.\nUse Play now to start a new one.";
      return render();
    }
    if (!g_player.active()) g_player.playQueue({path_}, 0);
    else if (next) g_player.insertNext(path_);
    else g_player.append(path_);
    g_app->pop();
  }
  std::string path_, playlist_, note_;
};

// PlaylistMenuScreen: rename or delete a playlist.
class PlaylistMenuScreen : public ShellListScreen {
 public:
  explicit PlaylistMenuScreen(std::string name) : ShellListScreen(name) {
    Row rename;
    rename.title = "Rename";
    rename.chevron = true;
    rename.onSelect = [name] {
      g_app->push(makeTextEntry("Rename " + quoted(name), name, "Save", 60,
                                [name](const std::string& typed) { return g_playlists.update(name, &typed, nullptr); },
                                [](const std::string&) { g_app->pop(); }));
    };
    Row del;
    del.title = "Delete playlist";
    del.subtitle = "The songs stay on the card";
    del.tile = &tiles::del;
    del.onSelect = [name] {
      g_app->push(std::unique_ptr<Screen>(new ConfirmScreen("Delete " + quoted(name) + "?", "The songs stay on the card.",
                                                            "Delete playlist", [name] {
                                                              g_playlists.remove(name);
                                                              g_app->popTo("Playlists");
                                                            })));
    };
    list_.setRows({rename, del});
  }
};

// PlaylistScreen: shuffle it, add songs (search), then its songs in order. Hold center on a song to move
// or remove it.
class PlaylistScreen : public RenderedList {
 public:
  explicit PlaylistScreen(std::string name) : RenderedList(name), name_(std::move(name)) { render(); }
  void onShow() override {
    g_library.rescanIfStale();  // 1.0.x: a playlist opening
    render();
  }

 protected:
  void render() override {
    Playlist p;
    if (!g_playlists.get(name_, p)) return list_.setMessage("This playlist is gone");
    std::vector<std::string> items;
    for (auto& s : p.paths)
      if (g_library.find(s)) items.push_back(s);
    std::vector<Row> rows;
    Row shuffle;
    shuffle.title = "Shuffle";
    shuffle.trailing = std::to_string(items.size());
    shuffle.tile = &tiles::shuffle;
    shuffle.enabled = !items.empty();
    shuffle.onSelect = [items] {
      std::vector<std::string> s = items;
      std::shuffle(s.begin(), s.end(), std::mt19937(std::random_device{}()));
      playNow(s, 0);
    };
    rows.push_back(shuffle);
    Row add;
    add.title = "Add songs";
    add.tile = &tiles::add;
    add.chevron = true;
    std::string name = name_;
    add.onSelect = [name] { g_app->push(makeSearchToAdd(name)); };
    rows.push_back(add);
    Row head;
    head.header = true;
    head.title = items.empty() ? "No songs yet \xc2\xb7 Add songs, or hold center on any song"
                               : std::to_string(items.size()) + (items.size() == 1 ? " song" : " songs") +
                                     " \xc2\xb7 hold center: move or remove";
    rows.push_back(head);
    // one list shared by every row (a copy each was 821 x 821 strings), and each row's text and
    // artwork filled in when it is drawn, as Songs does
    auto shared = std::make_shared<const std::vector<std::string>>(items);
    first_ = int(rows.size());
    count_ = int(items.size());
    rows.resize(rows.size() + items.size());
    for (size_t i = 0; i < items.size(); i++) {
      Row& r = rows[size_t(first_) + i];
      r.onSelect = [shared, i] { playNow(*shared, int(i)); };
      r.onLongSelect = [shared, i, name] { g_app->push(makeSongMenu((*shared)[i], name)); };
    }
    list_.fill = [this, shared](int i, Row& r) {
      if (i < first_ || i >= first_ + count_) return;
      const std::string& path = (*shared)[size_t(i - first_)];
      const Track* t = g_library.find(path);
      SongInfo song = songInfo(path);
      r.title = song.title;
      r.subtitle = song.subtitle;
      r.trailing = t && t->durationSec >= 0 ? formatTime(t->durationSec) : "";
      r.thumb = path;
    };
    size_t missing = p.paths.size() - items.size();
    if (missing > 0) {
      Row m;
      m.title = std::to_string(missing) + (missing == 1 ? " song isn't on the card" : " songs aren't on the card");
      m.subtitle = "Copy them back, or they're skipped";
      m.enabled = false;
      rows.push_back(m);
    }
    list_.setRows(std::move(rows), true);
  }

 private:
  std::string name_;
  int first_ = 0, count_ = 0;  // where the songs' rows are
};

// PlaylistsScreen: Library › Playlists: New playlist, then every playlist on the card. Hold center for
// its options.
class PlaylistsScreen : public RenderedList {
 public:
  PlaylistsScreen() : RenderedList("Playlists") { render(); }

 protected:
  void render() override {
    std::vector<Playlist> all = g_playlists.all();
    std::vector<Row> rows;
    Row add;
    add.title = "New playlist";
    add.tile = &tiles::add;
    add.onSelect = [] {
      g_app->push(makeTextEntry("New playlist", "", "Create", 60, [](const std::string& name) { return g_playlists.create(name); },
                                [](const std::string& name) { g_app->push(makePlaylist(Playlists::clean(name))); }));
    };
    rows.push_back(add);
    if (all.size() > 1) rows.push_back(searchRow(Scope::Playlists));
    if (all.empty()) {
      Row h;
      h.header = true;
      h.title = "No playlists yet";
      rows.push_back(h);
    }
    for (const Playlist& p : all) {
      Row r;
      r.title = p.name;
      r.trailing = std::to_string(p.paths.size());
      r.tile = &tiles::playlists;
      r.chevron = true;
      std::string name = p.name;
      r.onSelect = [name] { g_app->push(makePlaylist(name)); };
      r.onLongSelect = [name] { g_app->push(std::unique_ptr<Screen>(new PlaylistMenuScreen(name))); };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows), true);
  }
};

}  // namespace

std::unique_ptr<Screen> makePlaylists() { return std::unique_ptr<Screen>(new PlaylistsScreen()); }
std::unique_ptr<Screen> makePlaylist(const std::string& name) { return std::unique_ptr<Screen>(new PlaylistScreen(name)); }
std::unique_ptr<Screen> makePlaylistMenu(const std::string& name) { return std::unique_ptr<Screen>(new PlaylistMenuScreen(name)); }
std::unique_ptr<Screen> makeSongMenu(const std::string& path, const std::string& playlist) {
  return std::unique_ptr<Screen>(new LocalSongMenuScreen(path, playlist));
}
std::unique_ptr<Screen> makeAddToPlaylist(std::vector<std::string> paths) {
  return std::unique_ptr<Screen>(new AddToPlaylistScreen(std::move(paths)));
}

}  // namespace rondo::shell
