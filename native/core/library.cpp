#include "library.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <unordered_map>

#include <sqlite3.h>
#include <yyjson.h>

#include "fold.h"
#include "tags.h"

namespace rondo {
namespace {

const char* kAudio[] = {"flac", "mp3", "m4a", "aac", "ogg", "wav", "opus"};
const char* kVideo[] = {"mp4", "m4v", "mkv", "3gp", "webm"};
const char* kSkip[] = {"android", "lost.dir", "dcim", ".thumbnails"};
const char* kGenericFolders[] = {"music", "video", "videos", "movies", "sdcard0", "sdcard1"};

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

}  // namespace

Library::~Library() {
  if (db_) sqlite3_close(db_);
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

void Library::walk(const std::string& dir, int depth, std::vector<Track>& out) {
  if (depth > 6) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  static std::unordered_map<std::string, const Track*>* known = nullptr;
  std::unordered_map<std::string, const Track*> local;
  if (depth == 0) {
    for (const Track& t : tracks_) local[t.path] = &t;
    known = &local;
  }
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (!in(kSkip, 4, asciiLower(name))) walk(path, depth + 1, out);
      continue;
    }
    size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? "" : asciiLower(name.substr(dot + 1));
    bool video = in(kVideo, 5, ext);
    if (!video && !in(kAudio, 7, ext)) continue;
    int64_t size = st.st_size, modified = int64_t(st.st_mtime) * 1000;
    auto it = known->find(path);
    if (it != known->end() && it->second->size == size && it->second->modified / 1000 == modified / 1000)
      out.push_back(*it->second);
    else
      out.push_back(fromName(path, size, modified, video));
  }
  closedir(d);
}

void Library::scan(const std::vector<std::string>& roots) {
  std::vector<Track> found;
  std::vector<Track> old = tracks_;
  for (const std::string& r : roots) walk(r, 0, found);
  std::unordered_map<std::string, bool> now;
  for (const Track& t : found) now[t.path] = true;
  for (const Track& t : old)
    if (!now.count(t.path)) removed_.push_back(t.path);
  tracks_ = std::move(found);
  indexed_ = false;
}

int Library::readTags(int max) {
  int done = 0;
  for (Track& t : tracks_) {
    if (t.tagged) continue;
    if (max >= 0 && done >= max) break;
    Tags g = rondo::readTags(t.path.c_str());
    if (!g.title.empty()) t.title = g.title;
    if (!g.artist.empty()) t.artist = g.artist;
    else if (!g.albumArtist.empty()) t.artist = g.albumArtist;
    if (!g.album.empty()) t.album = g.album;
    t.track = g.track;
    if (g.durationSec >= 0) t.durationSec = g.durationSec;
    t.tagged = true;
    t.dirty = true;
    setKey(t);
    done++;
  }
  if (done) indexed_ = false;
  return done;
}


bool Library::save() {
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
  removed_.clear();
  sqlite3_stmt* put = nullptr;
  sqlite3_prepare_v2(db_,
                     "INSERT OR REPLACE INTO tracks (path, size, modified, video, title, artist, album, track, duration,"
                     " tagged) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                     -1, &put, nullptr);
  for (Track& t : tracks_) {
    if (!t.dirty) continue;
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
    t.dirty = false;
  }
  sqlite3_finalize(put);
  return sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}

namespace {
void byTitle(std::vector<const Track*>& v) {
  std::vector<std::pair<std::string, const Track*>> k;
  for (auto* t : v) k.emplace_back(lower(t->title), t);
  std::stable_sort(k.begin(), k.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < v.size(); i++) v[i] = k[i].second;
}
}  // namespace

std::vector<const Track*> Library::songs() const {
  std::vector<const Track*> v;
  for (const Track& t : tracks_)
    if (!t.video) v.push_back(&t);
  byTitle(v);
  return v;
}

std::vector<const Track*> Library::videos() const {
  std::vector<const Track*> v;
  for (const Track& t : tracks_)
    if (t.video) v.push_back(&t);
  byTitle(v);
  return v;
}

namespace {
// groups sorted case-insensitively by name (TreeMap with CASE_INSENSITIVE_ORDER: names differing only in
// case join the first one seen)
std::vector<Library::Group> group(const std::vector<Track>& tracks, bool album) {
  std::vector<Library::Group> out;
  std::unordered_map<std::string, size_t> at;
  for (const Track& t : tracks) {
    if (t.video || (album && t.album.empty())) continue;
    std::string name = album ? t.album : (t.artist.empty() ? "Unknown artist" : t.artist);
    std::string k = lower(name);
    auto it = at.find(k);
    if (it == at.end()) {
      at[k] = out.size();
      out.push_back({name, {}});
      it = at.find(k);
    }
    out[it->second].second.push_back(&t);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
  for (auto& g : out)
    std::stable_sort(g.second.begin(), g.second.end(), [&](const Track* a, const Track* b) {
      if (!album) {
        std::string la = lower(a->album), lb = lower(b->album);
        if (la != lb) return la < lb;
      }
      if (a->track != b->track) return a->track < b->track;
      return lower(a->title) < lower(b->title);
    });
  return out;
}
}  // namespace

void Library::index() const {
  if (indexed_) return;
  artists_ = group(tracks_, false);
  albums_ = group(tracks_, true);
  artistKeys_.clear();
  albumKeys_.clear();
  for (auto& g : artists_) artistKeys_.push_back(fold(g.first));
  for (auto& g : albums_) albumKeys_.push_back(fold(g.first));
  indexed_ = true;
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
    if (hit(t.key)) (t.video ? f.videos : f.songs).push_back(&t);
  }
  byTitle(f.songs);
  byTitle(f.videos);
  return f;
}

}  // namespace rondo
