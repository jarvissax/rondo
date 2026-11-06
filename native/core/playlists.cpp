#include "playlists.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <unordered_map>

#include "fold.h"
#include "library.h"

namespace rondo {
namespace {

constexpr size_t kMaxName = 60;

bool endsWithNoCase(const std::string& s, const char* ext) {
  size_t n = strlen(ext);
  return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, ext) == 0;
}

std::string stem(const std::string& file) {
  std::string base = file.substr(file.rfind('/') + 1);
  size_t dot = base.rfind('.');
  return dot == std::string::npos ? base : base.substr(0, dot);
}

// "/storage/sdcard0/Playlists/../Music/x.flac" -> "/storage/sdcard0/Music/x.flac"
std::string normalize(const std::string& path) {
  std::deque<std::string> out;
  size_t at = 0;
  while (at <= path.size()) {
    size_t slash = path.find('/', at);
    std::string part = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
    if (part == "..") {
      if (!out.empty()) out.pop_back();
    } else if (!part.empty() && part != ".") {
      out.push_back(part);
    }
    if (slash == std::string::npos) break;
    at = slash + 1;
  }
  std::string s;
  for (auto& p : out) s += "/" + p;
  return s.empty() ? "/" : s;
}

}  // namespace

Playlists::Playlists(const Library& library, std::string card) : library_(library), card_(std::move(card)) {
  dir_ = card_ + "/Playlists";
}

std::string Playlists::clean(const std::string& name) {
  std::string s;
  for (unsigned char c : name) s += (strchr("\\/:*?\"<>|", c) && c) || c < 0x20 || c == 0x7f ? ' ' : char(c);
  std::string t;  // collapse whitespace
  for (char c : s) {
    bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (space && (t.empty() || t.back() == ' ')) continue;
    t += space ? ' ' : c;
  }
  while (!t.empty() && (t.back() == ' ' || t.back() == '.')) t.pop_back();
  size_t a = 0;
  while (a < t.size() && (t[a] == ' ' || t[a] == '.')) a++;
  t = t.substr(a);
  // take(60): at most 60 characters
  size_t chars = 0, cut = t.size();
  for (size_t i = 0; i < t.size(); i++) {
    if ((uint8_t(t[i]) & 0xc0) != 0x80 && chars++ == kMaxName) {
      cut = i;
      break;
    }
  }
  return t.substr(0, cut);
}

std::vector<Playlist> Playlists::all() const {
  std::vector<Playlist> out;
  DIR* d = opendir(dir_.c_str());
  if (!d) return out;
  while (dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    if (!endsWithNoCase(name, ".m3u8") && !endsWithNoCase(name, ".m3u")) continue;
    std::string path = dir_ + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    out.push_back({stem(name), read(path)});
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const Playlist& a, const Playlist& b) { return lower(a.name) < lower(b.name); });
  return out;
}

std::string Playlists::fileOf(const std::string& name) const {
  std::string want = lower(clean(name));
  DIR* d = opendir(dir_.c_str());
  if (!d) return "";
  std::string found;
  while (dirent* e = readdir(d)) {
    std::string f = e->d_name;
    if ((endsWithNoCase(f, ".m3u8") || endsWithNoCase(f, ".m3u")) && lower(stem(f)) == want) {
      found = dir_ + "/" + f;
      break;
    }
  }
  closedir(d);
  return found;
}

bool Playlists::get(const std::string& name, Playlist& out) const {
  std::string f = fileOf(name);
  if (f.empty()) return false;
  out = {stem(f), read(f)};
  return true;
}

std::string Playlists::resolve(const std::string& entry) const {
  std::string p = entry;
  std::replace(p.begin(), p.end(), '\\', '/');
  if ((p.size() > 2 && isalpha((unsigned char)p[0]) && p[1] == ':' && p[2] == '/') || p.find("://") != std::string::npos)
    return "";  // another computer's drive, or a URL
  return normalize(p[0] == '/' ? p : dir_ + "/" + p);
}

std::vector<std::string> Playlists::read(const std::string& file) const {
  std::vector<std::string> out;
  FILE* f = fopen(file.c_str(), "rb");
  if (!f) return out;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    std::string s = line;
    if (s.compare(0, 3, "\xef\xbb\xbf") == 0) s = s.substr(3);  // a byte order mark
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) continue;
    s = s.substr(a, b - a + 1);
    if (s[0] == '#') continue;
    std::string path = resolve(s);
    if (!path.empty() && std::find(out.begin(), out.end(), path) == out.end()) out.push_back(path);
  }
  fclose(f);
  return out;
}

bool Playlists::write(const std::string& file, const std::vector<std::string>& paths) const {
  std::unordered_map<std::string, const Track*> byPath;
  for (const Track& t : library_.tracks()) byPath[t.path] = &t;
  std::string text = "#EXTM3U\n";
  for (const std::string& p : paths) {
    auto it = byPath.find(p);
    if (it != byPath.end()) {
      const Track* t = it->second;
      std::string who = t->artist.empty() ? t->title : t->title.empty() ? t->artist : t->artist + " - " + t->title;
      text += "#EXTINF:" + std::to_string(t->durationSec >= 0 ? t->durationSec : -1) + "," + who + "\n";
    }
    std::string prefix = card_ + "/";
    text += (p.compare(0, prefix.size(), prefix) == 0 ? "../" + p.substr(prefix.size()) : p) + "\n";
  }
  std::string base = file.substr(file.rfind('/') + 1);
  std::string tmp = file.substr(0, file.rfind('/') + 1) + "." + base + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = fflush(f) == 0 && ok;
  fsync(fileno(f));
  fclose(f);
  if (!ok) return false;
  if (rename(tmp.c_str(), file.c_str()) != 0) {
    unlink(file.c_str());
    return rename(tmp.c_str(), file.c_str()) == 0;
  }
  return true;
}

std::string Playlists::create(const std::string& name, const std::vector<std::string>& paths) {
  std::string c = clean(name);
  if (c.empty()) return "Give it a name";
  if (!fileOf(c).empty()) return "There's already a playlist called \xe2\x80\x9c" + c + "\xe2\x80\x9d";
  mkdir(dir_.c_str(), 0775);
  struct stat st;
  if (stat(dir_.c_str(), &st) != 0) return "The card isn't available";
  return write(dir_ + "/" + c + ".m3u8", paths) ? "" : "Couldn't save the playlist";
}

std::string Playlists::update(const std::string& name, const std::string* newName, const std::vector<std::string>* paths) {
  std::string file = fileOf(name);
  if (file.empty()) return "That playlist is gone";
  std::vector<std::string> songs = paths ? *paths : read(file);
  std::string target = file;
  if (newName) {
    std::string c = clean(*newName);
    if (c.empty()) return "Give it a name";
    std::string other = fileOf(c);
    if (!other.empty() && other != file) return "There's already a playlist called \xe2\x80\x9c" + c + "\xe2\x80\x9d";
    target = dir_ + "/" + c + ".m3u8";
  }
  if (target == file) return write(file, songs) ? "" : "Couldn't save the playlist";
  if (strcasecmp(target.c_str(), file.c_str()) == 0) {  // only the case changes: FAT sees one file
    unlink(file.c_str());
    return write(target, songs) ? "" : "Couldn't save the playlist";
  }
  if (!write(target, songs)) return "Couldn't save the playlist";
  unlink(file.c_str());
  return "";
}

std::string Playlists::add(const std::string& name, const std::vector<std::string>& paths) {
  Playlist p;
  if (!get(name, p)) return "That playlist is gone";
  for (const auto& s : paths)
    if (std::find(p.paths.begin(), p.paths.end(), s) == p.paths.end()) p.paths.push_back(s);
  return update(name, nullptr, &p.paths);
}

std::string Playlists::remove(const std::string& name) {
  std::string file = fileOf(name);
  if (file.empty()) return "";
  return unlink(file.c_str()) == 0 ? "" : "Couldn't delete it";
}

void Playlists::forget(const std::string& path) {
  for (Playlist& p : all()) {
    auto it = std::find(p.paths.begin(), p.paths.end(), path);
    if (it == p.paths.end()) continue;
    p.paths.erase(it);
    update(p.name, nullptr, &p.paths);
  }
}

}  // namespace rondo
