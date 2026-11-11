// file_ops.h.
#include "file_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <thread>

#include "../core/organize.h"
#include "app.h"
#include "main_queue.h"
#include "screens.h"

namespace rondo::shell::files {

const char* const kRoots[2] = {"/storage/sdcard0", "/storage/sdcard1"};

namespace {

// which storage a path is on (0 the card, 1 the internal storage, -1 neither)
int rootOf(const std::string& p) {
  for (int i = 0; i < 2; i++) {
    std::string r = kRoots[i];
    if (p == r || p.compare(0, r.size() + 1, r + "/") == 0) return i;
  }
  return -1;
}
std::string nameOf(const std::string& p) { return p.substr(p.rfind('/') + 1); }
bool exists(const std::string& p) {
  struct stat st;
  return lstat(p.c_str(), &st) == 0;
}
// what's in a folder, files and folders, deepest first (a delete's order)
void walkDeep(const std::string& dir, std::vector<std::string>& out, int depth = 0) {
  if (depth > 32) return;
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> here;
  while (dirent* e = readdir(d))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) here.push_back(dir + "/" + e->d_name);
  closedir(d);
  for (const std::string& p : here) {
    if (isDir(p)) walkDeep(p, out, depth + 1);
    out.push_back(p);
  }
}
// "Name (2).ext": a copy into a folder that has the name already
std::string freeName(const std::string& dir, const std::string& name) {
  if (!exists(dir + "/" + name)) return name;
  size_t dot = name.rfind('.');
  bool ext = dot != std::string::npos && dot > 0 && !isDir(dir + "/" + name);
  std::string base = ext ? name.substr(0, dot) : name, tail = ext ? name.substr(dot) : "";
  for (int n = 2; n < 1000; n++) {
    std::string c = base + " (" + std::to_string(n) + ")" + tail;
    if (!exists(dir + "/" + c)) return c;
  }
  return "";
}
std::string problem(int err) {
  switch (err) {
    case ENOSPC: return "not enough space";
    case EROFS: return "the storage is read-only";
    case EACCES:
    case EPERM: return "not allowed";
    case ENOENT: return "it isn't there any more";
    case ENAMETOOLONG: return "the name is too long";
    default: return strerror(err);
  }
}

// --- the transfer under way (one at a time) ---
std::mutex g_mu;
bool g_busy = false;
bool g_copying = false;
int g_item = 0, g_items = 0;
int64_t g_doneBytes = 0, g_totalBytes = 0;

int64_t sizeOf(const std::string& p) {
  struct stat st;
  if (lstat(p.c_str(), &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) return int64_t(st.st_size);
  int64_t n = 0;
  std::vector<std::string> all;
  walkDeep(p, all);
  for (const std::string& q : all)
    if (lstat(q.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) n += int64_t(st.st_size);
  return n;
}
// one file's bytes, its time kept (the library and Recently added go by it); errno on failure
int copyFile(const std::string& from, const std::string& to) {
  int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return errno;
  struct stat st;
  fstat(in, &st);
  int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
  if (out < 0) {
    int e = errno;
    close(in);
    return e;
  }
  std::unique_ptr<char[]> buf(new char[256 * 1024]);
  int err = 0;
  for (;;) {
    ssize_t n = read(in, buf.get(), 256 * 1024);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      err = errno;
      break;
    }
    if (n == 0) break;
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, buf.get() + off, size_t(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        err = w < 0 ? errno : ENOSPC;
        break;
      }
      off += w;
    }
    if (err) break;
    std::lock_guard<std::mutex> l(g_mu);
    g_doneBytes += n;
  }
  close(in);
  if (close(out) != 0 && !err) err = errno;
  if (err) {
    unlink(to.c_str());
    return err;
  }
  struct timeval times[2] = {{st.st_atime, 0}, {st.st_mtime, 0}};
  utimes(to.c_str(), times);
  return 0;
}
// a file or a folder with all it holds; errno of the first failure
int copyTree(const std::string& from, const std::string& to) {
  if (!isDir(from)) return copyFile(from, to);
  if (mkdir(to.c_str(), 0775) != 0 && errno != EEXIST) return errno;
  DIR* d = opendir(from.c_str());
  if (!d) return errno;
  std::vector<std::string> names;
  while (dirent* e = readdir(d))
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
  closedir(d);
  for (const std::string& n : names)
    if (int e = copyTree(from + "/" + n, to + "/" + n)) return e;
  return 0;
}

}  // namespace

std::string rootName(const std::string& root) { return root == kRoots[0] ? "Card" : "Internal storage"; }
bool isRoot(const std::string& p) { return p == kRoots[0] || p == kRoots[1]; }
bool inside(const std::string& p) { return p.find("..") == std::string::npos && rootOf(p) >= 0 && !isRoot(p); }
bool isDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
std::string entryName(const std::string& typed) {
  std::string n = cardName(typed);
  return n.empty() || n[0] == '.' ? std::string() : n;
}
int64_t freeBytes(const std::string& root) {
  struct statfs s;
  return statfs(root.c_str(), &s) == 0 ? int64_t(s.f_bavail) * int64_t(s.f_bsize) : -1;
}
int64_t totalBytes(const std::string& root) {
  struct statfs s;
  return statfs(root.c_str(), &s) == 0 ? int64_t(s.f_blocks) * int64_t(s.f_bsize) : -1;
}

std::string makeFolder(const std::string& dir, const std::string& typed) {
  std::string name = entryName(typed);
  if (name.empty()) return "Type a name";
  if (!(isRoot(dir) || inside(dir)) || !isDir(dir)) return "That folder isn't there";
  std::string p = dir + "/" + name;
  if (exists(p)) return "Something by that name is there already";
  if (mkdir(p.c_str(), 0775) != 0) return "Couldn't make it: " + problem(errno);
  return "";
}

std::string renameEntry(const std::string& from, const std::string& typed, std::string* to) {
  std::string name = entryName(typed);
  if (name.empty()) return "Type a name";
  if (!inside(from) || !exists(from)) return "It isn't there any more";
  std::string dir = from.substr(0, from.rfind('/')), target = dir + "/" + name;
  if (target == from) {
    if (to) *to = from;
    return "";
  }
  // only the case changing (FAT keeps one of the two): that's this file, not another
  if (strcasecmp(target.c_str(), from.c_str()) != 0 && exists(target)) return "Something by that name is there already";
  if (rename(from.c_str(), target.c_str()) != 0) return "Couldn't rename it: " + problem(errno);
  filesMoved({{from, target}});
  if (to) *to = target;
  return "";
}

Result remove(const std::vector<std::string>& paths) {
  Result r;
  for (const std::string& p : paths) {
    if (!inside(p)) continue;
    std::vector<std::string> all;
    if (isDir(p)) walkDeep(p, all);
    all.push_back(p);
    bool ok = true;
    for (const std::string& q : all) {
      bool gone;
      if (isDir(q)) {
        gone = rmdir(q.c_str()) == 0;
      } else if (const Track* t = g_library.find(q)) {  // a song or video: out of the library too
        gone = g_library.deleteFile(t->path);
        if (gone) g_playlists.forget(q);
      } else {
        gone = unlink(q.c_str()) == 0 || !exists(q);  // an .lrc deleteFile took already
      }
      if (!gone && ok) r.errors.push_back(nameOf(q) + ": " + problem(errno)), ok = false;
    }
    if (ok) r.done++;
  }
  return r;
}

bool transfer(std::vector<std::string> paths, const std::string& to, bool copy, std::function<void(const Result&)> done) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_busy) return false;
  }
  auto result = std::make_shared<Result>();
  if (!(isRoot(to) || inside(to)) || !isDir(to)) {
    result->errors.push_back("That folder isn't there");
    done(*result);
    return true;
  }
  std::vector<std::pair<std::string, std::string>> renamed, pending;  // from, to
  for (const std::string& from : paths) {
    if (!inside(from) || !exists(from)) continue;
    std::string name = nameOf(from);
    if (to == from || to.compare(0, from.size() + 1, from + "/") == 0) {
      result->errors.push_back(name + ": can't go into itself");
      continue;
    }
    bool sameFolder = from.substr(0, from.rfind('/')) == to;
    if (!copy && sameFolder) continue;  // already there
    std::string target = to + "/" + (copy ? freeName(to, name) : name);
    if (target == to + "/" || (!copy && exists(target))) {
      result->errors.push_back(name + ": something by that name is there already");
      continue;
    }
    // a move on one storage: a rename, now
    if (!copy && rootOf(from) == rootOf(to)) {
      if (rename(from.c_str(), target.c_str()) == 0) {
        renamed.push_back({from, target});
        result->done++;
        continue;
      }
      if (errno != EXDEV) {
        result->errors.push_back(name + ": " + problem(errno));
        continue;
      }
    }
    pending.push_back({from, target});
  }
  if (!renamed.empty()) filesMoved(renamed);
  if (pending.empty()) {
    done(*result);
    return true;
  }
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_busy = true, g_copying = copy, g_item = 0, g_items = int(pending.size()), g_doneBytes = 0, g_totalBytes = 0;
  }
  std::thread([pending, copy, result, done] {
    int64_t total = 0;
    for (const auto& p : pending) total += sizeOf(p.first);
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_totalBytes = total;
    }
    auto copied = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
    for (size_t k = 0; k < pending.size(); k++) {
      {
        std::lock_guard<std::mutex> l(g_mu);
        g_item = int(k) + 1;
      }
      const auto& p = pending[k];
      if (int e = copyTree(p.first, p.second)) {
        // what was copied of it goes again: a half folder is worse than none
        std::vector<std::string> part;
        if (isDir(p.second)) walkDeep(p.second, part);
        part.push_back(p.second);
        for (const std::string& q : part) isDir(q) ? rmdir(q.c_str()) : unlink(q.c_str());
        result->errors.push_back(nameOf(p.first) + ": " + problem(e));
        if (e == ENOSPC) break;  // the rest won't fit either
        continue;
      }
      copied->push_back(p);
    }
    postToMain([copied, copy, result, done] {
      if (!copy) {
        // the moved ones leave where they were; the library follows them (plays, ratings, playlists)
        for (const auto& p : *copied) {
          std::vector<std::string> all;
          if (isDir(p.first)) walkDeep(p.first, all);
          all.push_back(p.first);
          for (const std::string& q : all) isDir(q) ? rmdir(q.c_str()) : unlink(q.c_str());
        }
        filesMoved(*copied);
      }
      result->done += int(copied->size());
      g_library.scanAsync();  // copies are new files: read them
      {
        std::lock_guard<std::mutex> l(g_mu);
        g_busy = false;
      }
      done(*result);
    });
  }).detach();
  return true;
}

std::string transferStatus() {
  std::lock_guard<std::mutex> l(g_mu);
  if (!g_busy) return "";
  std::string s = std::string(g_copying ? "Copying " : "Moving ") + std::to_string(std::max(1, g_item)) + " of " + std::to_string(g_items);
  if (g_totalBytes > 0) s += " \xc2\xb7 " + std::to_string(int(g_doneBytes * 100 / g_totalBytes)) + "%";
  return s;
}

}  // namespace rondo::shell::files
