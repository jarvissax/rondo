// Library › Audiobooks (books.h): the books, "Continue listening" first; a book's own screen (Resume,
// Chapters, Bookmarks, Speed, Mark as finished); its chapters and bookmarks; the speeds; the sleep timer.
#include <stdio.h>
#include <time.h>

#include <algorithm>

#include "app.h"
#include "books.h"
#include "screens.h"

namespace rondo::shell {
namespace {

// what a row says of a book: "Author · 2 h 10 min left", "Author · Finished", "Author · 12 h 5 min"
std::string bookStatus(const Book& b, const BookPlay& d, const BookProgress& p) {
  if (p.finished) return tr("book.finished");
  if (p.started) return tr("book.left", {{"time", listenTime(std::max<int64_t>(0, d.lengthMs - p.ms), p.speed)}});
  return listenTime(d.lengthMs, 100);
}
std::string withAuthor(const std::string& author, const std::string& rest) {
  if (author.empty()) return rest;
  if (rest.empty()) return author;
  return author + " \xc2\xb7 " + rest;
}

// back to Now Playing when a list was opened from it, else Now Playing over it
void showPlaying(bool fromNowPlaying) {
  if (fromNowPlaying) g_app->popTo(tr("menu.nowPlaying"));
  else openNowPlaying();
}

class BooksScreen : public ShellListScreen {
 public:
  BooksScreen() : ShellListScreen(tr("books.title")) { render(); }
  void onShow() override {
    g_library.rescanIfStale();
    render();
  }
  bool tick(int64_t) override {
    if (generation_ == g_library.generation() && changes_ == g_library.bookChanges() && scanning_ == g_library.scanning()) return false;
    render();
    return true;
  }
  int64_t wakeAt() const override { return scanning_ ? now_ns() + 250 * kMs : 0; }

 private:
  void render() {
    generation_ = g_library.generation(), changes_ = g_library.bookChanges(), scanning_ = g_library.scanning();
    const std::vector<Book>& books = g_library.books();
    if (books.empty()) {
      list_.setMessage(scanning_ ? tr("library.scanning") : tr("books.none"));
      return;
    }
    struct Entry {
      const Book* book;
      BookProgress p;
      std::string subtitle;
    };
    std::vector<Entry> all;
    for (const Book& b : books) {
      BookPlay d = bookDetails(b);
      BookProgress p = bookProgress(b, d);
      all.push_back({&b, p, withAuthor(b.author, bookStatus(b, d, p))});
    }
    std::vector<const Entry*> going;
    for (const Entry& e : all)
      if (e.p.started && !e.p.finished) going.push_back(&e);
    std::stable_sort(going.begin(), going.end(), [](const Entry* a, const Entry* b) { return a->p.lastPlayed > b->p.lastPlayed; });
    std::vector<Row> rows;
    auto row = [&](const Entry& e) {
      Row r;
      r.title = e.book->title;
      r.subtitle = e.subtitle;
      r.thumb = e.book->cover;
      r.dot = !e.p.started;
      std::string key = e.book->key;
      r.onSelect = [key] { g_app->push(makeBook(key)); };
      r.onLongSelect = [key] {  // held: on with it at once
        if (const Book* b = g_library.bookByKey(key)) playBook(*b), openNowPlaying();
      };
      rows.push_back(std::move(r));
    };
    auto header = [&](const std::string& title) {
      Row h;
      h.header = true;
      h.title = title;
      rows.push_back(h);
    };
    if (!going.empty()) {
      header(tr("books.continue"));
      for (const Entry* e : going) row(*e);
    }
    header(tr("books.allHeader", {{"count", all.size()}}));
    for (const Entry& e : all) row(e);
    list_.setRows(std::move(rows), true);
  }
  uint64_t generation_ = 0, changes_ = 0;
  bool scanning_ = false;
};

// A book's screen: its cover and what it is above, what can be done with it below
class BookScreen : public Screen {
 public:
  explicit BookScreen(std::string key) : key_(std::move(key)) {
    if (const Book* b = g_library.bookByKey(key_)) title_ = b->title;
    render();
  }
  std::string title() const override { return title_.empty() ? tr("books.title") : title_; }
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
  bool tick(int64_t) override {
    PlayerState s = g_player.state();
    bool playing = bookNow() && bookNow()->key == key_ && s.playing;
    if (changes_ == g_library.bookChanges() && generation_ == g_library.generation() && playing == playing_) return false;
    render();
    return true;
  }
  void draw(Env& e, float w, float h) override {
    const Palette& p = e.p;
    e.c.layer(0);
    e.c.fill({0, 0, w, h}, p.shellBg);
    const Book* b = g_library.bookByKey(key_);
    int pad = e.dp(10), art = e.dp(64);
    float headH = float(pad + art + e.dp(8));
    if (b) {
      Rect ar{float(pad), float(pad), float(art), float(art)};
      e.c.layer(2);
      float a = 1;
      const Texture* tex = e.covers.get(b->cover, art, art, 0, e.now, &a);
      if (!tex || a < 1) {
        e.c.shape(ar, e.dpf(5), p.rowLine);
        int g = e.dp(30);
        e.icons.draw(e.c, Icon::Book, {ar.x + float((art - g) / 2), ar.y + float((art - g) / 2), float(g), float(g)}, p.muted);
      }
      e.c.layer(3);
      if (tex) e.c.image(*tex, ar, {0, 0, 1, 1}, e.dpf(5), withAlpha(0xffffffff, a));
      e.c.layer(4);
      float tx = ar.x + art + e.dp(10), tw = w - tx - pad, y = ar.y;
      for (const Label& l : wrap(e, b->title, 15, true, tw, 2)) {
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
      line(b->author, 12, p.text);
      if (!details_.narrator.empty()) line(tr("book.readBy", {{"name", details_.narrator}}), 11, p.muted);
      std::string len = listenTime(details_.lengthMs, 100);
      if (!details_.skips) len = tr("book.lengthAndChapters", {{"length", len}, {"count", details_.chapters.size()}});
      line(len, 11, p.muted);
    }
    e.c.layer(0);
    e.c.fill({0, headH - 1, w, 1}, p.rowLine);
    list_.layout(e, w, h - headH);
    list_.draw(e, 0, headH);
  }

 private:
  void render() {
    changes_ = g_library.bookChanges(), generation_ = g_library.generation();
    const Book* b = g_library.bookByKey(key_);
    if (!b) {
      list_.setMessage(tr("books.gone"));
      return;
    }
    title_ = b->title;
    details_ = bookDetails(*b);
    BookProgress p = bookProgress(*b, details_);
    const BookPlay* now = bookNow();
    PlayerState s = g_player.state();
    playing_ = now && now->key == key_ && s.playing;
    std::string key = key_;
    std::vector<Row> rows;
    auto add = [&](const std::string& title, const Tile& tile, std::string subtitle, std::string trailing, bool chevron,
                   std::function<void()> on) {
      Row r;
      r.title = title, r.tile = &tile, r.subtitle = std::move(subtitle), r.trailing = std::move(trailing), r.chevron = chevron;
      r.onSelect = std::move(on);
      rows.push_back(std::move(r));
    };
    int chapter = chapterAt(details_.chapters, p.place);
    std::string where = details_.skips ? "" : chapterName(details_, chapter);
    std::string left = tr("book.left", {{"time", listenTime(std::max<int64_t>(0, details_.lengthMs - p.ms), p.speed)}});
    if (playing_) {
      add(tr("menu.nowPlaying"), tiles::resume, withAuthor(where, left), "", true, [] { openNowPlaying(); });
    } else if (p.finished) {
      add(tr("book.startAgain"), tiles::startAgain, tr("book.finished"), "", false, [key] {
        if (const Book* b = g_library.bookByKey(key)) playBookFrom(*b, {0, 0}), openNowPlaying();
      });
    } else {
      add(p.started ? tr("book.resume") : tr("book.start"), tiles::resume, p.started ? withAuthor(where, left) : listenTime(details_.lengthMs, 100), "",
          false, [key] {
            if (const Book* b = g_library.bookByKey(key)) playBook(*b), openNowPlaying();
          });
    }
    if (p.started && !p.finished)
      add(tr("book.startAgain"), tiles::startAgain, "", "", false, [key] {
        if (const Book* b = g_library.bookByKey(key)) playBookFrom(*b, {0, 0}), openNowPlaying();
      });
    if (!details_.skips)
      add(tr("book.chapters"), tiles::chapters, "", std::to_string(details_.chapters.size()), true, [key] { g_app->push(makeChapters(key, false)); });
    size_t marks = g_library.bookmarks(key_).size();
    add(tr("book.bookmarks"), tiles::bookmarks, "", marks ? std::to_string(marks) : "", true, [key] { g_app->push(makeBookmarks(key, false)); });
    add(tr("book.speed"), tiles::speed, "", speedLabel(p.speed), true, [key] { g_app->push(makeBookSpeed(key)); });
    add(p.finished ? tr("book.markUnfinished") : tr("book.markFinished"), tiles::finished, "", "", false, [this, key, p] {
      if (const Book* b = g_library.bookByKey(key)) markBookFinished(*b, !p.finished);
      render();
    });
    list_.setRows(std::move(rows), true);
  }
  std::string key_, title_;
  BookPlay details_;
  RowList list_;
  uint64_t changes_ = 0, generation_ = 0;
  bool playing_ = false;
};

class ChaptersScreen : public ShellListScreen {
 public:
  ChaptersScreen(std::string key, bool fromNowPlaying) : ShellListScreen(tr("book.chapters")), key_(std::move(key)), fromNp_(fromNowPlaying) {
    render();
  }

 private:
  void render() {
    const Book* b = g_library.bookByKey(key_);
    if (!b) return list_.setMessage(tr("books.gone"));
    BookPlay d = bookDetails(*b);
    BookProgress p = bookProgress(*b, d);
    int now = chapterAt(d.chapters, p.place);
    std::vector<Row> rows;
    for (size_t i = 0; i < d.chapters.size(); i++) {
      const Chapter& c = d.chapters[i];
      Row r;
      r.title = chapterName(d, int(i));
      r.subtitle = formatTime(int(c.bookStartMs / 1000));
      r.trailing = int(i) == now && p.started ? tr("common.current") : formatTime(int((c.endMs - c.startMs) / 1000));
      std::string key = key_;
      Place at{c.file, c.startMs};
      bool fromNp = fromNp_;
      r.onSelect = [key, at, fromNp] {
        if (const Book* b = g_library.bookByKey(key)) playBookFrom(*b, at);
        showPlaying(fromNp);
      };
      rows.push_back(std::move(r));
    }
    list_.setRows(std::move(rows));
    if (now >= 0) list_.selectIndex(now);
  }
  std::string key_;
  bool fromNp_;
};

class BookmarksScreen : public ShellListScreen {
 public:
  BookmarksScreen(std::string key, bool fromNowPlaying) : ShellListScreen(tr("book.bookmarks")), key_(std::move(key)), fromNp_(fromNowPlaying) {
    render();
  }
  void onShow() override { render(); }

 private:
  void render() {
    const Book* b = g_library.bookByKey(key_);
    if (!b) return list_.setMessage(tr("books.gone"));
    BookPlay d = bookDetails(*b);
    std::vector<Row> rows;
    const BookPlay* now = bookNow();
    if (now && now->key == key_) {
      Row add;
      add.title = tr("book.addBookmark");
      add.tile = &tiles::bookmarks;
      add.onSelect = [this] {
        if (addBookmarkNow()) g_app->toast(tr("book.bookmarkAdded"));
        render();
      };
      rows.push_back(std::move(add));
    }
    std::vector<Bookmark> marks = g_library.bookmarks(key_);
    std::sort(marks.begin(), marks.end(), [](const Bookmark& a, const Bookmark& b) { return a.created > b.created; });
    for (const Bookmark& m : marks) {
      Place at{0, m.positionMs};
      for (size_t i = 0; i < d.paths.size(); i++)
        if (d.paths[i] == m.file) at.file = int(i);
      int c = chapterAt(d.chapters, at);
      Row r;
      int64_t inChapter = c >= 0 ? at.ms - d.chapters[size_t(c)].startMs : at.ms;
      r.title = d.skips ? formatTime(int(at.ms / 1000))
                        : tr("book.bookmark.title", {{"chapter", chapterName(d, c)}, {"time", formatTime(int(std::max<int64_t>(0, inChapter) / 1000))}});
      time_t t = time_t(m.created / 1000);
      r.subtitle = clockDate(t, false) + ", " + clockTime(t, g_prefs.clock24);
      std::string key = key_;
      bool fromNp = fromNp_;
      r.onSelect = [key, at, fromNp] {
        if (const Book* b = g_library.bookByKey(key)) playBookFrom(*b, at);
        showPlaying(fromNp);
      };
      int64_t created = m.created;
      r.onLongSelect = [key, created] {
        g_app->push(makeChoice(tr("book.bookmark.deleteTitle"), {{tr("book.bookmark.keep"), "keep"}, {tr("common.delete"), "delete"}},
                               [] { return std::string(); }, [key, created](const std::string& v) {
                                 if (v == "delete") g_library.removeBookmark(key, created);
                               }));
      };
      rows.push_back(std::move(r));
    }
    if (rows.empty()) return list_.setMessage(tr("book.bookmarks.none"));
    list_.setRows(std::move(rows), true);
  }
  std::string key_;
  bool fromNp_;
};

std::string g_sleepPicked;  // the sleep timer's row last picked ("15" … "end"), while it runs

}  // namespace

std::unique_ptr<Screen> makeAudiobooks() { return std::unique_ptr<Screen>(new BooksScreen()); }
std::unique_ptr<Screen> makeBook(const std::string& key) { return std::unique_ptr<Screen>(new BookScreen(key)); }
std::unique_ptr<Screen> makeChapters(const std::string& key, bool fromNowPlaying) {
  return std::unique_ptr<Screen>(new ChaptersScreen(key, fromNowPlaying));
}
std::unique_ptr<Screen> makeBookmarks(const std::string& key, bool fromNowPlaying) {
  return std::unique_ptr<Screen>(new BookmarksScreen(key, fromNowPlaying));
}

std::unique_ptr<Screen> makeBookSpeed(const std::string& key) {
  std::vector<std::pair<std::string, std::string>> choices;
  for (int s : kSpeeds) choices.push_back({s == 100 ? tr("book.speedNormal", {{"speed", speedLabel(s)}}) : speedLabel(s), std::to_string(s)});
  return makeChoice(
      tr("book.speed"), choices,
      [key] {
        const BookPlay* now = bookNow();
        if (now && now->key == key) return std::to_string(bookSpeedNow());
        const BookState* s = g_library.bookState(key);
        return std::to_string(s ? s->speed : 100);
      },
      [key](const std::string& v) {
        if (const Book* b = g_library.bookByKey(key)) setBookSpeed(*b, atoi(v.c_str()));
      });
}

std::unique_ptr<Screen> makeSleepTimer() {
  std::vector<std::pair<std::string, std::string>> choices = {{tr("common.off"), "off"}};
  for (int m : {15, 30, 45, 60}) choices.push_back({tr("sleep.minutes", {{"count", m}}), std::to_string(m)});
  choices.push_back({sleepEndName(), "end"});
  return makeChoice(
      tr("sleep.title"), choices, [] { return sleepMode() == SleepMode::Off ? std::string("off") : g_sleepPicked; },
      [](const std::string& v) {
        g_sleepPicked = v;
        if (v == "off") sleepStart(SleepMode::Off);
        else if (v == "end") sleepStart(SleepMode::EndOfItem);
        else sleepStart(SleepMode::Minutes, atoi(v.c_str()));
      },
      [](const std::string& v) {  // the one running says what's left
        return v == g_sleepPicked && sleepMode() == SleepMode::Minutes ? sleepMark() : std::string();
      });
}

}  // namespace rondo::shell
