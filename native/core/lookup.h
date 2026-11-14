// Looking up what the card's files don't say, for the dashboard (shell/fill_in.h): a series' years
// (TVmaze, then Wikidata), a movie's year (Wikidata), an album's genre and year (the iTunes Store, then
// MusicBrainz) and a song's lyrics (LRCLIB). Free services, no account or key. No network code here:
// the caller's fetch makes each GET (on the Y1 with net::trust's roots, in a test with the Mac's), and
// the services' own limits are kept here (MusicBrainz: one request a second).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace rondo::lookup {

// A GET of url: the HTTP status (0: no answer at all) and the body
using Fetch = std::function<int(const std::string& url, std::string& body)>;

enum class Result { Found, NotFound, Failed };  // Failed: a service didn't answer (try again later)

struct Title {
  std::string name;    // "Silo", "The Matrix"
  std::string years;   // a series': "2023-" (running), "2010-2022", "2019" (one year); a movie's: "1999"
  std::string detail;  // what tells two apart: "Apple TV · English · running", "1999 film by the Wachowskis"
};
// The series by that name, the best match first (TVmaze's; Wikidata's when TVmaze knows none)
Result findSeries(const Fetch& get, const std::string& name, std::vector<Title>& out);
// The films by that name, the best match first (Wikidata's)
Result findMovie(const Fetch& get, const std::string& name, std::vector<Title>& out);

struct Album {
  std::string album, artist, genre;  // artist: the album's own ("Various Artists" for a compilation)
  int year = 0;
  std::string source;  // "iTunes" or "MusicBrainz"
  struct Track {
    std::string title;
    int track = 0, disc = 0;
  };
  std::vector<Track> tracks;  // with withTracks only (the iTunes Store's albums)
};
// The album `album` by `artist`; album empty: the one the song `title` is on. countries: the iTunes
// Stores to ask, in order ("us", "tr"); MusicBrainz after them. songFirst: the song `title` asked about
// before the album (an album with one song on the card: a single's, mostly, found in one request).
Result findAlbum(const Fetch& get, const std::string& artist, const std::string& album, const std::string& title,
                 const std::vector<std::string>& countries, bool withTracks, Album& out, bool songFirst = false);

struct Lyrics {
  std::string text;  // LRC when synced, else plain lines
  bool synced = false;
  bool instrumental = false;  // LRCLIB knows the song has no words (text empty)
};
// The song's lyrics from LRCLIB: its exact record (artist, title, album, length) first, then a search
Result findLyrics(const Fetch& get, const std::string& artist, const std::string& title, const std::string& album,
                  int durationSec, Lyrics& out);

// for tests: a name as matching compares it (folded; brackets, " - Single" and punctuation left out)
std::string matchName(const std::string& name);
bool sameArtist(const std::string& a, const std::string& b);
std::string urlEncode(const std::string& s);
std::string titleCase(const std::string& genre);  // MusicBrainz's "progressive rock" -> "Progressive Rock"

}  // namespace rondo::lookup
