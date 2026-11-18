// Playlists of songs on the card (Playlists.kt in 1.0.x): each is an .m3u8 file in the card's Playlists
// folder, so it survives reflashing and computers' players open it too. Songs are written relative to
// that folder ("../Music/Artist - Title.flac"); absolute paths and Windows separators are read as well.
// The file name is the playlist's name.
// YouTube Music playlists Rondo downloaded are in Playlists/YouTube Music (downloads.h): listed with
// the others, their name "YouTube Music/<file name>"; they follow YouTube's, so the screens don't edit them.
#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rondo {

class Library;

struct Playlist {
  std::string name;  // what the calls below take: the file's name ("YouTube Music/<file's>" for a downloaded one)
  std::vector<std::string> paths;
  bool youtube = false;  // a downloaded YouTube Music playlist
  std::string title() const { return youtube ? name.substr(name.find('/') + 1) : name; }  // what a row shows
};

class Playlists {
 public:
  static constexpr const char* kYouTube = "YouTube Music";  // the downloaded ones' folder in Playlists
  explicit Playlists(const Library& library, std::string card = "/storage/sdcard0");
  std::vector<Playlist> all() const;  // by title, ignoring case
  bool get(const std::string& name, Playlist& out) const;
  // FAT can't hold / \ : * ? " < > | in a name, and a name is a file name here
  static std::string clean(const std::string& name);
  // A downloaded YouTube Music playlist: a file name for its title no other downloaded one has (" (2)"
  // after it when one does: its file, or one `taken` says is, whose file isn't there yet), 60 characters
  // at most as clean() makes it; and its file written (made when new), paths in order
  std::string freeYouTubeName(const std::string& title, const std::function<bool(const std::string&)>& taken = nullptr) const;
  bool writeYouTube(const std::string& fileName, const std::vector<std::string>& paths);
  static std::string youTubeKey(const std::string& fileName) { return std::string(kYouTube) + "/" + fileName; }
  // These return an empty string, or the problem to show (1.0.x's wording).
  std::string create(const std::string& name, const std::vector<std::string>& paths = {});
  std::string update(const std::string& name, const std::string* newName, const std::vector<std::string>* paths);
  std::string add(const std::string& name, const std::vector<std::string>& paths);
  std::string remove(const std::string& name);
  void forget(const std::string& path);  // a song left the card: drop it everywhere

 private:
  std::string fileOf(const std::string& name) const;  // the existing file for a name, or ""
  std::vector<std::string> read(const std::string& file) const;
  std::string resolve(const std::string& entry, const std::string& folder) const;  // relative to the file's folder
  bool write(const std::string& file, const std::vector<std::string>& paths) const;
  void list(const std::string& folder, bool youtube, std::vector<Playlist>& out) const;

  const Library& library_;
  std::string card_, dir_;
  // what each file held when it was last read or written, by its size and time: the playlists are
  // listed (Library's count, search) without reading every file again
  struct Read {
    int64_t size = -1, modified = -1;
    std::vector<std::string> paths;
  };
  mutable std::unordered_map<std::string, Read> read_;
};

}  // namespace rondo
