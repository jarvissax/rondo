// lookup.h.
#include "lookup.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include <yyjson.h>

#include "fold.h"

namespace rondo::lookup {

namespace {

using J = yyjson_val*;

struct Doc {
  yyjson_doc* d = nullptr;
  explicit Doc(const std::string& s) : d(yyjson_read(s.data(), s.size(), YYJSON_READ_ALLOW_INVALID_UNICODE)) {}
  ~Doc() { yyjson_doc_free(d); }
  J root() const { return d ? yyjson_doc_get_root(d) : nullptr; }
};
J key(J o, const char* k) { return o && yyjson_is_obj(o) ? yyjson_obj_get(o, k) : nullptr; }
std::string str(J v) { return yyjson_is_str(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(); }
int64_t num(J v) { return yyjson_is_int(v) ? yyjson_get_sint(v) : yyjson_is_real(v) ? int64_t(yyjson_get_real(v)) : 0; }
template <typename F>
void each(J a, F f) {
  if (!yyjson_is_arr(a)) return;
  size_t i, n;
  J v;
  yyjson_arr_foreach(a, i, n, v) f(v);
}
int yearOf(const std::string& date) {  // "1976-03-26T08:00:00Z", "+1999-03-31T00:00:00Z", "2023"
  size_t i = date.find_first_of("0123456789");
  if (i == std::string::npos || i + 4 > date.size()) return 0;
  int y = atoi(date.substr(i, 4).c_str());
  return y >= 1800 && y <= 2100 ? y : 0;
}
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::mutex g_paceMu;
std::map<std::string, int64_t> g_last;  // a service's last request (monotonic ms)
std::map<std::string, int> g_slower;    // what a service's refusals added to its pace (ms)

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
void sleepMs(int64_t ms) {
  timespec t = {time_t(ms / 1000), long(ms % 1000) * 1000000};
  nanosleep(&t, nullptr);
}

// At most one request every `ms` to a service (MusicBrainz asks for one a second; the iTunes Store
// allows about 20 a minute, in bursts), slower once it has refused one
void pace(const char* service, int ms) {
  std::unique_lock<std::mutex> l(g_paceMu);
  int64_t& at = g_last[service];
  int64_t wait = at + ms + g_slower[service] - nowMs();
  if (wait > 0) {
    l.unlock();
    sleepMs(wait);
    l.lock();
  }
  at = nowMs();
}

// A GET with a JSON answer. A refusal for going too fast (403, 429, 503): the service is asked more
// slowly from then on (up to one request in 4 s), and the request again after a wait (three times):
// the iTunes Store counts a minute's requests, the others a second's.
int getJson(const Fetch& get, const char* service, int paceMs, const std::string& url, std::string& body) {
  for (int attempt = 0;; attempt++) {
    pace(service, paceMs);
    body.clear();
    int st = get(url, body);
    if ((st == 403 || st == 429 || st == 503) && attempt < 3) {
      {
        std::lock_guard<std::mutex> l(g_paceMu);
        int& slower = g_slower[service];
        slower = std::min(4000 - paceMs, std::max(slower * 2, 1000));
        fprintf(stderr, "rondo: lookup: %s refused (%d): one request every %d ms from now\n", service, st, paceMs + slower);
      }
      sleepMs((strcmp(service, "itunes") == 0 ? 15000 : 3000) * (attempt + 1));
      continue;
    }
    return st;
  }
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
  std::string s;
  for (const std::string& p : parts)
    if (!p.empty()) s += (s.empty() ? "" : sep) + p;
  return s;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
    else o += '%', o += hex[c >> 4], o += hex[c & 15];
  }
  return o;
}

std::string matchName(const std::string& name) {
  std::string s = fold(name);
  // " - Single", " - EP" (the iTunes Store's)
  for (const char* tail : {" - single", " - ep"}) {
    size_t n = strlen(tail);
    if (s.size() > n && s.compare(s.size() - n, n, tail) == 0) s.resize(s.size() - n);
  }
  // what brackets say ("(Remastered)", "[Deluxe Edition]"): left out, as long as something stays
  std::string out;
  int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[') depth++;
    else if ((c == ')' || c == ']') && depth > 0) depth--;
    else if (!depth) out += c;
  }
  if (trim(out).empty()) out = s;
  std::string t;  // letters and digits (any script), one space between words
  for (size_t i = 0; i < out.size(); i++) {
    unsigned char c = out[i];
    bool word = c >= 0x80 || isalnum(c);
    if (c == '&') {
      if (!t.empty() && t.back() != ' ') t += ' ';
      t += "and";
      continue;
    }
    if (word) t += char(c);
    else if (c != '\'' && !t.empty() && t.back() != ' ') t += ' ';
  }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  return t;
}

namespace {
// "A & B", "A, B", "A feat. B", "A x B": the names one by one
std::vector<std::string> artistsOf(const std::string& s) {
  std::string f = fold(s);
  for (const char* sep : {" featuring ", " feat. ", " feat ", " ft. ", " ft ", " with ", " x ", " vs. ", " vs ", " & ", " and ", ", ", "; ", " / "}) {
    size_t at;
    while ((at = f.find(sep)) != std::string::npos) f.replace(at, strlen(sep), "\x1f");
  }
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= f.size()) {
    size_t b = f.find('\x1f', a);
    if (b == std::string::npos) b = f.size();
    std::string one = matchName(f.substr(a, b - a));
    if (!one.empty()) out.push_back(one);
    a = b + 1;
  }
  return out;
}
}  // namespace

bool sameArtist(const std::string& a, const std::string& b) {
  std::string x = matchName(a), y = matchName(b);
  if (x.empty() || y.empty()) return false;
  if (x == y) return true;
  auto nospace = [](std::string s) {
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s;
  };
  if (nospace(x) == nospace(y)) return true;  // "AC/DC" and "AC-DC", "Blok 3" and "BLOK3"
  // one inside the other, whole words: "Jimi Hendrix" and "The Jimi Hendrix Experience" (not
  // "Camel" and "Camelphat"; what's matched is an album or song of that name as well)
  auto within = [](const std::string& small, const std::string& big) {
    if (small.size() < 4 || small.size() >= big.size()) return false;
    std::string b = " " + big + " ";
    return b.find(" " + small + " ") != std::string::npos;
  };
  if (within(x, y) || within(y, x)) return true;
  std::vector<std::string> as = artistsOf(a), bs = artistsOf(b);
  for (const std::string& p : as)
    for (const std::string& q : bs)
      if (p == q || nospace(p) == nospace(q)) return true;
  return false;
}

namespace {
// what's searched for: a name without what its brackets say ("Scary Monsters and Nice Sprites
// (Deluxe Tour Edition)" finds nothing in the iTunes Store; without it, the album)
std::string searchName(const std::string& s) {
  std::string out;
  int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[') depth++;
    else if ((c == ')' || c == ']') && depth > 0) depth--;
    else if (!depth) out += c;
  }
  out = trim(out);
  return out.empty() ? s : out;
}
}  // namespace

std::string titleCase(const std::string& genre) {
  static const std::set<std::string> upper = {"r&b", "edm", "idm", "uk", "us", "dj", "ebm", "j-pop", "k-pop", "j-rock", "k-rap"};
  std::string out;
  size_t a = 0;
  while (a < genre.size()) {
    size_t b = genre.find(' ', a);
    if (b == std::string::npos) b = genre.size();
    std::string w = genre.substr(a, b - a);
    std::string lw = w;
    for (char& c : lw) c = char(tolower((unsigned char)c));
    if (upper.count(lw)) {
      for (char& c : w) c = char(toupper((unsigned char)c));
    } else {
      bool start = true;  // "hip-hop" -> "Hip-Hop"
      for (char& c : w) {
        if (start && islower((unsigned char)c)) c = char(toupper((unsigned char)c));
        start = c == '-' || c == '/';
      }
    }
    out += (out.empty() ? "" : " ") + w;
    a = b + 1;
  }
  return out;
}

// ---------- series and movies ----------

namespace {

// Wikidata's items for a name whose description says what they are: films, or series
Result wikidata(const Fetch& get, const std::string& name, bool series, std::vector<Title>& out) {
  std::string body;
  int st = getJson(get, "wikidata", 500, "https://www.wikidata.org/w/api.php?action=wbsearchentities&format=json&language=en&uselang=en&type=item&limit=12&search=" + urlEncode(name), body);
  if (st != 200) return Result::Failed;
  Doc d(body);
  struct Cand {
    std::string id, name, detail;
  };
  std::vector<Cand> cands;
  each(key(d.root(), "search"), [&](J e) {
    std::string desc = str(key(e, "description")), low = fold(desc);
    bool film = (contains(low, "film") || contains(low, "movie")) && !contains(low, "film series") && !contains(low, "franchise") &&
                !contains(low, "poster") && !contains(low, "soundtrack") && !contains(low, "character") && !contains(low, "festival") &&
                !contains(low, "television series") && !contains(low, "tv series");
    bool show = (contains(low, "television series") || contains(low, "tv series") || contains(low, "web series") || contains(low, "miniseries") ||
                 contains(low, "anime series") || contains(low, "animated series") || contains(low, "sitcom") || contains(low, "telenovela") ||
                 contains(low, "soap opera")) &&
                !contains(low, "episode") && !contains(low, "season of") && !contains(low, "character");
    if (series ? !show : !film) return;
    if (cands.size() >= 4) return;
    // the name as asked when it matched in another language ("Babam ve Oğlum", not "My Father and My Son")
    J m = key(e, "match");
    std::string label = str(key(e, "label")), lang = str(key(m, "language")), text = str(key(m, "text"));
    std::string shown = !text.empty() && lang != "en" && fold(text) == fold(name) ? name : label;
    if (shown.empty()) return;
    cands.push_back({str(key(e, "id")), shown, desc});
  });
  if (cands.empty()) return Result::NotFound;
  std::string values;
  for (const Cand& c : cands) values += " wd:" + c.id;
  std::string q = "SELECT ?item (MIN(?p) AS ?pub) (MIN(?s) AS ?start) (MAX(?e) AS ?end) WHERE { VALUES ?item {" + values +
                  " } OPTIONAL { ?item wdt:P577 ?p } OPTIONAL { ?item wdt:P580 ?s } OPTIONAL { ?item wdt:P582 ?e } } GROUP BY ?item";
  std::map<std::string, int> pub, start, end;
  if (getJson(get, "wikidata", 500, "https://query.wikidata.org/sparql?format=json&query=" + urlEncode(q), body) == 200) {
    Doc r(body);
    each(key(key(r.root(), "results"), "bindings"), [&](J b) {
      std::string item = str(key(key(b, "item"), "value"));
      item = item.substr(item.rfind('/') + 1);
      pub[item] = yearOf(str(key(key(b, "pub"), "value")));
      start[item] = yearOf(str(key(key(b, "start"), "value")));
      end[item] = yearOf(str(key(key(b, "end"), "value")));
    });
  }
  for (const Cand& c : cands) {
    Title t;
    t.name = c.name, t.detail = c.detail;
    int from = start[c.id] ? start[c.id] : pub[c.id];
    if (!from) from = yearOf(c.detail);  // "2014 film by Christopher Nolan"
    if (!from) continue;
    if (series) t.years = std::to_string(from) + (end[c.id] == from ? "" : "-" + (end[c.id] ? std::to_string(end[c.id]) : ""));
    else t.years = std::to_string(pub[c.id] ? pub[c.id] : from);
    out.push_back(t);
  }
  return out.empty() ? Result::NotFound : Result::Found;
}

}  // namespace

Result findSeries(const Fetch& get, const std::string& name, std::vector<Title>& out) {
  out.clear();
  std::string body;
  int st = getJson(get, "tvmaze", 600, "https://api.tvmaze.com/search/shows?q=" + urlEncode(name), body);
  if (st != 200 && st != 404) return Result::Failed;
  Doc d(body);
  each(d.root(), [&](J e) {
    J show = key(e, "show");
    std::string title = str(key(show, "name")), premiered = str(key(show, "premiered")), ended = str(key(show, "ended"));
    std::string status = str(key(show, "status"));
    int from = yearOf(premiered), to = yearOf(ended);
    if (title.empty() || !from || out.size() >= 5) return;
    Title t;
    t.name = title;
    bool over = status == "Ended" && to;
    t.years = std::to_string(from) + (over ? (to == from ? "" : "-" + std::to_string(to)) : "-");
    std::string channel = str(key(key(show, "network"), "name"));
    if (channel.empty()) channel = str(key(key(show, "webChannel"), "name"));
    t.detail = join({channel, str(key(show, "language")), over ? "ended" : "running"}, " · ");
    out.push_back(t);
  });
  if (!out.empty()) return Result::Found;
  return wikidata(get, name, true, out);
}

Result findMovie(const Fetch& get, const std::string& name, std::vector<Title>& out) {
  out.clear();
  return wikidata(get, name, false, out);
}

// ---------- albums ----------

namespace {

// The iTunes Store: the album by its name, and the song's album by the song; the song first when it's
// the album's only one on the card (a single's, mostly: one request instead of two)
Result itunes(const Fetch& get, const std::string& artist, const std::string& album, const std::string& title, const std::string& country,
              bool withTracks, bool songFirst, Album& out) {
  std::string body, base = "https://itunes.apple.com/";
  std::string tail = "&country=" + urlEncode(country) + "&lang=en_us";
  int64_t collection = 0;
  bool failed = false;
  auto byAlbum = [&] {
    if (album.empty() || collection) return;
    int st = getJson(get, "itunes", 1000, base + "search?media=music&entity=album&limit=15&term=" + urlEncode(artist + " " + searchName(album)) + tail, body);
    if (st != 200) failed = true;
    Doc d(body);
    std::string want = matchName(album);
    each(key(d.root(), "results"), [&](J e) {
      if (collection || matchName(str(key(e, "collectionName"))) != want || !sameArtist(artist, str(key(e, "artistName")))) return;
      collection = num(key(e, "collectionId"));
      out.album = str(key(e, "collectionName")), out.artist = str(key(e, "artistName")), out.genre = str(key(e, "primaryGenreName"));
      out.year = yearOf(str(key(e, "releaseDate")));
    });
  };
  auto bySong = [&] {  // the song: its album's details
    if (title.empty() || collection) return;
    int st = getJson(get, "itunes", 1000, base + "search?media=music&entity=song&limit=25&term=" + urlEncode(artist + " " + searchName(title)) + tail, body);
    if (st != 200) failed = true;
    Doc d(body);
    std::string want = matchName(title), wantAlbum = matchName(album);
    // the song on the album named, else its first album
    for (int pass = 0; pass < 2 && !collection; pass++)
      each(key(d.root(), "results"), [&](J e) {
        if (collection || matchName(str(key(e, "trackName"))) != want || !sameArtist(artist, str(key(e, "artistName")))) return;
        std::string on = str(key(e, "collectionName"));
        if (pass == 0 && !album.empty() && matchName(on) != wantAlbum) return;
        if (pass == 1 && !album.empty()) return;  // an album named but not this one: not a match
        collection = num(key(e, "collectionId"));
        std::string by = str(key(e, "collectionArtistName"));
        out.album = on, out.artist = by.empty() ? str(key(e, "artistName")) : by, out.genre = str(key(e, "primaryGenreName"));
        out.year = yearOf(str(key(e, "releaseDate")));
        out.tracks.push_back({str(key(e, "trackName")), int(num(key(e, "trackNumber"))), int(num(key(e, "discNumber")))});
      });
  };
  if (songFirst) bySong(), byAlbum();
  else byAlbum(), bySong();
  if (!collection) return failed ? Result::Failed : Result::NotFound;
  out.source = "iTunes";
  if (withTracks) {
    if (getJson(get, "itunes", 1000, base + "lookup?entity=song&limit=200&id=" + std::to_string(collection) + tail, body) == 200) {
      Doc d(body);
      each(key(d.root(), "results"), [&](J e) {
        if (str(key(e, "wrapperType")) != "track") return;
        out.tracks.push_back({str(key(e, "trackName")), int(num(key(e, "trackNumber"))), int(num(key(e, "discNumber")))});
      });
    }
  }
  return Result::Found;
}

std::string lucene(const std::string& s) {  // a phrase in a MusicBrainz query
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}
// the genre MusicBrainz's users named most often
std::string topGenre(J genres) {
  std::string best;
  int64_t most = 0;
  each(genres, [&](J g) {
    int64_t n = num(key(g, "count"));
    if (n > most) most = n, best = str(key(g, "name"));
  });
  return best.empty() ? "" : titleCase(best);
}

Result musicbrainz(const Fetch& get, const std::string& artist, const std::string& album, Album& out) {
  if (album.empty()) return Result::NotFound;
  std::string body, base = "https://musicbrainz.org/ws/2/";
  std::string q = "releasegroup:" + lucene(album) + " AND artist:" + lucene(artist);
  if (getJson(get, "musicbrainz", 1100, base + "release-group/?fmt=json&limit=8&query=" + urlEncode(q), body) != 200) return Result::Failed;
  std::string id, artistId;
  {
    Doc d(body);
    std::string want = matchName(album);
    each(key(d.root(), "release-groups"), [&](J e) {
      if (!id.empty() || matchName(str(key(e, "title"))) != want) return;
      std::string credit;
      J first = nullptr;
      each(key(e, "artist-credit"), [&](J c) {
        credit += str(key(c, "name")) + str(key(c, "joinphrase"));
        if (!first) first = c;
      });
      if (!sameArtist(artist, credit)) return;
      id = str(key(e, "id")), artistId = str(key(key(first, "artist"), "id"));
      out.album = str(key(e, "title")), out.artist = credit, out.year = yearOf(str(key(e, "first-release-date")));
    });
  }
  if (id.empty()) return Result::NotFound;
  out.source = "MusicBrainz";
  if (getJson(get, "musicbrainz", 1100, base + "release-group/" + id + "?fmt=json&inc=genres", body) == 200) {
    Doc d(body);
    out.genre = topGenre(key(d.root(), "genres"));
  }
  if (out.genre.empty() && !artistId.empty() && getJson(get, "musicbrainz", 1100, base + "artist/" + artistId + "?fmt=json&inc=genres", body) == 200) {
    Doc d(body);
    out.genre = topGenre(key(d.root(), "genres"));  // the artist's, when the album has none
  }
  return Result::Found;
}

}  // namespace

Result findAlbum(const Fetch& get, const std::string& artist, const std::string& album, const std::string& title,
                 const std::vector<std::string>& countries, bool withTracks, Album& out, bool songFirst) {
  bool failed = false;
  std::vector<std::string> asked;
  for (const std::string& c : countries) {
    if (c.empty() || std::find(asked.begin(), asked.end(), c) != asked.end()) continue;
    asked.push_back(c);
    Album a;
    Result r = itunes(get, artist, album, title, c, withTracks, songFirst, a);
    if (r == Result::Found && !a.genre.empty()) return out = a, Result::Found;
    if (r == Result::Found && out.source.empty()) out = a;  // found without a genre: kept, MusicBrainz may know it
    if (r == Result::Failed) failed = true;
  }
  Album m;
  Result r = musicbrainz(get, artist, album, m);
  if (r == Result::Found && (out.source.empty() || (!m.genre.empty() && out.genre.empty()))) {
    if (out.source.empty()) out = m;
    else out.genre = m.genre;
    return Result::Found;
  }
  if (!out.source.empty()) return Result::Found;
  return failed || r == Result::Failed ? Result::Failed : Result::NotFound;
}

// ---------- lyrics ----------

Result findLyrics(const Fetch& get, const std::string& artist, const std::string& title, const std::string& album, int durationSec,
                  Lyrics& out) {
  out = Lyrics();
  if (artist.empty() || title.empty()) return Result::NotFound;
  auto take = [&](J e) {
    std::string synced = trim(str(key(e, "syncedLyrics"))), plain = trim(str(key(e, "plainLyrics")));
    if (!synced.empty()) out.text = synced, out.synced = true;
    else if (!plain.empty()) out.text = plain;
    else if (yyjson_is_true(key(e, "instrumental"))) out.instrumental = true;
    return !out.text.empty() || out.instrumental;
  };
  std::string body, base = "https://lrclib.net/api/";
  std::string d = durationSec > 0 && durationSec <= 7200 ? "&duration=" + std::to_string(durationSec) : "";
  int st = getJson(get, "lrclib", 250, base + "get?artist_name=" + urlEncode(artist) + "&track_name=" + urlEncode(title) +
                                        (album.empty() ? "" : "&album_name=" + urlEncode(album)) + d, body);
  if (st == 200) {
    Doc doc(body);
    if (take(doc.root())) return Result::Found;
  } else if (st != 404) {
    return Result::Failed;
  }
  // a search: the same song within 3 s of its length, synced lyrics first
  st = getJson(get, "lrclib", 250, base + "search?artist_name=" + urlEncode(artist) + "&track_name=" + urlEncode(title), body);
  if (st != 200) return Result::Failed;
  Doc doc(body);
  std::string want = matchName(title);
  J best = nullptr;
  int bestScore = -1;
  each(doc.root(), [&](J e) {
    if (matchName(str(key(e, "trackName"))) != want || !sameArtist(artist, str(key(e, "artistName")))) return;
    double len = yyjson_is_num(key(e, "duration")) ? yyjson_get_num(key(e, "duration")) : 0;
    if (durationSec > 0 && (len <= 0 || fabs(len - durationSec) > 3)) return;
    int score = !trim(str(key(e, "syncedLyrics"))).empty() ? 2 : !trim(str(key(e, "plainLyrics"))).empty() ? 1 : 0;
    if (score > bestScore) best = e, bestScore = score;
  });
  if (best && take(best)) return Result::Found;
  return Result::NotFound;
}

}  // namespace rondo::lookup
