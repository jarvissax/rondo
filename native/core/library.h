// The card library (today's LocalLibrary.kt, natively): music and videos on the microSD card and the
// internal FAT partition, found by walking the folders (the firmware has no media provider). A scan
// lists files at once with titles from their names ("Artist - Title.flac"); tags are read afterwards and
// kept in a SQLite index, so only new or changed files are read again.
#pragma once
#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;

namespace rondo {

struct Track {
  std::string path;
  int64_t size = 0, modified = 0;  // modified: ms since 1970, like Java's File.lastModified()
  bool video = false;
  std::string title, artist, album;
  std::string albumArtist, genre;  // "" not tagged
  int track = 0;
  int disc = 0, year = 0;  // 0: not tagged
  int durationSec = -1;  // -1: unknown
  // how it's been listened to (kept apart from the tags: a rescan never touches them)
  int plays = 0;
  int64_t lastPlayed = 0;  // ms since 1970, 0: never
  int rating = 0;          // 0 (none) to 5 stars
  bool tagged = false;
  std::string key;  // fold() of what search matches
  bool dirty = true;
  bool gone = false;  // deleted from the card in this run: kept, so screens' pointers stay valid
};

// a file the walk found
struct FileEntry {
  std::string path;
  int64_t size = 0, modified = 0;
  bool video = false;
};

class Library {
 public:
  Library();
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

  // The same on a thread of its own (LocalLibrary.kt's background scan), at a lower priority than
  // the UI: the walk, then the tags of new and changed files. What it finds is applied by applyScan()
  // on the UI thread, the file list at once and the tags 40 at a time, and saved there when it ends
  // (the database write on the thread too). Nothing happens if a scan runs already.
  void scanAsync();
  // which roots are mounted (bit 0 the card, bit 1 the internal storage): one appearing is a reason
  // to scan (the boot's mount finishing after the start, a card put in)
  int presentRoots() const;
  // A scan when the last one ended over 30 s ago (1.0.x: Library, its search or a playlist opening)
  void rescanIfStale();
  bool scanning() const { return scanning_; }
  // From the UI thread's loop: true when the library changed (screens redraw)
  bool applyScan();
  // Grows with every change of what the library holds (a scan's files, tags, a delete)
  uint64_t generation() const { return generation_; }
  // The database write of what changed, on the thread (deleteFile uses it)
  void saveAsync();

  // Tracks never move once added (a deque; a song that leaves the card is marked gone): screens keep
  // pointers to them across a rescan.
  const std::deque<Track>& tracks() const { return tracks_; }
  std::vector<const Track*> songs() const;   // sorted by lower-case title
  std::vector<const Track*> videos() const;
  using Group = std::pair<std::string, std::vector<const Track*>>;
  std::vector<Group> artists() const;  // songs by artist ("Unknown artist"), by album, track, title
  // by album and album artist (two albums of one name by different artists stay apart), by disc, track
  std::vector<Group> albums() const;
  std::vector<Group> albumArtists() const;  // by the album artist (else the artist), by album, disc, track
  std::vector<Group> genres() const;        // songs with a genre, by genre
  // The year an album's songs carry (0: none): for "Album (2019)"
  static int yearOf(const std::vector<const Track*>& songs);

  // Listening: a song played through (Now Playing calls it once a song is half over, or 4 min in),
  // its rating; both kept in the database's stats table, by path.
  void notePlayed(const std::string& path);
  bool setRating(const std::string& path, int stars);
  // Smart lists: the most played, the last played, the best rated (4 stars and up), never played
  std::vector<const Track*> mostPlayed(size_t max = 100) const;
  std::vector<const Track*> recentlyPlayed(size_t max = 100) const;
  std::vector<const Track*> topRated() const;
  std::vector<const Track*> neverPlayed() const;
  // A file that moved on the card (organized, renamed, moved by the dashboard): its entry, plays and
  // rating follow it. false: no track at `from`.
  bool moved(const std::string& from, const std::string& to);
  // how many of each, without copying them (the Library screen's counts)
  size_t songCount() const { index(); return songs_.size(); }
  size_t videoCount() const { index(); return videos_.size(); }
  size_t artistCount() const { index(); return artists_.size(); }
  size_t albumCount() const { index(); return albums_.size(); }
  size_t albumArtistCount() const { index(); return albumArtists_.size(); }
  size_t genreCount() const { index(); return genres_.size(); }

  struct Found {
    std::vector<Group> artists, albums;
    std::vector<const Track*> songs, videos;
  };
  // Every word must be in the title, artist, album or file name, ignoring case and accents.
  Found search(const std::string& query) const;
  // LocalLibrary.delete(): the file and the .lrc next to it leave the card, the song leaves the library.
  bool deleteFile(const std::string& path);
  // The track at a path (null: not in the library).
  const Track* find(const std::string& path) const;

 private:
  struct Worker;
  static void list(const std::string& dir, int depth, std::vector<FileEntry>& out);
  // roots: the folders the listing covers; a file under another stays as it is
  void merge(std::vector<FileEntry>& found, const std::vector<std::string>& roots);
  void index() const;  // the groups and their folded names, rebuilt after a change
  sqlite3* db_ = nullptr;
  mutable bool indexed_ = false;
  mutable std::vector<Group> artists_, albums_, albumArtists_, genres_;
  void saveStats(const Track& t);
  mutable std::vector<const Track*> songs_, videos_;
  mutable std::vector<std::string> artistKeys_, albumKeys_;
  mutable std::unordered_map<std::string, const Track*> byPath_;
  std::deque<Track> tracks_;
  std::vector<std::string> removed_;
  std::unique_ptr<Worker> worker_;
  bool scanning_ = false;
  int64_t scanEnded_ = 0;
  uint64_t generation_ = 1;
};

}  // namespace rondo
