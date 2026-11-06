// The card library (today's LocalLibrary.kt, natively): music and videos on the microSD card and the
// internal FAT partition, found by walking the folders (the firmware has no media provider). A scan
// lists files at once with titles from their names ("Artist - Title.flac"); tags are read afterwards and
// kept in a SQLite index, so only new or changed files are read again.
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace rondo {

struct Track {
  std::string path;
  int64_t size = 0, modified = 0;  // modified: ms since 1970, like Java's File.lastModified()
  bool video = false;
  std::string title, artist, album;
  int track = 0;
  int durationSec = -1;  // -1: unknown
  bool tagged = false;
  std::string key;  // fold() of what search matches
  bool dirty = true;
};

class Library {
 public:
  ~Library();
  // The index database; created when missing.
  bool open(const char* dbPath);
  // 1.0.x's files/local-index.json, read when the index is empty (migration).
  int importIndexJson(const char* path);
  // Walks the roots. Unchanged files keep their entries; new or changed ones get name-based entries.
  void scan(const std::vector<std::string>& roots = {"/storage/sdcard0", "/storage/sdcard1"});
  // Reads the tags of up to `max` entries not tagged yet (-1: all); returns how many were read.
  int readTags(int max = -1);
  // Writes what changed since the last save, in one transaction.
  bool save();

  const std::vector<Track>& tracks() const { return tracks_; }
  std::vector<const Track*> songs() const;   // sorted by lower-case title
  std::vector<const Track*> videos() const;
  using Group = std::pair<std::string, std::vector<const Track*>>;
  std::vector<Group> artists() const;  // songs by artist ("Unknown artist"), by album, track, title
  std::vector<Group> albums() const;

  struct Found {
    std::vector<Group> artists, albums;
    std::vector<const Track*> songs, videos;
  };
  // Every word must be in the title, artist, album or file name, ignoring case and accents.
  Found search(const std::string& query) const;

 private:
  void walk(const std::string& dir, int depth, std::vector<Track>& out);
  sqlite3* db_ = nullptr;
  std::vector<Track> tracks_;
  std::vector<std::string> removed_;
};

}  // namespace rondo
