#include "smart.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <random>
#include <set>

#include <yyjson.h>

#include "fold.h"
#include "library.h"
#include "playlists.h"

namespace rondo {

namespace {

const char kSuffix[] = ".smart.json";

const std::set<std::string> kText = {"title", "artist", "album", "albumArtist", "genre", "folder"};
const std::set<std::string> kNumber = {"year", "rating", "plays", "length"};
const std::set<std::string> kDate = {"added", "lastPlayed"};
const std::set<std::string> kTextOps = {"is", "isNot", "contains", "notContains", "startsWith"};
const std::set<std::string> kNumberOps = {"is", "isNot", "atLeast", "atMost"};
const std::set<std::string> kDateOps = {"inLast", "notInLast"};
const std::set<std::string> kSorts = {"random", "title", "artist", "album", "added", "lastPlayed", "plays", "rating", "year"};

int64_t nowMs() {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

std::string textOf(const Track& t, const std::string& field) {
  if (field == "title") return t.title;
  if (field == "artist") return t.artist;
  if (field == "album") return t.album;
  if (field == "albumArtist") return t.albumArtist.empty() ? t.artist : t.albumArtist;
  if (field == "genre") return t.genre;
  // folder: the folder's path on the card, from the card's top ("Music/Pink Floyd/The Wall (1979)")
  std::string dir = t.path.substr(0, t.path.rfind('/'));
  for (const char* root : {"/storage/sdcard0", "/storage/sdcard1"}) {
    size_t n = strlen(root);
    if (dir.compare(0, n, root) == 0) return dir.size() > n ? dir.substr(n + 1) : "";
  }
  return dir;
}

double numberOf(const Track& t, const std::string& field) {
  if (field == "year") return t.year;
  if (field == "rating") return t.rating;
  if (field == "plays") return t.plays;
  return t.durationSec;  // length
}

bool matches(const Track& t, const SmartRule& r, int64_t now) {
  if (kText.count(r.field)) {
    std::string have = fold(textOf(t, r.field)), want = fold(r.text);
    if (r.op == "is") return have == want;
    if (r.op == "isNot") return have != want;
    if (r.op == "contains") return have.find(want) != std::string::npos;
    if (r.op == "notContains") return have.find(want) == std::string::npos;
    return have.compare(0, want.size(), want) == 0;  // startsWith
  }
  if (kNumber.count(r.field)) {
    double v = numberOf(t, r.field);
    if (r.field == "length" && v < 0) return false;
    if (r.op == "is") return v == r.number;
    if (r.op == "isNot") return v != r.number;
    if (r.op == "atLeast") return v >= r.number;
    return v <= r.number;  // atMost
  }
  int64_t at = r.field == "added" ? t.modified : t.lastPlayed;
  bool recent = at > 0 && now - at <= int64_t(r.number * 86400000.0);
  return r.op == "inLast" ? recent : !recent;
}

}  // namespace

SmartPlaylists::SmartPlaylists(std::string card) : dir_(std::move(card) + "/Playlists") {}

std::string SmartPlaylists::fileOf(const std::string& name) const {
  std::string want = Playlists::clean(name);
  if (DIR* d = opendir(dir_.c_str())) {  // the existing file for the name, whatever its case
    while (dirent* e = readdir(d)) {
      std::string f = e->d_name;
      size_t n = sizeof(kSuffix) - 1;
      if (f.size() > n && !strcasecmp(f.c_str() + f.size() - n, kSuffix) && !strcasecmp(f.substr(0, f.size() - n).c_str(), want.c_str())) {
        closedir(d);
        return dir_ + "/" + f;
      }
    }
    closedir(d);
  }
  return "";
}

std::vector<SmartPlaylist> SmartPlaylists::all() const {
  std::vector<SmartPlaylist> out;
  if (DIR* d = opendir(dir_.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string f = e->d_name;
      size_t n = sizeof(kSuffix) - 1;
      if (f.size() <= n || strcasecmp(f.c_str() + f.size() - n, kSuffix)) continue;
      FILE* fp = fopen((dir_ + "/" + f).c_str(), "rb");
      if (!fp) continue;
      std::string json;
      char buf[4096];
      size_t got;
      while ((got = fread(buf, 1, sizeof(buf), fp)) > 0 && json.size() < 65536) json.append(buf, got);
      fclose(fp);
      SmartPlaylist p;
      if (!fromJson(json, p)) continue;
      p.name = f.substr(0, f.size() - n);  // the file's name is the playlist's
      out.push_back(std::move(p));
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end(), [](const SmartPlaylist& a, const SmartPlaylist& b) { return lower(a.name) < lower(b.name); });
  return out;
}

bool SmartPlaylists::get(const std::string& name, SmartPlaylist& out) const {
  for (SmartPlaylist& p : all())
    if (!strcasecmp(p.name.c_str(), Playlists::clean(name).c_str())) {
      out = std::move(p);
      return true;
    }
  return false;
}

std::string SmartPlaylists::problem(const SmartPlaylist& p) {
  if (p.rules.empty()) return "Add a rule";
  if (p.rules.size() > 12) return "At most 12 rules";
  for (const SmartRule& r : p.rules) {
    bool ok = (kText.count(r.field) && kTextOps.count(r.op)) || (kNumber.count(r.field) && kNumberOps.count(r.op)) ||
              (kDate.count(r.field) && kDateOps.count(r.op));
    if (!ok) return "A rule Rondo doesn't know: " + r.field + " " + r.op;
    if (kText.count(r.field) && r.text.empty() && r.op != "is" && r.op != "isNot") return "A rule without text";
    if (kDate.count(r.field) && r.number <= 0) return "A number of days above 0";
  }
  if (!kSorts.count(p.sort)) return "An order Rondo doesn't know: " + p.sort;
  if (p.limit < 0 || p.limit > 5000) return "A limit of 0 to 5000 songs";
  return "";
}

std::string SmartPlaylists::save(SmartPlaylist p, const std::string& oldName) const {
  p.name = Playlists::clean(p.name);
  if (p.name.empty()) return "Type a name";
  std::string why = problem(p);
  if (!why.empty()) return why;
  std::string existing = fileOf(p.name);
  bool renaming = !oldName.empty() && strcasecmp(Playlists::clean(oldName).c_str(), p.name.c_str()) != 0;
  if (renaming && !existing.empty()) return "A smart playlist by that name exists";
  if (oldName.empty() && !existing.empty()) return "A smart playlist by that name exists";
  mkdir(dir_.c_str(), 0775);
  std::string file = dir_ + "/" + p.name + kSuffix, tmp = file + ".part";
  FILE* fp = fopen(tmp.c_str(), "wb");
  if (!fp) return "Couldn't write to the card";
  std::string json = toJson(p);
  bool ok = fwrite(json.data(), 1, json.size(), fp) == json.size();
  ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0 && ok;
  ok = fclose(fp) == 0 && ok;
  if (!ok || rename(tmp.c_str(), file.c_str()) != 0) {
    unlink(tmp.c_str());
    return "Couldn't write to the card";
  }
  if (renaming) {
    std::string old = fileOf(oldName);
    if (!old.empty() && old != file) unlink(old.c_str());
  }
  return "";
}

std::string SmartPlaylists::remove(const std::string& name) const {
  std::string f = fileOf(name);
  if (f.empty()) return "There's no smart playlist by that name";
  return unlink(f.c_str()) == 0 ? "" : "Couldn't delete it from the card";
}

std::vector<const Track*> SmartPlaylists::songs(const SmartPlaylist& p, const Library& library) {
  int64_t now = nowMs();
  std::vector<const Track*> v;
  for (const Track* t : library.songs()) {
    bool all = true, any = false;
    for (const SmartRule& r : p.rules) {
      bool m = matches(*t, r, now);
      all &= m, any |= m;
    }
    if (p.rules.empty() ? false : p.matchAll ? all : any) v.push_back(t);
  }
  // library.songs() is by title already
  auto byText = [](const std::string& a, const std::string& b) { return lower(a) < lower(b); };
  const std::string& s = p.sort;
  if (s == "random") {
    std::mt19937 rng(uint32_t(now / 1000));
    std::shuffle(v.begin(), v.end(), rng);
  } else if (s == "artist" || s == "album") {
    std::stable_sort(v.begin(), v.end(), [&](const Track* a, const Track* b) {
      if (s == "artist" && lower(a->artist) != lower(b->artist)) return byText(a->artist, b->artist);
      if (lower(a->album) != lower(b->album)) return byText(a->album, b->album);
      if (a->disc != b->disc) return a->disc < b->disc;
      return a->track < b->track;
    });
  } else if (s != "title") {
    std::stable_sort(v.begin(), v.end(), [&](const Track* a, const Track* b) {
      if (s == "added") return a->modified > b->modified;
      if (s == "lastPlayed") return a->lastPlayed > b->lastPlayed;
      if (s == "plays") return a->plays > b->plays;
      if (s == "rating") return a->rating > b->rating;
      return a->year > b->year;  // year, the newest first
    });
  }
  if (p.limit > 0 && v.size() > size_t(p.limit)) v.resize(size_t(p.limit));
  return v;
}

std::string SmartPlaylists::toJson(const SmartPlaylist& p) {
  yyjson_mut_doc* d = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  yyjson_mut_obj_add_str(d, root, "match", p.matchAll ? "all" : "any");
  yyjson_mut_val* rules = yyjson_mut_arr(d);
  for (const SmartRule& r : p.rules) {
    yyjson_mut_val* o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "field", r.field.c_str());
    yyjson_mut_obj_add_strcpy(d, o, "op", r.op.c_str());
    if (kText.count(r.field)) yyjson_mut_obj_add_strcpy(d, o, "value", r.text.c_str());
    else yyjson_mut_obj_add_real(d, o, "value", r.number);
    yyjson_mut_arr_append(rules, o);
  }
  yyjson_mut_obj_add_val(d, root, "rules", rules);
  yyjson_mut_obj_add_strcpy(d, root, "sort", p.sort.c_str());
  yyjson_mut_obj_add_int(d, root, "limit", p.limit);
  size_t len = 0;
  char* out = yyjson_mut_write(d, YYJSON_WRITE_PRETTY, &len);
  std::string s = out ? std::string(out, len) + "\n" : "{}\n";
  free(out);
  yyjson_mut_doc_free(d);
  return s;
}

bool SmartPlaylists::fromJson(const std::string& json, SmartPlaylist& out) {
  yyjson_doc* d = yyjson_read(json.data(), json.size(), 0);
  if (!d) return false;
  yyjson_val* root = yyjson_doc_get_root(d);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(d);
    return false;
  }
  auto str = [](yyjson_val* o, const char* k) {
    yyjson_val* v = yyjson_obj_get(o, k);
    return yyjson_is_str(v) ? std::string(yyjson_get_str(v)) : std::string();
  };
  SmartPlaylist p;
  if (yyjson_val* n = yyjson_obj_get(root, "name"); yyjson_is_str(n)) p.name = yyjson_get_str(n);
  p.matchAll = str(root, "match") != "any";
  size_t i, n;
  yyjson_val* e;
  yyjson_arr_foreach(yyjson_obj_get(root, "rules"), i, n, e) {
    SmartRule r;
    r.field = str(e, "field"), r.op = str(e, "op");
    yyjson_val* v = yyjson_obj_get(e, "value");
    if (yyjson_is_str(v)) r.text = yyjson_get_str(v), r.number = atof(r.text.c_str());
    else if (yyjson_is_num(v)) r.number = yyjson_get_num(v);
    p.rules.push_back(std::move(r));
  }
  std::string sort = str(root, "sort");
  if (!sort.empty()) p.sort = sort;
  if (yyjson_val* l = yyjson_obj_get(root, "limit"); yyjson_is_num(l)) p.limit = int(yyjson_get_num(l));
  yyjson_doc_free(d);
  out = std::move(p);
  return true;
}

}  // namespace rondo
