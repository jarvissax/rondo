// Playlists and the song menu (LibraryScreens.kt and TextEntryScreen.kt in 1.0.x), ported 1:1: the
// playlists on the card, one playlist, its menu, adding a song to one, the menu of a song on the card,
// the confirm question and one line of text typed on the wheel keyboard.
#include <stdio.h>

#include <algorithm>
#include <random>

#include "../ui/keyboard.h"
#include "app.h"
#include "downloads.h"
#include "screens.h"

namespace rondo::shell {
namespace {

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
    cancel.title = tr("common.cancel");
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
                  std::function<void(const std::string&)> then, bool emoji)
      : title_(std::move(title)), note_(std::move(note)), max_(maxLength), kb_(std::move(action), Keyboard::Style::Shell, emoji),
        shown_(std::move(shown)), onDone_(std::move(onDone)), then_(std::move(then)) {
    kb_.setTyped(std::move(initial));
    line_ = note_;
  }
  ~TextEntryScreen() override { *alive_ = false; }
  std::string title() const override { return title_; }
  bool onAction(Action a, int steps) override {
    if (working_) return true;
    Keyboard::Result r;  // TvKeyboard.handle()
    if (!keyboardAction(kb_, a, steps, &r)) return false;
    if (r == Keyboard::Result::Action) {
      finish();
    } else if (r == Keyboard::Result::Typed) {
      kb_.keepAtMost(size_t(max_));
    }
    if (r == Keyboard::Result::Typed || r == Keyboard::Result::Delete) line_ = note_, problem_ = false;
    return true;
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
    drawKeyboard(e, kb_, area, w, h, tr("library.keyboardHint"), p.muted, p.text);
  }

 private:
  void finish() {
    std::string text = trimmed(kb_.typed());
    working_ = true;
    line_ = tr("playlists.justAMoment");
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
                                      std::function<void(const std::string&)> then, bool emoji) {
  auto now = [onDone](const std::string& text, std::function<void(const std::string&)> answer) { answer(onDone(text)); };
  return std::unique_ptr<Screen>(new TextEntryScreen(std::move(title), std::move(initial), std::move(action), "", maxLength,
                                                     nullptr, now, std::move(then), emoji));
}

std::unique_ptr<Screen> makeTextEntry(std::string title, std::string initial, std::string action, std::string note,
                                      size_t maxLength, std::function<std::string(const std::string&)> shown,
                                      TextAnswer onDone, std::function<void(const std::string&)> then, bool emoji) {
  return std::unique_ptr<Screen>(new TextEntryScreen(std::move(title), std::move(initial), std::move(action), std::move(note),
                                                     maxLength, std::move(shown), std::move(onDone), std::move(then), emoji));
}

namespace {

class PlaylistScreen;
class PlaylistMenuScreen;

// AddToPlaylistScreen: pick a playlist for the songs (or start a new one with them).
class AddToPlaylistScreen : public ShellListScreen {
 public:
  explicit AddToPlaylistScreen(std::vector<std::string> paths) : ShellListScreen(tr("playlists.addTo")), paths_(std::move(paths)) {
    std::vector<Row> rows;
    Row add;
    add.title = tr("playlists.new");
    add.tile = &tiles::add;
    auto paths2 = paths_;
    add.onSelect = [paths2] {
      g_app->push(makeTextEntry(tr("playlists.new"), "", tr("playlists.create"), 60,
                                [paths2](const std::string& name) { return g_playlists.create(name, paths2); },
                                [](const std::string&) { g_app->pop(); }));
    };
    rows.push_back(add);
    for (const Playlist& p : g_playlists.all()) {
      if (p.youtube) continue;  // a downloaded one follows YouTube's
      bool has = std::all_of(paths_.begin(), paths_.end(),
                             [&](const std::string& s) { return std::find(p.paths.begin(), p.paths.end(), s) != p.paths.end(); });
      Row r;
      r.title = p.name;
      r.trailing = has ? tr("library.added") : std::to_string(p.paths.size());
      r.tile = p.cover.empty() ? &tiles::playlists : nullptr;
      r.thumb = p.cover;
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
    auto entry = [&](const std::string& title, std::function<void()> f) {
      Row r;
      r.title = title;
      r.onSelect = std::move(f);
      rows.push_back(std::move(r));
      return &rows.back();
    };
    std::string path = path_;
    entry(tr("playlists.song.playNow"), [path] { playNow({path}, 0); });
    entry(tr("playlists.song.playNext"), [this] { enqueue(true); });
    entry(tr("playlists.song.addToQueue"), [this] { enqueue(false); });
    Row* add = entry(tr("playlists.addTo"), [path] { g_app->push(makeAddToPlaylist({path})); });
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
      h.title = tr("playlists.song.inPlaylist", {{"name", playlist_}});
      rows.push_back(h);
      std::string name = playlist_;
      Row* up = entry(tr("playlists.song.moveUp"), [name, paths, at] { move(name, paths, at, at - 1); });
      up->enabled = at > 0;
      Row* down = entry(tr("playlists.song.moveDown"), [name, paths, at] { move(name, paths, at, at + 1); });
      down->enabled = at >= 0 && at < int(paths.size()) - 1;
      entry(tr("playlists.song.remove"), [name, paths, path] {
        std::vector<std::string> rest;
        for (auto& s : paths)
          if (s != path) rest.push_back(s);
        g_playlists.update(name, nullptr, &rest);
        g_app->pop();
      });
    }
    std::string title = songInfo(path_).title;
    Row* del = entry(tr("playlists.song.delete"), [this, path, title] {
      g_app->push(std::unique_ptr<Screen>(new ConfirmScreen(tr("playlists.song.deleteQuestion"),
                                                            tr("playlists.song.deleteNote", {{"title", title}}), tr("common.delete"),
                                                            [this, path] {
                                                              bool ok = g_library.deleteFile(path);
                                                              if (ok) g_playlists.forget(path);
                                                              g_app->pop();  // the question
                                                              if (ok) g_app->pop();  // this menu
                                                              else note_ = tr("playlists.song.cantDelete"), render();
                                                            })));
    });
    del->subtitle = tr("playlists.song.cantUndo");
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
      note_ = tr("playlists.song.notOnline");
      return render();
    }
    if (!g_player.active()) g_player.playQueue({path_}, 0);
    else if (next) g_player.insertNext(path_);
    else g_player.append(path_);
    g_app->pop();
  }
  std::string path_, playlist_, note_;
};

// PlaylistMenuScreen: rename or delete a playlist. A downloaded YouTube Music one (downloads.h) isn't
// renamed or edited here, it follows YouTube's: update it, or delete it with its songs or without.
class PlaylistMenuScreen : public ShellListScreen {
 public:
  explicit PlaylistMenuScreen(std::string name) : ShellListScreen(Playlist{name, {}, isYouTube(name)}.title()) {
    if (isYouTube(name)) {
      youtube(name);
      return;
    }
    Row rename;
    rename.title = tr("common.rename");
    rename.chevron = true;
    rename.onSelect = [name] {
      g_app->push(makeTextEntry(tr("playlists.renameTitle", {{"name", name}}), name, tr("common.save"), 60,
                                [name](const std::string& typed) { return g_playlists.update(name, &typed, nullptr); },
                                [](const std::string&) { g_app->pop(); }));
    };
    Row del;
    del.title = tr("playlists.delete");
    del.subtitle = tr("playlists.songsStay");
    del.tile = &tiles::del;
    del.onSelect = [name] {
      g_app->push(std::unique_ptr<Screen>(new ConfirmScreen(tr("playlists.deleteQuestion", {{"name", name}}), tr("playlists.songsStayNote"),
                                                            tr("playlists.delete"), [name] {
                                                              g_playlists.remove(name);
                                                              g_app->popTo(tr("common.playlists"));  // PlaylistsScreen's title
                                                            })));
    };
    list_.setRows({rename, del});
  }

 private:
  static bool isYouTube(const std::string& name) {
    std::string prefix = std::string(Playlists::kYouTube) + "/";
    return name.compare(0, prefix.size(), prefix) == 0;
  }
  void youtube(const std::string& name) {
    std::string title = Playlist{name, {}, true}.title(), id = playlistDownloadOf(name);
    std::vector<Row> rows;
    if (!id.empty()) {
      Row up;
      up.title = tr("playlists.updateDownload");
      up.subtitle = tr("playlists.updateDownloadNote");
      up.tile = &tiles::downloads;
      up.onSelect = [id, title] {
        downloadPlaylist(id, title);
        g_app->pop();
      };
      rows.push_back(up);
    }
    Row keep;
    keep.title = tr("playlists.delete");
    keep.subtitle = tr("playlists.itsSongsStay");
    keep.tile = &tiles::del;
    keep.onSelect = [name, title] {
      g_app->push(std::unique_ptr<Screen>(new ConfirmScreen(tr("playlists.deleteQuestion", {{"name", title}}), tr("playlists.itsSongsStayNote"),
                                                            tr("playlists.delete"), [name] {
                                                              deleteLibraryPlaylist(name, false);
                                                              g_app->popTo(tr("common.playlists"));
                                                            })));
    };
    rows.push_back(keep);
    Row all;
    all.title = tr("playlists.deleteWithSongs");
    all.subtitle = tr("playlists.deleteWithSongsNote");
    all.onSelect = [name, title] {
      g_app->push(std::unique_ptr<Screen>(new ConfirmScreen(tr("playlists.deleteWithSongsQuestion", {{"name", title}}),
                                                            tr("playlists.deleteWithSongsWarning"), tr("playlists.deleteThem"), [name] {
                                                              deleteLibraryPlaylist(name, true);
                                                              g_app->popTo(tr("common.playlists"));
                                                            })));
    };
    rows.push_back(all);
    list_.setRows(std::move(rows));
  }
};

// PlaylistScreen: shuffle it, add songs (search), then its songs in order. Hold center on a song to move
// or remove it.
class PlaylistScreen : public RenderedList {
 public:
  explicit PlaylistScreen(std::string name) : RenderedList(name), name_(std::move(name)) {
    std::string prefix = std::string(Playlists::kYouTube) + "/";
    youtube_ = name_.compare(0, prefix.size(), prefix) == 0;
    if (youtube_) title_ = name_.substr(prefix.size());
    render();
  }
  void onShow() override {
    g_library.rescanIfStale();  // 1.0.x: a playlist opening
    render();
  }

 protected:
  void render() override {
    Playlist p;
    if (!g_playlists.get(name_, p)) return list_.setMessage(tr("playlists.thisIsGone"));
    std::vector<std::string> items;
    for (auto& s : p.paths)
      if (g_library.find(s)) items.push_back(s);
    std::vector<Row> rows;
    Row shuffle;
    shuffle.title = tr("library.shuffle");
    shuffle.trailing = std::to_string(items.size());
    shuffle.tile = &tiles::shuffle;
    shuffle.enabled = !items.empty();
    shuffle.onSelect = [items] {
      std::vector<std::string> s = items;
      std::shuffle(s.begin(), s.end(), std::mt19937(std::random_device{}()));
      playNow(s, 0);
    };
    rows.push_back(shuffle);
    std::string name = name_;
    if (!youtube_) {  // a downloaded one follows YouTube's: not edited here
      Row add;
      add.title = tr("playlists.addSongs");
      add.tile = &tiles::add;
      add.chevron = true;
      add.onSelect = [name] { g_app->push(makeSearchToAdd(name)); };
      rows.push_back(add);
    }
    Row head;
    head.header = true;
    std::string count = tr("playlists.songCount", {{"count", items.size()}});
    head.title = youtube_       ? (items.empty() ? tr("playlists.noSongsOnCard") : count + " \xc2\xb7 " + tr("playlists.fromYouTubeMusic"))
                 : items.empty() ? tr("playlists.noSongsYet")
                                 : count + " \xc2\xb7 " + tr("playlists.holdToMove");
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
      std::string in = youtube_ ? "" : name;  // a downloaded one's songs aren't moved or taken out here
      r.onLongSelect = [shared, i, in] { g_app->push(makeSongMenu((*shared)[i], in)); };
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
      m.title = tr("playlists.missing", {{"count", missing}});
      m.subtitle = youtube_ ? tr("playlists.missingYouTube") : tr("playlists.missingCopy");
      m.enabled = false;
      rows.push_back(m);
    }
    list_.setRows(std::move(rows), true);
  }

 private:
  std::string name_;
  bool youtube_ = false;  // a downloaded YouTube Music playlist
  int first_ = 0, count_ = 0;  // where the songs' rows are
};

// PlaylistsScreen: Library › Playlists: New playlist, then every playlist on the card. Hold center for
// its options.
class PlaylistsScreen : public RenderedList {
 public:
  PlaylistsScreen() : RenderedList(tr("common.playlists")) { render(); }  // what popTo() looks for

 protected:
  void render() override {
    std::vector<Playlist> all = g_playlists.all();
    std::vector<Row> rows;
    Row add;
    add.title = tr("playlists.new");
    add.tile = &tiles::add;
    add.onSelect = [] {
      g_app->push(makeTextEntry(tr("playlists.new"), "", tr("playlists.create"), 60, [](const std::string& name) { return g_playlists.create(name); },
                                [](const std::string& name) { g_app->push(makePlaylist(Playlists::clean(name))); }));
    };
    rows.push_back(add);
    if (all.size() > 1) rows.push_back(searchRow(Scope::Playlists));
    if (all.empty()) {
      Row h;
      h.header = true;
      h.title = tr("playlists.noneYet");
      rows.push_back(h);
    }
    for (const Playlist& p : all) {
      Row r;
      r.title = p.title();
      if (p.youtube) r.subtitle = "YouTube Music";
      r.trailing = std::to_string(p.paths.size());
      r.tile = p.cover.empty() ? &tiles::playlists : nullptr;  // its picture when it has one (a downloaded one's cover)
      r.thumb = p.cover;
      r.chevron = true;
      std::string name = p.name;
      r.onSelect = [name] { g_app->push(makePlaylist(name)); };
      r.onLongSelect = [name] { g_app->push(std::unique_ptr<Screen>(new PlaylistMenuScreen(name))); };
      rows.push_back(std::move(r));
    }
    // smart playlists (made on the dashboard): their songs worked out as they open
    std::vector<SmartPlaylist> smart = g_smart.all();
    if (!smart.empty()) {
      Row h;
      h.header = true;
      h.title = tr("playlists.smart");
      rows.push_back(h);
    }
    for (const SmartPlaylist& p : smart) {
      Row r;
      r.title = p.name;
      r.subtitle = tr("playlists.smartNote");
      r.trailing = std::to_string(SmartPlaylists::songs(p, g_library).size());
      r.tile = &tiles::smart;
      r.chevron = true;
      SmartPlaylist copy = p;
      r.onSelect = [copy] { g_app->push(makeSongList(copy.name, SmartPlaylists::songs(copy, g_library))); };
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
