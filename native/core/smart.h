// Smart playlists: songs chosen by rules instead of by hand, as iTunes' smart playlists. Each is a
// <name>.smart.json beside the .m3u8 playlists in the card's Playlists folder (it survives reflashing
// like them). The songs are worked out from the library whenever the list is opened, so a smart
// playlist follows what you add, play and rate. Made and edited on the dashboard; the Y1 lists and
// plays them with the playlists.
#pragma once
#include <string>
#include <vector>

namespace rondo {

class Library;
struct Track;

// field: title, artist, album, albumArtist, genre, folder (text: is, isNot, contains, notContains,
// startsWith); year, rating, plays, length (seconds) (number: is, isNot, atLeast, atMost); added,
// lastPlayed (date: inLast, notInLast, a number of days)
struct SmartRule {
  std::string field, op, text;
  double number = 0;
};

struct SmartPlaylist {
  std::string name;
  bool matchAll = true;  // all rules, else any
  std::vector<SmartRule> rules;
  // random, title, artist, album, added, lastPlayed, plays, rating, year
  std::string sort = "artist";
  int limit = 0;  // songs at most (0: no limit)
};

class SmartPlaylists {
 public:
  explicit SmartPlaylists(std::string card = "/storage/sdcard0");
  std::vector<SmartPlaylist> all() const;  // by name, ignoring case
  bool get(const std::string& name, SmartPlaylist& out) const;
  // "" when saved (renamed from oldName when given), else the problem to show
  std::string save(SmartPlaylist p, const std::string& oldName = "") const;
  std::string remove(const std::string& name) const;
  // The songs a playlist's rules pick from the library, in its order, up to its limit
  static std::vector<const Track*> songs(const SmartPlaylist& p, const Library& library);
  // A rule that can't be understood (an unknown field or operator), as words; "" when all are fine
  static std::string problem(const SmartPlaylist& p);
  static std::string toJson(const SmartPlaylist& p);
  static bool fromJson(const std::string& json, SmartPlaylist& out);

 private:
  std::string fileOf(const std::string& name) const;
  std::string dir_;
};

}  // namespace rondo
