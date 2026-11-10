#include "organize.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "fold.h"

namespace rondo {

namespace {

std::string dirOf(const std::string& p) { return p.substr(0, p.rfind('/')); }
std::string baseOf(const std::string& p) { return p.substr(p.rfind('/') + 1); }
std::string stemOf(const std::string& p) {
  std::string b = baseOf(p);
  size_t dot = b.rfind('.');
  return dot == std::string::npos || dot == 0 ? b : b.substr(0, dot);
}
std::string extOf(const std::string& p) {
  std::string b = baseOf(p);
  size_t dot = b.rfind('.');
  if (dot == std::string::npos || dot == 0) return "";
  std::string x = b.substr(dot);
  for (char& c : x) c = char(tolower((unsigned char)c));
  return x;
}
bool exists(const std::string& p) {
  struct stat st;
  return lstat(p.c_str(), &st) == 0;
}
std::string two(int n) {
  char b[16];
  snprintf(b, sizeof(b), "%02d", n);
  return b;
}
// "Stranger.Things" or "Stranger_Things" -> "Stranger Things"
std::string spaced(std::string s) {
  for (char& c : s)
    if (c == '.' || c == '_') c = ' ';
  std::string o;
  for (char c : s)
    if (!(c == ' ' && (o.empty() || o.back() == ' '))) o += c;
  while (!o.empty() && (o.back() == ' ' || o.back() == '-')) o.pop_back();
  return o;
}
// the first 19xx or 20xx in a name (a release's year), 0 for none
int yearIn(const std::string& s, size_t* at = nullptr) {
  for (size_t i = 0; i + 4 <= s.size(); i++) {
    if (!isdigit((unsigned char)s[i]) || (i && isdigit((unsigned char)s[i - 1]))) continue;
    if (i + 4 < s.size() && isdigit((unsigned char)s[i + 4])) continue;
    int y = atoi(s.substr(i, 4).c_str());
    if (y >= 1900 && y <= 2099) {
      if (at) *at = i;
      return y;
    }
  }
  return 0;
}
// S01E02 (also s1e2, 1x02) in a file name: its position, season and episode
bool episodeIn(const std::string& s, size_t& at, int& season, int& episode) {
  for (size_t i = 0; i + 3 < s.size(); i++) {
    if ((s[i] == 'S' || s[i] == 's') && isdigit((unsigned char)s[i + 1]) && (i == 0 || !isalnum((unsigned char)s[i - 1]))) {
      size_t j = i + 1;
      int se = 0, ep = 0;
      while (j < s.size() && isdigit((unsigned char)s[j]) && j - i <= 3) se = se * 10 + (s[j++] - '0');
      if (j < s.size() && (s[j] == 'E' || s[j] == 'e') && j + 1 < s.size() && isdigit((unsigned char)s[j + 1])) {
        j++;
        while (j < s.size() && isdigit((unsigned char)s[j])) ep = ep * 10 + (s[j++] - '0');
        at = i, season = se, episode = ep;
        return true;
      }
    }
    if (isdigit((unsigned char)s[i]) && (i == 0 || !isalnum((unsigned char)s[i - 1])) && i + 3 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X') &&
        isdigit((unsigned char)s[i + 2]) && isdigit((unsigned char)s[i + 3])) {
      at = i, season = s[i] - '0', episode = (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
      return true;
    }
  }
  return false;
}
// a release's words cut where its quality and source begin ("Movie.Name.1080p.BluRay.x264")
std::string withoutRelease(const std::string& s) {
  static const char* const junk[] = {"2160p", "1080p", "720p", "480p", "bluray", "brrip", "bdrip", "webrip", "web-dl", "webdl",
                                     "hdtv", "dvdrip", "x264", "x265", "h264", "h265", "hevc", "aac", "remux", "proper", "repack"};
  std::string low = s;
  for (char& c : low) c = char(tolower((unsigned char)c));
  size_t cut = s.size();
  for (const char* j : junk) {
    size_t at = low.find(j);
    if (at != std::string::npos && at > 0) cut = std::min(cut, at);
  }
  return s.substr(0, cut);
}

const char* const kCovers[] = {"cover.jpg", "folder.jpg", "front.jpg", "albumart.jpg", "cover.png", "folder.png", "front.png"};

}  // namespace

std::string cardName(const std::string& in) {
  std::string s;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = in[i];
    if (c < 0x20 || c == 0x7f) s += ' ';
    else if (c == ':') s += " -";
    else if (c == '/' || c == '\\' || c == '|') s += '-';
    else if (c == '"') s += '\'';
    else if (c == '*' || c == '?' || c == '<' || c == '>') continue;
    else s += char(c);
  }
  std::string t;  // spaces collapsed
  for (char c : s)
    if (!(c == ' ' && (t.empty() || t.back() == ' '))) t += c;
  while (!t.empty() && (t.back() == ' ' || t.back() == '.')) t.pop_back();
  size_t a = 0;
  while (a < t.size() && (t[a] == ' ' || t[a] == '.')) a++;
  t = t.substr(a);
  if (t.size() > 80) {  // cut at a character's start (UTF-8)
    size_t cut = 80;
    while (cut > 0 && (uint8_t(t[cut]) & 0xc0) == 0x80) cut--;
    t = t.substr(0, cut);
    while (!t.empty() && (t.back() == ' ' || t.back() == '.')) t.pop_back();
  }
  return t;
}

OrganizePlan planOrganize(const std::vector<OrganizeItem>& items, const std::string& card) {
  OrganizePlan plan;
  std::vector<const OrganizeItem*> songs, videos;
  for (const OrganizeItem& it : items) {
    if (it.path.compare(0, card.size() + 1, card + "/") != 0) continue;
    (it.video ? videos : songs).push_back(&it);
  }
  std::vector<std::pair<std::string, std::string>> files;  // from, to (kind by the lists above)
  std::vector<OrganizeMove::Kind> kinds;

  // music: albums by album artist and album, their year the one most of the songs carry
  struct Album {
    std::vector<const OrganizeItem*> songs;
    std::set<int> discs;
    std::map<int, int> years;
  };
  std::map<std::string, Album> albums;
  auto artistOf = [](const OrganizeItem* s) {
    std::string a = !s->albumArtist.empty() ? s->albumArtist : !s->artist.empty() ? s->artist : "Unknown Artist";
    std::string n = cardName(a);
    return n.empty() ? std::string("Unknown Artist") : n;
  };
  auto albumOf = [](const OrganizeItem* s) {
    std::string n = cardName(s->album.empty() ? "Unknown Album" : s->album);
    return n.empty() ? std::string("Unknown Album") : n;
  };
  for (const OrganizeItem* s : songs) {
    Album& a = albums[fold(artistOf(s)) + "\x1f" + fold(albumOf(s))];
    a.songs.push_back(s);
    a.discs.insert(s->disc > 0 ? s->disc : 1);
    if (s->year) a.years[s->year]++;
  }
  std::map<std::string, std::string> coverFor;  // a source folder's cover -> the album folder it goes to
  std::map<std::string, std::set<std::string>> albumsIn;  // a source folder -> the album folders its songs go to
  for (auto& kv : albums) {
    Album& a = kv.second;
    int year = 0, most = 0;
    for (auto& y : a.years)
      if (y.second > most) most = y.second, year = y.first;
    const OrganizeItem* first = a.songs.front();
    std::string folder = card + "/Music/" + artistOf(first) + "/" + albumOf(first) + (year ? " (" + std::to_string(year) + ")" : "");
    for (const OrganizeItem* s : a.songs) {
      std::string dir = folder;
      if (a.discs.size() > 1) dir += "/Disc " + std::to_string(s->disc > 0 ? s->disc : 1);
      std::string title = cardName(s->title.empty() ? stemOf(s->path) : s->title);
      if (title.empty()) title = cardName(stemOf(s->path));
      std::string name = (s->track > 0 ? two(s->track) + " - " : std::string()) + title;
      files.push_back({s->path, dir + "/" + name + extOf(s->path)});
      kinds.push_back(OrganizeMove::Song);
      albumsIn[dirOf(s->path)].insert(folder);
    }
  }

  // videos: episodes into Shows, the rest into Movies
  for (const OrganizeItem* v : videos) {
    std::string stem = stemOf(v->path);
    size_t at;
    int season, episode;
    if (episodeIn(stem, at, season, episode)) {
      std::string series = cardName(spaced(stem.substr(0, at)));
      std::string parent = baseOf(dirOf(v->path));
      int year = 0;  // the series' year from its folder ("Stranger Things (2016-2025)")
      if (series.empty()) {  // "S01E02.mp4" in a series' folder: the folder names it
        size_t y;
        series = cardName(spaced(parent));
        if (yearIn(series, &y) && y > 0) series = cardName(series.substr(0, y)), year = yearIn(parent);
        while (!series.empty() && (series.back() == '(' || series.back() == ' ')) series.pop_back();
      } else if (fold(parent).find(fold(series)) != std::string::npos) {
        year = yearIn(parent);
      }
      if (series.empty()) series = "Unknown Series";
      std::string dir = card + "/Shows/" + series + (year ? " (" + std::to_string(year) + ")" : "") + "/Season " + two(season);
      files.push_back({v->path, dir + "/" + series + " S" + two(season) + "E" + two(episode) + extOf(v->path)});
    } else {
      size_t y = std::string::npos;
      int year = yearIn(stem, &y);
      std::string name = year && y > 0 ? stem.substr(0, y) : withoutRelease(stem);
      name = cardName(spaced(name));
      while (!name.empty() && (name.back() == '(' || name.back() == '[' || name.back() == ' ')) name.pop_back();
      if (name.empty()) name = cardName(v->title.empty() ? stem : v->title);
      if (!year) year = v->year;
      std::string full = name + (year ? " (" + std::to_string(year) + ")" : "");
      files.push_back({v->path, card + "/Movies/" + full + "/" + full + extOf(v->path)});
    }
    kinds.push_back(OrganizeMove::Video);
  }

  // where everything goes, two to one place never: " (2)" for the second
  std::set<std::string> taken;  // folded targets
  std::set<std::string> leaving;
  for (auto& f : files) leaving.insert(fold(f.first));
  for (size_t i = 0; i < files.size(); i++) {
    std::string& to = files[i].second;
    if (fold(to) == fold(files[i].first)) {  // already there
      taken.insert(fold(to));
      continue;
    }
    std::string stem = to.substr(0, to.size() - extOf(to).size()), ext = extOf(to);
    for (int n = 2; taken.count(fold(to)) || (exists(to) && !leaving.count(fold(to))); n++) to = stem + " (" + std::to_string(n) + ")" + ext;
    taken.insert(fold(to));
  }

  // each source folder's lyrics and subtitles, read once (the card's Music folder can hold
  // hundreds of songs: read once for each, the plan took seconds on the Y1)
  std::map<std::string, std::vector<std::string>> sidecarsIn;
  for (auto& fl : files) {
    std::string dir = dirOf(fl.first);
    if (sidecarsIn.count(dir)) continue;
    std::vector<std::string>& names = sidecarsIn[dir];
    if (DIR* d = opendir(dir.c_str())) {
      while (dirent* e = readdir(d)) {
        std::string x = extOf(e->d_name);
        if (x == ".lrc" || x == ".srt" || x == ".ass" || x == ".vtt") names.push_back(e->d_name);
      }
      closedir(d);
    }
  }
  for (size_t i = 0; i < files.size(); i++) {
    const std::string &from = files[i].first, &to = files[i].second;
    if (from == to) {
      plan.alreadyThere++;
      continue;
    }
    plan.moves.push_back({kinds[i], from, to});
    (kinds[i] == OrganizeMove::Song ? plan.songs : plan.videos)++;
    // its .lrc (a song) or .srt (a video), named as the file is
    std::string dir = dirOf(from), stem = stemOf(from), toStem = to.substr(0, to.size() - extOf(to).size());
    for (const std::string& n : sidecarsIn[dir]) {
      {
        std::string x = extOf(n);
        bool lyrics = kinds[i] == OrganizeMove::Song && x == ".lrc";
        bool subs = kinds[i] == OrganizeMove::Video && (x == ".srt" || x == ".ass" || x == ".vtt");
        if (!lyrics && !subs) continue;
        std::string s = stemOf(n);
        // "Movie.srt", or "Movie.en.srt" (a language)
        if (strcasecmp(s.c_str(), stem.c_str()) == 0) plan.moves.push_back({lyrics ? OrganizeMove::Lyrics : OrganizeMove::Subtitles, dir + "/" + n, toStem + x});
        else if (subs && s.size() > stem.size() && !strncasecmp(s.c_str(), stem.c_str(), stem.size()) && s[stem.size()] == '.')
          plan.moves.push_back({OrganizeMove::Subtitles, dir + "/" + n, toStem + s.substr(stem.size()) + x});
        else continue;
        plan.sidecars++;
      }
    }
  }
  // a folder of one album's songs: its cover goes with them
  for (auto& kv : albumsIn) {
    if (kv.second.size() != 1) continue;
    const std::string& target = *kv.second.begin();
    if (fold(kv.first) == fold(target)) continue;
    if (DIR* d = opendir(kv.first.c_str())) {
      while (dirent* e = readdir(d)) {
        for (const char* c : kCovers)
          if (!strcasecmp(e->d_name, c) && !exists(target + "/" + e->d_name)) {
            plan.moves.push_back({OrganizeMove::Cover, kv.first + "/" + e->d_name, target + "/" + e->d_name});
            plan.covers++;
          }
      }
      closedir(d);
    }
  }
  return plan;
}

namespace {
bool makeDirs(const std::string& dir) {
  if (dir.empty() || exists(dir)) return true;
  if (!makeDirs(dirOf(dir))) return false;
  return mkdir(dir.c_str(), 0775) == 0 || errno == EEXIST;
}
}  // namespace

OrganizeResult applyOrganize(const OrganizePlan& plan, const std::string& card, const std::function<void(int, int)>& progress) {
  OrganizeResult r;
  std::set<std::string> left;  // the folders files left
  int total = int(plan.moves.size()), n = 0;
  for (const OrganizeMove& m : plan.moves) {
    n++;
    if (!exists(m.from)) {
      r.errors.push_back(baseOf(m.from) + ": not on the card any more");
    } else if (!makeDirs(dirOf(m.to))) {
      r.errors.push_back(baseOf(m.from) + ": couldn't make " + dirOf(m.to) + " (" + strerror(errno) + ")");
    } else if (exists(m.to) && strcasecmp(m.from.c_str(), m.to.c_str()) != 0) {
      r.errors.push_back(baseOf(m.from) + ": " + baseOf(m.to) + " is there already");
    } else if (rename(m.from.c_str(), m.to.c_str()) != 0) {
      r.errors.push_back(baseOf(m.from) + ": " + strerror(errno));
    } else {
      r.done.push_back(m);
      left.insert(dirOf(m.from));
    }
    if (progress) progress(n, total);
  }
  // the folders the moves emptied, up to the card's top folders (Music, Videos…), which stay
  for (std::string d : left)
    while (d.size() > card.size() + 1 && dirOf(d) != card && rmdir(d.c_str()) == 0) d = dirOf(d);
  sync();
  return r;
}

}  // namespace rondo
