// books.h: an audiobook as the player's queue, its place kept, its controls, and the sleep timer.
//
//   book-queue.txt   the book playing: its key, then its files, a line each (written as it starts)
//   book-state.txt   where: the file's index, the position, playing, the speed, whether the music
//                    waits, when (with every save): what brings the book back after a restart
#include "books.h"

#include <math.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {

std::string dataDir() { return g_prefsPath.substr(0, g_prefsPath.rfind('/') + 1); }

bool writeFile(const std::string& path, const std::string& text) {
  std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && fflush(f) == 0;
  fclose(f);
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

// the book playing (on), and what it was when the music was lent to it
struct Session {
  bool on = false;
  BookPlay book;
  int speed = 100;
  bool lent = false;  // the music queue waits in queue.txt
  bool musicShuffle = false;
  int musicRepeat = 0;
  int64_t savedAt = 0;    // now_ns() of the last save
  bool wasPlaying = false;
  int64_t pausedWall = 0;  // ms since 1970 when it paused (0: playing, or not known)
  bool detailed = false;   // chapters from the library (a book restored at startup has only its files)
  bool finishedHandled = false;
};
Session g;

void forgetSessionFiles() {
  unlink((dataDir() + "book-queue.txt").c_str());
  unlink((dataDir() + "book-state.txt").c_str());
}

void writeSessionQueue() {
  std::string text = g.book.key + "\n";
  for (const std::string& p : g.book.paths) text += p + "\n";
  writeFile(dataDir() + "book-queue.txt", text);
}

void writeSessionState(int index, int64_t ms, bool playing) {
  char b[160];
  snprintf(b, sizeof(b), "%d %lld %d %d %d %lld\n", index, (long long)ms, playing ? 1 : 0, g.speed, g.lent ? 1 : 0,
           (long long)time(nullptr));
  writeFile(dataDir() + "book-state.txt", b);
}

// the player's place in the book playing
bool placeNow(Place& p, PlayerState* out = nullptr) {
  PlayerState s = g_player.state();
  if (out) *out = s;
  if (s.index < 0 || s.index >= int(g.book.paths.size())) return false;
  p.file = s.index;
  p.ms = PlayerHost::positionAt(s, now_ns());
  if (s.ended && !g.book.fileMs.empty()) p.file = int(g.book.fileMs.size()) - 1, p.ms = std::max<int64_t>(0, g.book.fileMs.back());
  return true;
}

void saveSession(bool force = false) {
  if (!g.on) return;
  Place p;
  PlayerState s;
  if (!placeNow(p, &s)) return;
  bool playing = s.playing && !s.ended;
  BookState st;
  if (const BookState* old = g_library.bookState(g.book.key)) st = *old;
  st.key = g.book.key;
  st.file = g.book.paths[size_t(p.file)];
  st.positionMs = p.ms;
  st.speed = g.speed;
  int64_t at = bookMsOf(g.book.fileMs, p);
  st.finished = finishedAt(at, g.book.lengthMs) || s.ended;
  st.lastPlayed = playing || !g.pausedWall ? wallNowMs() : g.pausedWall;
  g_library.setBookState(st);
  writeSessionState(p.file, p.ms, playing);
  g.savedAt = now_ns();
  (void)force;
}

// the book stops being the queue (replaced, left, finished): its place is kept, the session files go
void endSession() {
  if (!g.on) return;
  saveSession(true);
  g.on = false;
  g.lent = false;
  forgetSessionFiles();
}

// a resume after a pause: a little earlier, the longer the pause
void resumeWithRewind() {
  Place p;
  PlayerState s;
  if (!placeNow(p, &s)) return g_player.play();
  int64_t rw = g.pausedWall ? rewindAfterPause(wallNowMs() - g.pausedWall) : 0;
  g.pausedWall = 0;
  if (rw <= 0) return g_player.play();
  int64_t at = std::max<int64_t>(0, bookMsOf(g.book.fileMs, p) - rw);
  Place q = placeAt(g.book.fileMs, at);
  if (q.file == p.file) {
    g_player.seekTo(q.ms);
    g_player.play();
  } else {
    g_player.playBook(g.book.paths, q.file, q.ms, true, g.speed);
  }
}

// to a place of the book playing, playing or paused as it is
void goTo(Place t) {
  PlayerState s = g_player.state();
  if (t.file == s.index) g_player.seekTo(t.ms);
  else g_player.playBook(g.book.paths, t.file, t.ms, s.playing && !s.ended, g.speed);
  g.pausedWall = 0;  // a place chosen: no rewind on the next play
}

// the player's queue is the book's
bool queueIsBook() { return !g.book.paths.empty() && g_player.queue() == g.book.paths; }

// The player puts in another queue (anything but a book): the book keeps its place, the music's
// shuffle and repeat come back for what plays now
void onReplace() {
  if (!g.on) return;
  bool shuffle = g.musicShuffle;
  int repeat = g.musicRepeat;
  endSession();
  g_player.setShuffle(shuffle);
  g_player.setRepeat(repeat);
}

// the sleep timer
struct Sleep {
  SleepMode mode = SleepMode::Off;
  int64_t until = 0;  // now_ns() (Minutes)
  bool fading = false;
  int64_t fadeEnd = 0;
};
Sleep g_sleep;
constexpr int64_t kFadeMs = 10000;

}  // namespace

int64_t wallNowMs() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

BookPlay bookDetails(const Book& b) {
  BookPlay d;
  d.key = b.key, d.title = b.title, d.author = b.author;
  std::vector<std::vector<FileChapter>> infos;
  std::vector<std::string> titles;
  for (const Track* t : b.files) {
    d.paths.push_back(t->path);
    const BookFileInfo* info = g_library.bookFileInfo(t->path);
    int64_t ms = info && info->durationMs > 0 ? info->durationMs : t->durationSec >= 0 ? int64_t(t->durationSec) * 1000 : 0;
    d.fileMs.push_back(ms);
    d.lengthMs += ms;
    infos.push_back(info ? info->chapters : std::vector<FileChapter>());
    titles.push_back(t->title);
    if (d.narrator.empty() && info) d.narrator = info->narrator;
  }
  d.chapters = buildChapters(d.fileMs, infos, titles);
  d.skips = skipsTime(d.chapters);
  // a book of one file: its chapter has the book's name, not the file's
  if (d.skips && !d.chapters.empty()) d.chapters[0].title = d.title;
  return d;
}

std::string chapterName(const BookPlay& b, int chapter) {
  if (chapter < 0 || chapter >= int(b.chapters.size())) return "";
  const std::string& t = b.chapters[size_t(chapter)].title;
  return t.empty() ? tr("book.chapterNumber", {{"number", chapter + 1}}) : t;
}

BookProgress bookProgress(const Book& b, const BookPlay& d) {
  BookProgress p;
  if (const BookState* s = g_library.bookState(b.key)) {
    p.speed = s->speed;
    p.finished = s->finished;
    p.lastPlayed = s->lastPlayed;
    p.started = s->lastPlayed > 0;
    for (size_t i = 0; i < d.paths.size(); i++)
      if (d.paths[i] == s->file) p.place = {int(i), s->positionMs};
  }
  if (g.on && g.book.key == b.key) {  // the one playing: the player's place
    Place now;
    if (placeNow(now)) p.place = now, p.started = true;
    p.speed = g.speed;
  }
  p.ms = bookMsOf(d.fileMs, p.place);
  return p;
}

std::string listenTime(int64_t ms, int speed) {
  int64_t t = ms * 100 / std::max(1, speed);
  if (t < 60000) return tr("book.time.underMinute");
  int64_t minutes = (t + 30000) / 60000, h = minutes / 60, m = minutes % 60;
  if (h && m) return tr("book.time.hoursMinutes", {{"hours", h}, {"minutes", m}});
  if (h) return tr("book.time.hours", {{"count", h}});
  return tr("book.time.minutes", {{"count", m}});
}

std::string speedLabel(int percent) {
  std::string s = speedText(percent);
  std::string point = tr("feed.decimalMark");
  size_t dot = s.find('.');
  if (dot != std::string::npos && point != ".") s.replace(dot, 1, point);
  return s;
}

const BookPlay* bookNow() { return g.on && queueIsBook() ? &g.book : nullptr; }

Place bookPlace() {
  Place p;
  if (g.on) placeNow(p);
  return p;
}

int bookSpeedNow() { return g.on ? g.speed : 100; }

void playBookFrom(const Book& b, Place p, bool play) {
  BookPlay d = bookDetails(b);
  if (d.paths.empty()) return;
  p.file = std::max(0, std::min(p.file, int(d.paths.size()) - 1));
  p.ms = std::max<int64_t>(0, p.ms);
  const BookState* st = g_library.bookState(b.key);
  int speed = st ? clampSpeed(st->speed) : 100;
  if (g.on && g.book.key != b.key) saveSession(true);  // another book: it keeps its place, the music still waits
  if (!g.on) {
    // the music queue waits for the book, where it is (queue.txt); nothing to give back without one
    PlayerState s = g_player.state();
    bool music = g_player.active() && s.index >= 0 && !s.video && !s.fmOn;
    if (music) resumeSaveNow();
    else resumeForgetMusic();
    g.lent = music && resumeHasMusic();
    g.musicShuffle = s.shuffle != 0, g.musicRepeat = s.repeat;
  }
  g.on = true;
  g.book = d;
  g.detailed = true;
  g.speed = speed;
  g.pausedWall = 0;
  g.finishedHandled = false;
  writeSessionQueue();
  g_player.setShuffle(false);  // a book in its order, once
  g_player.setRepeat(0);
  g_player.playBook(d.paths, p.file, p.ms, play, speed);
  writeSessionState(p.file, p.ms, play);
  // started (again): not finished, played now
  BookState ns = st ? *st : BookState();
  ns.key = b.key, ns.file = d.paths[size_t(p.file)], ns.positionMs = p.ms, ns.speed = speed;
  ns.lastPlayed = wallNowMs();
  if (!finishedAt(bookMsOf(d.fileMs, p), d.lengthMs)) ns.finished = false;
  g_library.setBookState(ns);
  g.savedAt = now_ns();
  g.wasPlaying = play;
}

void playBook(const Book& b) {
  // the one playing, paused: on from where it is, a little earlier
  if (g.on && g.book.key == b.key && queueIsBook()) {
    PlayerState s = g_player.state();
    if (!s.playing) {
      if (s.ended) playBookFrom(b, {0, 0});
      else resumeWithRewind();
    }
    return;
  }
  BookPlay d = bookDetails(b);
  BookProgress p = bookProgress(b, d);
  if (p.finished || !p.started) return playBookFrom(b, p.finished ? Place{0, 0} : p.place);
  int64_t rw = p.lastPlayed ? rewindAfterPause(wallNowMs() - p.lastPlayed) : 0;
  playBookFrom(b, placeAt(d.fileMs, std::max<int64_t>(0, p.ms - rw)));
}

void playBookFile(const Book& b, const std::string& path) {
  BookPlay d = bookDetails(b);
  for (size_t i = 0; i < d.paths.size(); i++)
    if (d.paths[i] == path) {
      // the file the book is at already: on from its place
      BookProgress p = bookProgress(b, d);
      if (p.started && p.place.file == int(i) && !p.finished) return playBook(b);
      return playBookFrom(b, {int(i), 0});
    }
  playBook(b);
}

void setBookSpeed(const Book& b, int percent) {
  percent = clampSpeed(percent);
  if (g.on && g.book.key == b.key) {
    g.speed = percent;
    g_player.setSpeed(percent);
    saveSession(true);
    return;
  }
  BookState st;
  if (const BookState* old = g_library.bookState(b.key)) st = *old;
  st.key = b.key;
  st.speed = percent;
  g_library.setBookState(st);
}

void markBookFinished(const Book& b, bool finished) {
  if (g.on && g.book.key == b.key) saveSession(true);
  BookState st;
  if (const BookState* old = g_library.bookState(b.key)) st = *old;
  st.key = b.key;
  st.finished = finished;
  if (finished && !st.lastPlayed) st.lastPlayed = wallNowMs();
  if (!finished) {
    // unfinished: from its start next time (as never played, unless it was left somewhere)
    BookPlay d = bookDetails(b);
    int64_t at = 0;
    for (size_t i = 0; i < d.paths.size(); i++)
      if (d.paths[i] == st.file) at = bookMsOf(d.fileMs, {int(i), st.positionMs});
    if (finishedAt(at, d.lengthMs)) st.file = d.paths.empty() ? "" : d.paths[0], st.positionMs = 0;
  }
  g_library.setBookState(st);
}

bool addBookmarkNow() {
  if (!bookNow()) return false;
  Place p;
  if (!placeNow(p)) return false;
  Bookmark m;
  m.key = g.book.key;
  m.file = g.book.paths[size_t(p.file)];
  m.positionMs = p.ms;
  m.created = wallNowMs();
  g_library.addBookmark(m);
  return true;
}

bool musicLent() { return bookNow() && g.lent; }

void backToMusic() {
  if (!g.on) return;
  bool lent = g.lent;
  if (g_player.state().playing) g_player.pause();
  endSession();
  if (lent) resumeMusic(true);
}

void transportToggle() {
  if (bookNow()) {
    PlayerState s = g_player.state();
    if (!s.playing && !s.ended && s.index >= 0) return resumeWithRewind();
  }
  g_player.togglePlay();
}

void transportPlay() {
  if (bookNow()) {
    PlayerState s = g_player.state();
    if (!s.playing && !s.ended && s.index >= 0) return resumeWithRewind();
  }
  g_player.play();
}

void transportNext() {
  if (const BookPlay* b = bookNow()) {
    Place at;
    if (!placeNow(at)) return;
    Place t = nextPlace(b->chapters, b->fileMs, at);
    if (t.file >= 0) goTo(t);
    return;
  }
  g_player.next();
}

void transportPrev() {
  if (const BookPlay* b = bookNow()) {
    Place at;
    if (!placeNow(at)) return;
    goTo(previousPlace(b->chapters, b->fileMs, at));
    return;
  }
  g_player.previous();
}

void transportHold(int dir) {
  if (bookNow()) return g_player.seekBy(dir * 30000);
  int64_t d = g_player.state().durationMs;  // 2 % of the length per repeat, at least 10 s
  int64_t step = std::max<int64_t>(10000, d / 50);
  g_player.seekBy(dir * step);
}

void booksStart() {
  g_player.onReplace = [] { onReplace(); };
  if (g.on && !g.detailed)  // restored at startup: the chapters now the library is open
    if (const Book* b = g_library.bookByKey(g.book.key)) {
      BookPlay d = bookDetails(*b);
      if (d.paths == g.book.paths) g.book = d, g.detailed = true;
    }
}

bool booksTick(int64_t now) {
  bool changed = false;
  PlayerState s = g_player.state();
  if (g.on && !queueIsBook()) {
    // something else took the queue without the player's hooks (it can't, but nothing is lost)
    g.on = false;
    g.lent = false;
    forgetSessionFiles();
  } else if (!g.on && queueIsBook() && s.playing && !s.ended) {
    // the book's queue back from elsewhere (a voice message played in its place): a book again
    g.on = true;
    g.lent = false;
    writeSessionQueue();
    if (s.speed != g.speed) g_player.setSpeed(g.speed);
  }
  if (g.on) {
    if (!g.detailed)
      if (const Book* b = g_library.bookByKey(g.book.key)) {
        BookPlay d = bookDetails(*b);
        if (d.paths == g.book.paths) g.book = d, g.detailed = true, changed = true;
      }
    bool playing = s.playing && !s.ended;
    if (playing != g.wasPlaying) {  // a pause (or the end) is kept at once; a play just noted
      g.wasPlaying = playing;
      if (!playing) g.pausedWall = wallNowMs();
      saveSession(true);
      changed = true;
    } else if (playing && now - g.savedAt >= 10 * kSec) {
      saveSession();
    }
    if (s.ended && !g.finishedHandled) {
      // the book's end: finished, and the music that waited comes back where it was, paused
      g.finishedHandled = true;
      saveSession(true);
      if (g.lent) {
        endSession();
        resumeMusic(false);
      }
      changed = true;
    }
  }
  // the sleep timer
  if (g_sleep.mode != SleepMode::Off) {
    if (g_sleep.fading) {
      if (!s.playing) {  // paused meanwhile: done
        g_sleep = {};
        changed = true;
      } else if (now >= g_sleep.fadeEnd) {
        g_sleep = {};
        g_player.pause();  // after the fade: silent already
        changed = true;
      }
    } else {
      int64_t left = -1;
      if (g_sleep.mode == SleepMode::Minutes) {
        left = (g_sleep.until - now) / kMs;
        if (left <= 0 && !s.playing) g_sleep = {}, changed = true;
      } else if (s.playing && !s.ended && s.index >= 0) {
        int64_t pos = PlayerHost::positionAt(s, now), end = s.durationMs;
        if (const BookPlay* b = bookNow()) {
          int c = chapterAt(b->chapters, {s.index, pos});
          if (c >= 0) end = b->chapters[size_t(c)].endMs;
        }
        if (end > 0) left = std::max<int64_t>(0, end - pos) * 100 / std::max(1, s.speed > 0 ? s.speed : 100);
      }
      if (left >= 0 && left <= kFadeMs && s.playing && g_sleep.mode != SleepMode::Off) {
        g_player.fade(int(std::max<int64_t>(1, left)));
        g_sleep.fading = true;
        g_sleep.fadeEnd = now + std::max<int64_t>(1, left) * kMs;
        changed = true;
      }
    }
  }
  return changed;
}

int64_t booksWakeAt() {
  if (g_sleep.mode == SleepMode::Off) return 0;
  if (g_sleep.fading) return g_sleep.fadeEnd;
  if (g_sleep.mode == SleepMode::Minutes) return g_sleep.until - kFadeMs * kMs;
  return 0;  // the end of what plays: the loop looks every 250 ms while it plays
}

void booksSaveNow() { saveSession(true); }

BookResume booksRestore() {
  FILE* f = fopen((dataDir() + "book-state.txt").c_str(), "r");
  if (!f) return BookResume::None;
  int index = 0, playing = 0, speed = 100, lent = 0;
  long long ms = 0, at = 0;
  bool ok = fscanf(f, "%d %lld %d %d %d %lld", &index, &ms, &playing, &speed, &lent, &at) == 6;
  fclose(f);
  std::vector<std::string> paths;
  std::string key;
  if (FILE* q = fopen((dataDir() + "book-queue.txt").c_str(), "r")) {
    char buf[4096];
    bool first = true;
    while (fgets(buf, sizeof(buf), q)) {
      std::string line = buf;
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      if (first) key = line, first = false;
      else if (!line.empty()) paths.push_back(line);
    }
    fclose(q);
  }
  if (!ok || key.empty() || paths.empty() || index < 0 || index >= int(paths.size())) {
    forgetSessionFiles();
    return BookResume::None;
  }
  const std::string& cur = paths[size_t(index)];
  if (access(cur.c_str(), R_OK) != 0) {
    int root = cur.compare(0, 17, "/storage/sdcard0/") == 0 ? 1 : cur.compare(0, 17, "/storage/sdcard1/") == 0 ? 2 : 0;
    if (root && !(g_library.presentRoots() & root)) return BookResume::Waiting;
    forgetSessionFiles();  // the book is gone: the music comes back instead
    return BookResume::None;
  }
  g = Session();
  g.on = true;
  g.book.key = key;
  g.book.paths = paths;
  g.book.fileMs.assign(paths.size(), 0);
  g.speed = clampSpeed(speed);
  g.lent = lent != 0;
  // playing again only after a crash or a quick restart (seconds ago); after a night off, paused
  bool resume = playing && time(nullptr) - at < 20;
  g.wasPlaying = resume;
  g.pausedWall = resume ? 0 : int64_t(at) * 1000;
  g_player.setShuffle(false);
  g_player.setRepeat(0);
  g_player.playBook(paths, index, ms, resume, g.speed);
  g.savedAt = now_ns();
  printf("rondo: the audiobook is back: %zu files, at %d, %lld ms%s\n", paths.size(), index, ms, resume ? ", playing" : "");
  return BookResume::Restored;
}

bool booksOwnQueue() { return queueIsBook(); }

bool booksQueueMoved(const std::vector<std::string>& queue) {
  if (!g.on || !queueIsBook()) return false;
  if (queue.size() != g.book.paths.size()) return false;
  PlayerState s = g_player.state();
  Place p;
  placeNow(p);
  g.book.paths = queue;
  if (const Book* b = g_library.bookOf(queue.front())) g.book.key = b->key;
  writeSessionQueue();
  g_player.playBook(queue, p.file, p.ms, s.playing != 0, g.speed);
  return true;
}

bool booksTakeQueueAdd(const std::string& path, bool next) {
  if (!bookNow()) return false;
  if (!resumeQueueAdd(path, next)) return false;
  g.lent = true;
  PlayerState s = g_player.state();
  writeSessionState(s.index, PlayerHost::positionAt(s, now_ns()), s.playing && !s.ended);
  return true;
}

void sleepStart(SleepMode mode, int minutes) {
  if (g_sleep.fading) g_player.fade(0);  // the sound back first
  g_sleep = {};
  g_sleep.mode = mode;
  if (mode == SleepMode::Minutes) g_sleep.until = now_ns() + int64_t(minutes) * 60 * kSec;
}

SleepMode sleepMode() { return g_sleep.mode; }

int64_t sleepLeftMs() { return g_sleep.mode == SleepMode::Minutes ? std::max<int64_t>(0, (g_sleep.until - now_ns()) / kMs) : 0; }

std::string sleepMark() {
  switch (g_sleep.mode) {
    case SleepMode::Off: return "";
    case SleepMode::Minutes: {
      int64_t m = (sleepLeftMs() + 59999) / 60000;
      return tr("sleep.mark.minutes", {{"count", std::max<int64_t>(1, m)}});
    }
    default: return tr("sleep.mark.end");
  }
}

std::string sleepEndName() {
  if (const BookPlay* b = bookNow()) return b->skips ? tr("sleep.endOfBook") : tr("sleep.endOfChapter");
  return tr("sleep.endOfSong");
}

}  // namespace rondo::shell
