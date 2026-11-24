#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <sqlite3.h>
#include <yyjson.h>

#include "books.h"
#include "clock.h"
#include "fold.h"
#include "tags.h"

namespace rondo {
namespace {

const char* kAudio[] = {"flac", "mp3", "m4a", "m4b", "aac", "ogg", "oga", "opus", "wav", "w64", "aif", "aiff", "aifc", "wma",
                        "ape", "wv", "ac3", "eac3", "ec3", "dts", "mka", "dsf", "dff", "tta", "tak", "mpc", "amr", "awb",
                        "mp2", "caf", "au"};
const char* kVideo[] = {"mp4", "m4v", "mov", "mkv", "3gp", "webm"};
const char* kSkip[] = {"android", "lost.dir", "dcim", ".thumbnails"};
const char* kGenericFolders[] = {"music", "video", "videos", "movies", "sdcard0", "sdcard1"};
const char* kRoots[] = {"/storage/sdcard0", "/storage/sdcard1"};  // the card, the internal storage

// whether something is mounted at this path (/proc/mounts' second field)
bool mounted(const char* path) {
  FILE* f = fopen("/proc/mounts", "r");
  if (!f) return false;
  char line[512], dev[256], at[256];
  bool found = false;
  while (!found && fgets(line, sizeof(line), f))
    if (sscanf(line, "%255s %255s", dev, at) == 2 && !strcmp(at, path)) found = true;
  fclose(f);
  return found;
}

bool under(const std::string& path, const std::vector<std::string>& roots) {
  for (const std::string& r : roots)
    if (path.size() > r.size() && path[r.size()] == '/' && path.compare(0, r.size(), r) == 0) return true;
  return false;
}

bool in(const char* const* list, size_t n, const std::string& s) {
  for (size_t i = 0; i < n; i++)
    if (s == list[i]) return true;
  return false;
}

std::string asciiLower(std::string s) {
  for (char& c : s) c = char(tolower((unsigned char)c));
  return s;
}

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string baseName(const std::string& path) { return path.substr(path.rfind('/') + 1); }

// what search matches: a song's title, artist, album and file name; a video's title and file name. And
// whether it is an audiobook's (core/books.h), which follows the path and the genre.
void setKey(Track& t) {
  t.key = t.video ? fold(t.title + " " + baseName(t.path)) : fold(t.title + " " + t.artist + " " + t.album + " " + baseName(t.path));
  t.book = !t.video && (bookCandidate(t.path) || isBookGenre(t.genre));
}

// "Artist - Title.flac"; a "01 " track prefix is dropped (LocalLibrary.fromName()).
Track fromName(const std::string& path, int64_t size, int64_t modified, bool video) {
  Track t;
  t.path = path, t.size = size, t.modified = modified, t.video = video;
  std::string name = baseName(path);
  size_t dot = name.rfind('.');
  std::string base = dot == std::string::npos ? name : name.substr(0, dot);
  std::replace(base.begin(), base.end(), '_', ' ');
  base = trim(base);
  size_t sep = base.find(" - ");
  std::string title;
  if (sep != std::string::npos) {
    t.artist = trim(base.substr(0, sep));
    title = base.substr(sep + 3);
  } else {
    std::string dir = path.substr(0, path.rfind('/'));
    std::string parent = baseName(dir);
    if (!in(kGenericFolders, 6, asciiLower(parent))) t.artist = parent;
    title = base;
  }
  // ^\d{1,3}[ .-]+
  size_t i = 0;
  while (i < title.size() && i < 3 && isdigit((unsigned char)title[i])) i++;
  size_t j = i;
  while (i > 0 && j < title.size() && (title[j] == ' ' || title[j] == '.' || title[j] == '-')) j++;
  if (i > 0 && j > i) title = title.substr(j);
  title = trim(title);
  t.title = title.empty() ? base : title;
  setKey(t);
  return t;
}

// a file's tags over its name-based entry
void applyTags(Track& t, const Tags& g) {
  if (!g.title.empty()) t.title = g.title;
  if (!g.artist.empty()) t.artist = g.artist;
  else if (!g.albumArtist.empty()) t.artist = g.albumArtist;
  if (!g.album.empty()) t.album = g.album;
  t.albumArtist = g.albumArtist, t.genre = g.genre;
  t.track = g.track, t.disc = g.disc, t.year = g.year;
  if (g.durationSec >= 0) t.durationSec = g.durationSec;
  t.tagged = true;
  t.dirty = true;
  setKey(t);
}

bool writeRows(sqlite3* db, const std::vector<Track>& rows, const std::vector<std::string>& removed);

struct TagResult {
  std::string path;
  int64_t size, modified;
  Tags tags;
  bool hasBook = false;  // an audiobook's file: what FFmpeg read of it (its chapters, its length)
  BookFileInfo book;
};

struct BookInfoRow {
  std::string path;
  int64_t size, modified;
  BookFileInfo info;
};
bool writeBookInfo(sqlite3* db, const std::vector<BookInfoRow>& rows);

}  // namespace

bool isAudioExtension(const std::string& ext) { return in(kAudio, sizeof(kAudio) / sizeof(kAudio[0]), ext); }

// The library's thread: jobs in order (a scan, then the saves), so the database has one writer
struct Library::Worker {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  std::thread thread;
  bool stop = false, started = false;
  // what the running scan found, for the UI thread
  bool listed = false, tagsDone = false;
  std::vector<FileEntry> listing;
  std::vector<std::string> roots;  // the roots the listing covers
  std::deque<std::vector<TagResult>> batches;

  void post(std::function<void()> job) {
    std::lock_guard<std::mutex> l(mu);
    jobs.push_back(std::move(job));
    if (!started) {
      started = true;
      thread = std::thread([this] { run(); });
    }
    cv.notify_one();
  }
  void run() {
    prctl(PR_SET_NAME, "rondo-library");
    setpriority(PRIO_PROCESS, 0, 10);  // below the UI: the card is read while the screen moves
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [this] { return stop || !jobs.empty(); });
        if (jobs.empty()) return;  // stopping, with nothing left
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
  }
  ~Worker() {
    {
      std::lock_guard<std::mutex> l(mu);
      stop = true;
      cv.notify_one();
    }
    if (thread.joinable()) thread.join();  // the last save is written first
  }
};

Library::Library() : worker_(new Worker) {}

Library::~Library() {
  worker_.reset();
  if (db_) sqlite3_close(db_);
}

void Library::scanAsync() {
  if (scanning_) return;
  scanning_ = true;
  // the files already read, as they were: unchanged, their tags aren't read again
  auto known = std::make_shared<std::unordered_map<std::string, std::pair<int64_t, int64_t>>>();
  known->reserve(tracks_.size());
  for (const Track& t : tracks_)
    // a book's file whose chapters weren't read yet (from before Rondo knew books) is read again
    if (!t.gone && t.tagged && !(t.book && !bookInfoFor_.count(t.path))) (*known)[t.path] = {t.size, t.modified / 1000};
  Worker* w = worker_.get();
  w->post([w, known] {
    // a root that isn't mounted (the card's mount still to come at boot, no card in) is an empty
    // folder: its files aren't gone, so it is left out of what the listing covers
    std::vector<FileEntry> found;
    std::vector<std::string> roots;
    for (const char* root : kRoots) {
      size_t before = found.size();
      list(root, 0, found);
      if (found.size() > before || mounted(root)) roots.push_back(root);
    }
    std::vector<FileEntry> need;
    for (const FileEntry& f : found) {
      auto it = known->find(f.path);
      if (it == known->end() || it->second.first != f.size || it->second.second != f.modified / 1000) need.push_back(f);
    }
    {
      std::lock_guard<std::mutex> l(w->mu);
      w->listing = std::move(found);
      w->roots = std::move(roots);
      w->listed = true;
    }
    // the tags, 40 files at a time (LocalLibrary.kt's batches): the list redraws as they come
    std::vector<TagResult> batch;
    for (const FileEntry& f : need) {
      batch.push_back({f.path, f.size, f.modified, rondo::readTags(f.path.c_str()), false, {}});
      TagResult& r = batch.back();
      if (!f.video && (bookCandidate(f.path) || isBookGenre(r.tags.genre))) r.hasBook = readBookInfo(f.path.c_str(), r.book);
      if (batch.size() == 40) {
        std::lock_guard<std::mutex> l(w->mu);
        w->batches.push_back(std::move(batch));
        batch.clear();
      }
    }
    std::lock_guard<std::mutex> l(w->mu);
    if (!batch.empty()) w->batches.push_back(std::move(batch));
    w->tagsDone = true;
  });
}

void Library::rescanIfStale() {
  if (!scanning_ && (scanEnded_ == 0 || now_ns() - scanEnded_ > 30 * kSec)) scanAsync();
}

int Library::presentRoots() const {
  int bits = 0;
  for (size_t i = 0; i < sizeof(kRoots) / sizeof(kRoots[0]); i++)
    if (mounted(kRoots[i])) bits |= 1 << i;
  return bits;
}

bool Library::applyScan() {
  if (!scanning_) return false;
  Worker* w = worker_.get();
  std::vector<FileEntry> listing;
  std::vector<std::string> roots;
  std::vector<TagResult> batch;
  bool gotListing = false, gotBatch = false, done = false;
  {
    std::lock_guard<std::mutex> l(w->mu);
    if (w->listed) {
      listing.swap(w->listing);
      roots.swap(w->roots);
      w->listed = false;
      gotListing = true;
    } else if (!w->batches.empty()) {
      batch = std::move(w->batches.front());
      w->batches.pop_front();
      gotBatch = true;
    } else if (w->tagsDone) {
      w->tagsDone = false;
      done = true;
    }
  }
  if (gotListing) {
    uint64_t before = generation_;
    merge(listing, roots);
    return generation_ != before;
  }
  if (gotBatch) {
    std::unordered_map<std::string, Track*> byPath;
    byPath.reserve(tracks_.size());
    for (Track& t : tracks_)
      if (!t.gone) byPath[t.path] = &t;
    for (TagResult& r : batch) {
      auto it = byPath.find(r.path);
      // the file as it was read (not changed again since)
      if (it != byPath.end() && it->second->size == r.size && it->second->modified / 1000 == r.modified / 1000) {
        applyTags(*it->second, r.tags);
        if (r.hasBook) {
          bookInfo_[r.path] = std::move(r.book);
          bookInfoFor_[r.path] = {r.size, r.modified};
          bookInfoDirty_.push_back(r.path);
        }
      }
    }
    indexed_ = false;
    generation_++;
    return true;
  }
  if (!done) return false;
  scanning_ = false;
  scanEnded_ = now_ns();
  saveAsync();
  return true;  // "Scanning…" goes
}

void Library::saveAsync() {
  auto rows = std::make_shared<std::vector<Track>>();
  for (Track& t : tracks_)
    if (t.dirty) rows->push_back(t), t.dirty = false;
  auto removed = std::make_shared<std::vector<std::string>>(std::move(removed_));
  removed_.clear();
  auto books = std::make_shared<std::vector<BookInfoRow>>();
  for (const std::string& p : bookInfoDirty_) {
    auto it = bookInfo_.find(p);
    auto at = bookInfoFor_.find(p);
    if (it != bookInfo_.end() && at != bookInfoFor_.end()) books->push_back({p, at->second.first, at->second.second, it->second});
  }
  bookInfoDirty_.clear();
  if (rows->empty() && removed->empty() && books->empty()) return;
  sqlite3* db = db_;
  worker_->post([db, rows, removed, books] {
    writeRows(db, *rows, *removed);
    if (!books->empty()) writeBookInfo(db, *books);
  });
}

bool Library::open(const char* path) {
  if (sqlite3_open(path, &db_) != SQLITE_OK) return false;
  sqlite3_exec(db_,
               "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
               "CREATE TABLE IF NOT EXISTS tracks (path TEXT PRIMARY KEY, size INTEGER, modified INTEGER,"
               " video INTEGER, title TEXT, artist TEXT, album TEXT, track INTEGER, duration INTEGER, tagged INTEGER);"
               "CREATE TABLE IF NOT EXISTS stats (path TEXT PRIMARY KEY, plays INTEGER, last_played INTEGER, rating INTEGER);"
               // audiobooks (core/books.h): what FFmpeg read of their files, where each book was left, bookmarks
               "CREATE TABLE IF NOT EXISTS book_files (path TEXT PRIMARY KEY, size INTEGER, modified INTEGER, duration_ms INTEGER,"
               " narrator TEXT, chapters TEXT);"
               "CREATE TABLE IF NOT EXISTS book_state (key TEXT PRIMARY KEY, file TEXT, position INTEGER, speed INTEGER,"
               " finished INTEGER, last_played INTEGER);"
               "CREATE TABLE IF NOT EXISTS bookmarks (key TEXT, file TEXT, position INTEGER, created INTEGER);",
               nullptr, nullptr, nullptr);
  // version 2 (Nov 10): the album artist, genre, disc and year kept; every song's tags read again
  int version = 0;
  sqlite3_stmt* v = nullptr;
  if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &v, nullptr) == SQLITE_OK && sqlite3_step(v) == SQLITE_ROW)
    version = sqlite3_column_int(v, 0);
  sqlite3_finalize(v);
  if (version < 2)
    sqlite3_exec(db_,
                 "BEGIN; ALTER TABLE tracks ADD COLUMN album_artist TEXT; ALTER TABLE tracks ADD COLUMN genre TEXT;"
                 " ALTER TABLE tracks ADD COLUMN disc INTEGER; ALTER TABLE tracks ADD COLUMN year INTEGER;"
                 " UPDATE tracks SET tagged = 0 WHERE video = 0; PRAGMA user_version = 2; COMMIT;",
                 nullptr, nullptr, nullptr);
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db_,
                     "SELECT path, size, modified, video, title, artist, album, track, duration, tagged, album_artist, genre,"
                     " disc, year FROM tracks",
                     -1, &st, nullptr);
  auto text = [&](int i) {
    const unsigned char* s = sqlite3_column_text(st, i);
    return s ? std::string((const char*)s) : std::string();
  };
  while (sqlite3_step(st) == SQLITE_ROW) {
    Track t;
    t.path = text(0);
    t.size = sqlite3_column_int64(st, 1);
    t.modified = sqlite3_column_int64(st, 2);
    t.video = sqlite3_column_int(st, 3) != 0;
    t.title = text(4), t.artist = text(5), t.album = text(6);
    t.track = sqlite3_column_int(st, 7);
    t.durationSec = sqlite3_column_type(st, 8) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 8);
    t.tagged = sqlite3_column_int(st, 9) != 0;
    t.albumArtist = text(10), t.genre = text(11);
    t.disc = sqlite3_column_int(st, 12), t.year = sqlite3_column_int(st, 13);
    t.dirty = false;
    setKey(t);
    tracks_.push_back(std::move(t));
  }
  sqlite3_finalize(st);
  // plays and ratings, by path
  std::unordered_map<std::string, Track*> byPath;
  for (Track& t : tracks_) byPath[t.path] = &t;
  if (sqlite3_prepare_v2(db_, "SELECT path, plays, last_played, rating FROM stats", -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      auto it = byPath.find(text(0));
      if (it == byPath.end()) continue;
      it->second->plays = sqlite3_column_int(st, 1);
      it->second->lastPlayed = sqlite3_column_int64(st, 2);
      it->second->rating = sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
  }
  // the audiobooks' files, places and bookmarks
  if (sqlite3_prepare_v2(db_, "SELECT path, size, modified, duration_ms, narrator, chapters FROM book_files", -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      std::string path = text(0);
      BookFileInfo info;
      info.durationMs = sqlite3_column_type(st, 3) == SQLITE_NULL ? -1 : sqlite3_column_int64(st, 3);
      info.narrator = text(4);
      info.chapters = decodeChapters(text(5));
      bookInfoFor_[path] = {sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2)};
      bookInfo_[path] = std::move(info);
    }
    sqlite3_finalize(st);
  }
  // a book file changed since it was read: read again by the next scan
  for (auto it = bookInfoFor_.begin(); it != bookInfoFor_.end();) {
    auto t = byPath.find(it->first);
    if (t == byPath.end() || t->second->size != it->second.first || t->second->modified / 1000 != it->second.second / 1000) {
      bookInfo_.erase(it->first);
      it = bookInfoFor_.erase(it);
    } else {
      ++it;
    }
  }
  if (sqlite3_prepare_v2(db_, "SELECT key, file, position, speed, finished, last_played FROM book_state", -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      BookState b;
      b.key = text(0), b.file = text(1);
      b.positionMs = sqlite3_column_int64(st, 2);
      b.speed = clampSpeed(sqlite3_column_int(st, 3));
      b.finished = sqlite3_column_int(st, 4) != 0;
      b.lastPlayed = sqlite3_column_int64(st, 5);
      bookStates_[b.key] = b;
    }
    sqlite3_finalize(st);
  }
  if (sqlite3_prepare_v2(db_, "SELECT key, file, position, created FROM bookmarks ORDER BY created", -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      Bookmark b;
      b.key = text(0), b.file = text(1);
      b.positionMs = sqlite3_column_int64(st, 2);
      b.created = sqlite3_column_int64(st, 3);
      bookmarks_[b.key].push_back(b);
    }
    sqlite3_finalize(st);
  }
  return true;
}

int Library::importIndexJson(const char* path) {
  if (!tracks_.empty()) return 0;
  yyjson_doc* doc = yyjson_read_file(path, 0, nullptr, nullptr);
  yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
  size_t i, n;
  yyjson_val* e;
  auto str = [](yyjson_val* o, const char* k) {
    yyjson_val* v = yyjson_obj_get(o, k);
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v)) : std::string();
  };
  yyjson_arr_foreach(root, i, n, e) {
    Track t;
    t.path = str(e, "path");
    t.size = yyjson_get_sint(yyjson_obj_get(e, "size"));
    t.modified = yyjson_get_sint(yyjson_obj_get(e, "modified"));
    t.video = yyjson_get_bool(yyjson_obj_get(e, "video"));
    t.title = str(e, "title"), t.artist = str(e, "artist"), t.album = str(e, "album");
    t.track = int(yyjson_get_sint(yyjson_obj_get(e, "track")));
    yyjson_val* d = yyjson_obj_get(e, "durationSec");
    t.durationSec = yyjson_is_num(d) ? int(yyjson_get_num(d)) : -1;
    t.tagged = yyjson_get_bool(yyjson_obj_get(e, "tagged"));
    setKey(t);
    if (!t.path.empty()) tracks_.push_back(std::move(t));
  }
  if (doc) yyjson_doc_free(doc);
  return int(tracks_.size());
}

void Library::list(const std::string& dir, int depth, std::vector<FileEntry>& out) {
  if (depth > 6) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (!in(kSkip, 4, asciiLower(name))) list(path, depth + 1, out);
      continue;
    }
    size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : asciiLower(name.substr(dot + 1));
    bool video = in(kVideo, 6, ext);
    if (!video && !isAudioExtension(ext)) continue;
    out.push_back({path, int64_t(st.st_size), int64_t(st.st_mtime) * 1000, video});
  }
  closedir(d);
}

void Library::merge(std::vector<FileEntry>& found, const std::vector<std::string>& roots) {
  // in place: a track stays where it is (screens hold pointers to it), a changed file is updated where
  // it is (named from its file again, its tags read again), a new one is added at the end, one that
  // left the card is marked gone (one under a root the listing didn't cover stays)
  std::unordered_map<std::string, Track*> byPath;
  byPath.reserve(tracks_.size());
  for (Track& t : tracks_)
    if (!t.gone) byPath[t.path] = &t;
  std::unordered_map<std::string, bool> now;
  now.reserve(found.size());
  bool changed = false;
  for (FileEntry& f : found) {
    now[f.path] = true;
    auto it = byPath.find(f.path);
    if (it == byPath.end()) {
      tracks_.push_back(fromName(f.path, f.size, f.modified, f.video));
      changed = true;
    } else if (it->second->size != f.size || it->second->modified / 1000 != f.modified / 1000) {
      *it->second = fromName(f.path, f.size, f.modified, f.video);
      changed = true;
    }
  }
  for (Track& t : tracks_)
    if (!t.gone && !now.count(t.path) && under(t.path, roots)) {
      t.gone = true;
      removed_.push_back(t.path);
      changed = true;
    }
  // the card as it was (most rescans): nothing to sort again, no screen to rebuild
  if (!changed) return;
  indexed_ = false;
  generation_++;
}

void Library::scan(const std::vector<std::string>& roots) {
  std::vector<FileEntry> found;
  for (const std::string& r : roots) list(r, 0, found);
  merge(found, roots);
}

int Library::readTags(int max) {
  int done = 0;
  for (Track& t : tracks_) {
    if (t.tagged) continue;
    if (max >= 0 && done >= max) break;
    applyTags(t, rondo::readTags(t.path.c_str()));
    done++;
  }
  if (done) indexed_ = false, generation_++;
  return done;
}


bool Library::save() {
  std::vector<Track> rows;
  for (Track& t : tracks_)
    if (t.dirty) rows.push_back(t), t.dirty = false;
  bool ok = writeRows(db_, rows, removed_);
  removed_.clear();
  return ok;
}

namespace {
// One transaction: the paths that left, then the rows that changed. Whichever thread writes is the
// only one using the database meanwhile (open() reads it before the worker starts).
bool writeRows(sqlite3* db_, const std::vector<Track>& rows, const std::vector<std::string>& removed_) {
  if (!db_) return false;
  sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
  sqlite3_stmt* del = nullptr;
  sqlite3_prepare_v2(db_, "DELETE FROM tracks WHERE path = ?", -1, &del, nullptr);
  for (const std::string& p : removed_) {
    sqlite3_bind_text(del, 1, p.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(del);
    sqlite3_reset(del);
  }
  sqlite3_finalize(del);
  sqlite3_stmt* put = nullptr;
  sqlite3_prepare_v2(db_,
                     "INSERT OR REPLACE INTO tracks (path, size, modified, video, title, artist, album, track, duration,"
                     " tagged, album_artist, genre, disc, year) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                     -1, &put, nullptr);
  for (const Track& t : rows) {
    sqlite3_bind_text(put, 1, t.path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(put, 2, t.size);
    sqlite3_bind_int64(put, 3, t.modified);
    sqlite3_bind_int(put, 4, t.video);
    sqlite3_bind_text(put, 5, t.title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(put, 6, t.artist.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(put, 7, t.album.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(put, 8, t.track);
    if (t.durationSec >= 0) sqlite3_bind_int(put, 9, t.durationSec);
    else sqlite3_bind_null(put, 9);
    sqlite3_bind_int(put, 10, t.tagged);
    sqlite3_bind_text(put, 11, t.albumArtist.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(put, 12, t.genre.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(put, 13, t.disc);
    sqlite3_bind_int(put, 14, t.year);
    sqlite3_step(put);
    sqlite3_reset(put);
  }
  sqlite3_finalize(put);
  return sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool writeBookInfo(sqlite3* db, const std::vector<BookInfoRow>& rows) {
  if (!db) return false;
  sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
  sqlite3_stmt* put = nullptr;
  sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO book_files (path, size, modified, duration_ms, narrator, chapters) VALUES (?, ?, ?, ?, ?, ?)",
                     -1, &put, nullptr);
  for (const BookInfoRow& r : rows) {
    std::string chapters = encodeChapters(r.info.chapters);
    sqlite3_bind_text(put, 1, r.path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(put, 2, r.size);
    sqlite3_bind_int64(put, 3, r.modified);
    if (r.info.durationMs >= 0) sqlite3_bind_int64(put, 4, r.info.durationMs);
    else sqlite3_bind_null(put, 4);
    sqlite3_bind_text(put, 5, r.info.narrator.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(put, 6, chapters.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(put);
    sqlite3_reset(put);
  }
  sqlite3_finalize(put);
  return sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}
}  // namespace

namespace {
void byTitle(std::vector<const Track*>& v) {
  std::vector<std::pair<std::string, const Track*>> k;
  for (auto* t : v) k.emplace_back(lower(t->title), t);
  std::stable_sort(k.begin(), k.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < v.size(); i++) v[i] = k[i].second;
}
}  // namespace

std::vector<Library::Group> Library::albumArtists() const {
  index();
  return albumArtists_;
}

std::vector<Library::Group> Library::genres() const {
  index();
  return genres_;
}

int Library::yearOf(const std::vector<const Track*>& songs) {
  std::unordered_map<int, int> count;  // the year most of them carry
  int best = 0, most = 0;
  for (const Track* t : songs)
    if (t->year && ++count[t->year] > most) most = count[t->year], best = t->year;
  return best;
}

namespace {
int64_t wallMs() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
}  // namespace

void Library::saveStats(const Track& t) {
  sqlite3* db = db_;
  std::string path = t.path;
  int plays = t.plays, rating = t.rating;
  int64_t last = t.lastPlayed;
  worker_->post([db, path, plays, last, rating] {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO stats (path, plays, last_played, rating) VALUES (?, ?, ?, ?)", -1, &st,
                           nullptr) != SQLITE_OK)
      return;
    sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, plays);
    sqlite3_bind_int64(st, 3, last);
    sqlite3_bind_int(st, 4, rating);
    sqlite3_step(st);
    sqlite3_finalize(st);
  });
}

void Library::notePlayed(const std::string& path) {
  for (Track& t : tracks_)
    if (!t.gone && t.path == path) {
      t.plays++;
      t.lastPlayed = wallMs();
      saveStats(t);
      generation_++;
      return;
    }
}

bool Library::setRating(const std::string& path, int stars) {
  if (stars < 0 || stars > 5) return false;
  for (Track& t : tracks_)
    if (!t.gone && t.path == path) {
      t.rating = stars;
      saveStats(t);
      generation_++;
      return true;
    }
  return false;
}

namespace {
std::vector<const Track*> songsWhere(const std::deque<Track>& tracks, const std::function<bool(const Track&)>& keep) {
  std::vector<const Track*> v;
  for (const Track& t : tracks)
    if (!t.gone && !t.video && !t.book && keep(t)) v.push_back(&t);
  return v;
}
}  // namespace

std::vector<const Track*> Library::mostPlayed(size_t max) const {
  std::vector<const Track*> v = songsWhere(tracks_, [](const Track& t) { return t.plays > 0; });
  std::stable_sort(v.begin(), v.end(), [](const Track* a, const Track* b) {
    return a->plays != b->plays ? a->plays > b->plays : a->lastPlayed > b->lastPlayed;
  });
  if (v.size() > max) v.resize(max);
  return v;
}

std::vector<const Track*> Library::recentlyPlayed(size_t max) const {
  std::vector<const Track*> v = songsWhere(tracks_, [](const Track& t) { return t.lastPlayed > 0; });
  std::stable_sort(v.begin(), v.end(), [](const Track* a, const Track* b) { return a->lastPlayed > b->lastPlayed; });
  if (v.size() > max) v.resize(max);
  return v;
}

std::vector<const Track*> Library::topRated() const {
  std::vector<const Track*> v = songsWhere(tracks_, [](const Track& t) { return t.rating >= 4; });
  std::stable_sort(v.begin(), v.end(), [](const Track* a, const Track* b) {
    return a->rating != b->rating ? a->rating > b->rating : a->plays > b->plays;
  });
  return v;
}

std::vector<const Track*> Library::neverPlayed() const {
  std::vector<const Track*> v = songsWhere(tracks_, [](const Track& t) { return t.plays == 0; });
  byTitle(v);
  return v;
}

bool Library::moved(const std::string& from, const std::string& to) {
  for (Track& t : tracks_)
    if (!t.gone && t.path == from) {
      removed_.push_back(from);
      t.path = to;
      t.dirty = true;
      setKey(t);
      if (auto it = bookInfo_.find(from); it != bookInfo_.end()) {  // what was read of a book file follows it
        bookInfo_[to] = std::move(it->second);
        bookInfo_.erase(from);
        bookInfoFor_[to] = bookInfoFor_[from];
        bookInfoFor_.erase(from);
        bookInfoDirty_.push_back(to);
      }
      sqlite3* db = db_;
      worker_->post([db, from, to] {  // the stats row follows (written after the tracks: same thread)
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "UPDATE OR REPLACE stats SET path = ? WHERE path = ?", -1, &st, nullptr) != SQLITE_OK) return;
        sqlite3_bind_text(st, 1, to.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, from.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
      });
      indexed_ = false;
      generation_++;
      saveAsync();
      return true;
    }
  return false;
}

bool Library::retagged(const std::string& path, const Tags& tags, int64_t size, int64_t modified) {
  for (Track& t : tracks_)
    if (!t.gone && t.path == path) {
      applyTags(t, tags);
      t.size = size, t.modified = modified;
      indexed_ = false;
      generation_++;
      return true;
    }
  return false;
}

std::vector<const Track*> Library::songs() const {
  index();
  return songs_;
}

std::vector<const Track*> Library::videos() const {
  index();
  return videos_;
}

namespace {
// groups sorted case-insensitively by name (TreeMap with CASE_INSENSITIVE_ORDER: names differing only in
// case join the first one seen)
enum class By { Artist, Album, AlbumArtist, Genre };
std::vector<Library::Group> group(const std::deque<Track>& tracks, By by) {
  bool album = by == By::Album;
  std::vector<Library::Group> out;
  std::unordered_map<std::string, size_t> at;
  std::vector<std::string> keys;  // lower() of each group's name, computed once
  for (const Track& t : tracks) {
    if (t.gone || t.video || t.book || (album && t.album.empty()) || (by == By::Genre && t.genre.empty())) continue;
    const std::string& artist = by == By::AlbumArtist && !t.albumArtist.empty() ? t.albumArtist : t.artist;
    // "Unknown artist" stays English here: it's the group's name, which opens, sorts and finds it; the
    // screens show it in the language spoken (shell/library_screens.cpp, groupTitle)
    std::string name = album ? t.album : by == By::Genre ? t.genre : (artist.empty() ? "Unknown artist" : artist);
    // an album is its name and its album artist: "Greatest Hits" by two artists are two albums
    std::string k = lower(name) + (album && !t.albumArtist.empty() ? "\x1f" + lower(t.albumArtist) : "");
    auto it = at.find(k);
    if (it == at.end()) {
      at[k] = out.size();
      out.push_back({name, {}});
      keys.push_back(k);
      it = at.find(k);
    }
    out[it->second].second.push_back(&t);
  }
  std::vector<size_t> order(out.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });
  std::vector<Library::Group> sorted;
  sorted.reserve(out.size());
  for (size_t i : order) sorted.push_back(std::move(out[i]));
  // inside a group: by album (artists only), track, title; the lower-case keys made once per track
  std::unordered_map<const Track*, std::pair<std::string, std::string>> k;
  for (auto& g : sorted)
    for (const Track* t : g.second) k[t] = {album ? std::string() : lower(t->album), lower(t->title)};
  for (auto& g : sorted)
    std::stable_sort(g.second.begin(), g.second.end(), [&](const Track* a, const Track* b) {
      const auto& ka = k[a];
      const auto& kb = k[b];
      if (ka.first != kb.first) return ka.first < kb.first;
      if (a->disc != b->disc) return a->disc < b->disc;
      if (a->track != b->track) return a->track < b->track;
      return ka.second < kb.second;
    });
  return sorted;
}
}  // namespace

void Library::index() const {
  if (indexed_) return;
  songs_.clear();
  videos_.clear();
  std::vector<BookFile> bookFiles;
  for (const Track& t : tracks_) {
    if (t.gone) continue;
    if (t.book) {
      BookFile f;
      f.path = t.path, f.title = t.title, f.album = t.album, f.artist = t.artist, f.albumArtist = t.albumArtist;
      f.disc = t.disc, f.track = t.track;
      auto info = bookInfo_.find(t.path);
      f.durationMs = info != bookInfo_.end() && info->second.durationMs > 0 ? info->second.durationMs
                     : t.durationSec >= 0                                   ? int64_t(t.durationSec) * 1000
                                                                            : -1;
      f.ownChapters = info != bookInfo_.end() && info->second.chapters.size() >= 2;
      bookFiles.push_back(std::move(f));
      continue;
    }
    (t.video ? videos_ : songs_).push_back(&t);
  }
  byTitle(songs_);
  byTitle(videos_);
  artists_ = group(tracks_, By::Artist);
  albums_ = group(tracks_, By::Album);
  albumArtists_ = group(tracks_, By::AlbumArtist);
  genres_ = group(tracks_, By::Genre);
  byPath_.clear();
  for (const Track& t : tracks_)
    if (!t.gone) byPath_[t.path] = &t;
  books_.clear();
  bookByKey_.clear();
  bookOfPath_.clear();
  bookKeys_.clear();
  for (BookGroup& g : groupBooks(std::move(bookFiles))) {
    Book b;
    b.key = g.key, b.title = g.title, b.author = g.author, b.durationMs = g.durationMs;
    for (const BookFile& f : g.files)
      if (auto it = byPath_.find(f.path); it != byPath_.end()) b.files.push_back(it->second);
    if (b.files.empty()) continue;
    b.cover = b.files.front()->path;
    std::string first = b.cover.substr(0, b.cover.rfind('/'));
    if (first != b.key && b.key != b.cover)  // its files in disc folders: the picture in the book's folder
      for (const char* name : {"cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg", "front.jpg", "cover.png", "folder.png"})
        if (access((b.key + "/" + name).c_str(), R_OK) == 0) {
          b.cover = b.key + "/" + name;
          break;
        }
    bookByKey_[b.key] = books_.size();
    for (const Track* t : b.files) bookOfPath_[t->path] = books_.size();
    bookKeys_.push_back(fold(b.title + " " + b.author));
    books_.push_back(std::move(b));
  }
  artistKeys_.clear();
  albumKeys_.clear();
  for (auto& g : artists_) artistKeys_.push_back(fold(g.first));
  for (auto& g : albums_) albumKeys_.push_back(fold(g.first));
  indexed_ = true;
}

bool Library::deleteFile(const std::string& path) {
  Track* t = nullptr;
  for (Track& x : tracks_)
    if (x.path == path && !x.gone) t = &x;
  if (!t || unlink(path.c_str()) != 0) return false;
  size_t slash = path.rfind('/'), dot = path.rfind('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) unlink((path.substr(0, dot) + ".lrc").c_str());
  t->gone = true;
  removed_.push_back(path);
  indexed_ = false;
  generation_++;
  saveAsync();  // the database on the library's thread
  return true;
}

const Track* Library::find(const std::string& path) const {
  index();
  auto it = byPath_.find(path);
  return it == byPath_.end() ? nullptr : it->second;
}

std::vector<Library::Group> Library::artists() const {
  index();
  return artists_;
}
std::vector<Library::Group> Library::albums() const {
  index();
  return albums_;
}

Library::Found Library::search(const std::string& query) const {
  Found f;
  std::vector<std::string> words;
  std::string q = fold(query), w;
  for (char c : q + " ") {
    if (c == ' ') {
      if (!w.empty()) words.push_back(w);
      w.clear();
    } else {
      w += c;
    }
  }
  if (words.empty()) return f;
  auto hit = [&](const std::string& key) {
    for (const auto& x : words)
      if (key.find(x) == std::string::npos) return false;
    return true;
  };
  index();
  for (size_t i = 0; i < artists_.size(); i++)
    if (hit(artistKeys_[i])) f.artists.push_back(artists_[i]);
  for (size_t i = 0; i < albums_.size(); i++)
    if (hit(albumKeys_[i])) f.albums.push_back(albums_[i]);
  for (const Track& t : tracks_) {
    if (!t.gone && !t.book && hit(t.key)) (t.video ? f.videos : f.songs).push_back(&t);
  }
  for (size_t i = 0; i < books_.size(); i++)
    if (hit(bookKeys_[i])) f.books.push_back(&books_[i]);
  byTitle(f.songs);
  byTitle(f.videos);
  return f;
}

// ---- audiobooks ----

const Book* Library::bookByKey(const std::string& key) const {
  index();
  auto it = bookByKey_.find(key);
  return it == bookByKey_.end() ? nullptr : &books_[it->second];
}

const Book* Library::bookOf(const std::string& path) const {
  index();
  auto it = bookOfPath_.find(path);
  return it == bookOfPath_.end() ? nullptr : &books_[it->second];
}

const BookFileInfo* Library::bookFileInfo(const std::string& path) const {
  auto it = bookInfo_.find(path);
  return it == bookInfo_.end() ? nullptr : &it->second;
}

const BookState* Library::bookState(const std::string& key) const {
  auto it = bookStates_.find(key);
  return it == bookStates_.end() ? nullptr : &it->second;
}

void Library::setBookState(const BookState& s) {
  bookStates_[s.key] = s;
  saveBookState(s);
  bookChanges_++;
}

void Library::saveBookState(const BookState& b) {
  sqlite3* db = db_;
  BookState s = b;
  worker_->post([db, s] {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO book_state (key, file, position, speed, finished, last_played) VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &st, nullptr) != SQLITE_OK)
      return;
    sqlite3_bind_text(st, 1, s.key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, s.file.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, s.positionMs);
    sqlite3_bind_int(st, 4, s.speed);
    sqlite3_bind_int(st, 5, s.finished);
    sqlite3_bind_int64(st, 6, s.lastPlayed);
    sqlite3_step(st);
    sqlite3_finalize(st);
  });
}

const std::vector<Bookmark>& Library::bookmarks(const std::string& key) const {
  static const std::vector<Bookmark> none;
  auto it = bookmarks_.find(key);
  return it == bookmarks_.end() ? none : it->second;
}

void Library::addBookmark(const Bookmark& b) {
  bookmarks_[b.key].push_back(b);
  saveBookmarks(b.key);
  bookChanges_++;
}

void Library::removeBookmark(const std::string& key, int64_t created) {
  auto it = bookmarks_.find(key);
  if (it == bookmarks_.end()) return;
  auto& v = it->second;
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Bookmark& b) { return b.created == created; }), v.end());
  saveBookmarks(key);
  bookChanges_++;
}

void Library::saveBookmarks(const std::string& key) {
  sqlite3* db = db_;
  std::vector<Bookmark> v = bookmarks(key);
  worker_->post([db, key, v] {
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM bookmarks WHERE key = ?", -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_STATIC);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "INSERT INTO bookmarks (key, file, position, created) VALUES (?, ?, ?, ?)", -1, &st, nullptr) == SQLITE_OK) {
      for (const Bookmark& b : v) {
        sqlite3_bind_text(st, 1, b.key.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, b.file.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, b.positionMs);
        sqlite3_bind_int64(st, 4, b.created);
        sqlite3_step(st);
        sqlite3_reset(st);
      }
      sqlite3_finalize(st);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
  });
}

void Library::bookMoved(const std::string& from, const std::string& to) {
  auto follow = [&](std::string& p) {
    if (p == from) p = to;
    else if (p.size() > from.size() && p.compare(0, from.size() + 1, from + "/") == 0) p = to + p.substr(from.size());
  };
  std::vector<std::string> keys;
  for (auto& kv : bookStates_) keys.push_back(kv.first);
  for (const std::string& k : keys) {
    BookState s = bookStates_[k];
    std::string nk = k;
    follow(nk);
    std::string before = s.file;
    follow(s.file);
    if (nk == k && s.file == before) continue;
    s.key = nk;
    bookStates_.erase(k);
    bookStates_[nk] = s;
    sqlite3* db = db_;
    std::string old = k;
    worker_->post([db, old] {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(db, "DELETE FROM book_state WHERE key = ?", -1, &st, nullptr) != SQLITE_OK) return;
      sqlite3_bind_text(st, 1, old.c_str(), -1, SQLITE_STATIC);
      sqlite3_step(st);
      sqlite3_finalize(st);
    });
    saveBookState(s);
  }
  keys.clear();
  for (auto& kv : bookmarks_) keys.push_back(kv.first);
  for (const std::string& k : keys) {
    std::vector<Bookmark> v = bookmarks_[k];
    std::string nk = k;
    follow(nk);
    bool changed = nk != k;
    for (Bookmark& b : v) {
      std::string before = b.file;
      follow(b.file);
      b.key = nk;
      changed |= b.file != before;
    }
    if (!changed) continue;
    bookmarks_.erase(k);
    bookmarks_[k].clear();
    saveBookmarks(k);  // the old key's rows go
    bookmarks_.erase(k);
    bookmarks_[nk] = v;
    saveBookmarks(nk);
  }
  bookChanges_++;
}

void Library::flush(int ms) {
  struct Done {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
  };
  auto d = std::make_shared<Done>();
  worker_->post([d] {
    std::lock_guard<std::mutex> l(d->mu);
    d->done = true;
    d->cv.notify_all();
  });
  std::unique_lock<std::mutex> l(d->mu);
  d->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return d->done; });
}

}  // namespace rondo
