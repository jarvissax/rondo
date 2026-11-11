// Files on the card and the internal storage, for Library › Files on the Y1 and the dashboard's file
// manager alike. Every change keeps the library (plays, ratings), the playlists and the queue in step.
#pragma once
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace rondo::shell::files {

extern const char* const kRoots[2];  // the card, the internal storage
std::string rootName(const std::string& root);  // "Card", "Internal storage"
bool isRoot(const std::string& path);
bool inside(const std::string& path);  // under a root (not a root itself), no ".."
bool isDir(const std::string& path);
std::string entryName(const std::string& typed);  // a name FAT can hold, or "" (no dot files)
int64_t freeBytes(const std::string& root), totalBytes(const std::string& root);

// UI thread (the library is): "" when done, else what went wrong, in words for the user
std::string makeFolder(const std::string& dir, const std::string& name);
std::string renameEntry(const std::string& from, const std::string& name, std::string* to = nullptr);

struct Result {
  int done = 0;                     // files and folders moved, copied or deleted
  std::vector<std::string> errors;  // "name: what went wrong"
};
Result remove(const std::vector<std::string>& paths);  // files, and folders with all they hold

// Moving or copying files and folders into a folder. A move on the same storage is a rename, done at
// once; a copy, or a move between the card and the internal storage, copies on a thread of its own
// (then deletes, for a move). `done` comes on the UI thread. One transfer at a time: false while one runs.
bool transfer(std::vector<std::string> paths, const std::string& to, bool copy, std::function<void(const Result&)> done);
// the transfer under way: "" when none, else "Copying 3 of 12 · 45%"
std::string transferStatus();

}  // namespace rondo::shell::files
