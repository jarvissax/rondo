#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/prctl.h>
#include <sys/resource.h>

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <sqlite3.h>
#include <yyjson.h>

#include "clock.h"
#include "fold.h"
#include "tags.h"

namespace rondo {
namespace {

const char* kAudio[] = {"flac", "mp3", "m4a", "aac", "ogg", "wav", "opus"};
const char* kVideo[] = {"mp4", "m4v", "mkv", "3gp", "webm"};
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

// what search matches: a song's title, artist, album and file name; a video's title and file name
void setKey(Track& t) {
  t.key = t.video ? fold(t.title + " " + baseName(t.path)) : fold(t.title + " " + t.artist + " " + t.album + " " + baseName(t.path));
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
  t.track = g.track;
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
};

}  // namespace

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
    if (!t.gone && t.tagged) (*known)[t.path] = {t.size, t.modified / 1000};
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
      batch.push_back({f.path, f.size, f.modified, rondo::readTags(f.path.c_str())});
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
      if (it != byPath.end() && it->second->size == r.size && it->second->modified / 1000 == r.modified / 1000)
        applyTags(*it->second, r.tags);
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
  if (rows->empty() && removed->empty()) return;
  sqlite3* db = db_;
  worker_->post([db, rows, removed] { writeRows(db, *rows, *removed); });
}

bool Library::open(const char* path) {
  if (sqlite3_open(path, &db_) != SQLITE_OK) return false;
  sqlite3_exec(db_,
               "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
               "CREATE TABLE IF NOT EXISTS tracks (path TEXT PRIMARY KEY, size INTEGER, modified INTEGER,"
               " video INTEGER, title TEXT, artist TEXT, album TEXT, track INTEGER, duration INTEGER, tagged INTEGER);",
               nullptr, nullptr, nullptr);
  sqlite3_stmt* st = nullptr;
  sqlite3_prepare_v2(db_, "SELECT path, size, modified, video, title, artist, album, track, duration, tagged FROM tracks",
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
    t.dirty = false;
    setKey(t);
    tracks_.push_back(std::move(t));
  }
  sqlite3_finalize(st);
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
    bool video = in(kVideo, 5, ext);
    if (!video && !in(kAudio, 7, ext)) continue;
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
                     " tagged) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
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
    sqlite3_step(put);
    sqlite3_reset(put);
  }
  sqlite3_finalize(put);
  return sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
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
std::vector<Library::Group> group(const std::deque<Track>& tracks, bool album) {
  std::vector<Library::Group> out;
  std::unordered_map<std::string, size_t> at;
  std::vector<std::string> keys;  // lower() of each group's name, computed once
  for (const Track& t : tracks) {
    if (t.gone || t.video || (album && t.album.empty())) continue;
    std::string name = album ? t.album : (t.artist.empty() ? "Unknown artist" : t.artist);
    std::string k = lower(name);
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
  for (const Track& t : tracks_)
    if (!t.gone) (t.video ? videos_ : songs_).push_back(&t);
  byTitle(songs_);
  byTitle(videos_);
  artists_ = group(tracks_, false);
  albums_ = group(tracks_, true);
  byPath_.clear();
  for (const Track& t : tracks_)
    if (!t.gone) byPath_[t.path] = &t;
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
    if (!t.gone && hit(t.key)) (t.video ? f.videos : f.songs).push_back(&t);
  }
  byTitle(f.songs);
  byTitle(f.videos);
  return f;
}

}  // namespace rondo
