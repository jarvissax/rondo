// Playlists of songs on the card (Playlists.kt in 1.0.x): each is an .m3u8 file in the card's Playlists
// folder, so it survives reflashing and computers' players open it too. Songs are written relative to
// that folder ("../Music/Artist - Title.flac"); absolute paths and Windows separators are read as well.
// The file name is the playlist's name.
#pragma once
#include <string>
#include <vector>

namespace rondo {

class Library;

struct Playlist {
  std::string name;
  std::vector<std::string> paths;
};

class Playlists {
 public:
  explicit Playlists(const Library& library, std::string card = "/storage/sdcard0");
  std::vector<Playlist> all() const;  // by name, ignoring case
  bool get(const std::string& name, Playlist& out) const;
  // FAT can't hold / \ : * ? " < > | in a name, and a name is a file name here
  static std::string clean(const std::string& name);
  // These return an empty string, or the problem to show (1.0.x's wording).
  std::string create(const std::string& name, const std::vector<std::string>& paths = {});
  std::string update(const std::string& name, const std::string* newName, const std::vector<std::string>* paths);
  std::string add(const std::string& name, const std::vector<std::string>& paths);
  std::string remove(const std::string& name);
  void forget(const std::string& path);  // a song left the card: drop it everywhere

 private:
  std::string fileOf(const std::string& name) const;  // the existing file for a name, or ""
  std::vector<std::string> read(const std::string& file) const;
  std::string resolve(const std::string& entry) const;
  bool write(const std::string& file, const std::vector<std::string>& paths) const;

  const Library& library_;
  std::string card_, dir_;
};

}  // namespace rondo
