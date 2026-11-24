// Audiobooks on the card (Library › Audiobooks): which files are books, how they group into books,
// their chapters, and the rules of spoken-word playback (where Next and Previous go, how far a resume
// rewinds, when a book counts as finished). Pure logic, no files read here: host-tested in
// native/tests/host/books_test.cpp. What FFmpeg reads of a book file (its chapters, its length to the
// millisecond, the narrator) comes through setBookReader(), as the tags' fallback does.
#pragma once
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace rondo {

// ---- what's a book ----

// Under an Audiobooks folder at the root of the card or the internal storage, in any letter case (the
// stock firmware's /storage/sdcard0/Audiobooks; "Audiobook", "Audio books" and Spanish "Audiolibros"
// count too). bookRoot(): that folder's path, "" when the file isn't under one.
std::string bookRoot(const std::string& path);
bool inBookFolder(const std::string& path);
bool isM4b(const std::string& path);
// "Audiobook" or "Audiobooks" (and "Audio book", "Audiolibro"), any case
bool isBookGenre(const std::string& genre);
// what the library asks first, before any tag is read: a file whose book info is worth reading
inline bool bookCandidate(const std::string& path) { return inBookFolder(path) || isM4b(path); }

// ---- what FFmpeg reads of a book file (player/probe.cpp, through setBookReader) ----

struct FileChapter {
  int64_t startMs = 0, endMs = 0;
  std::string title;
};
struct BookFileInfo {
  int64_t durationMs = -1;  // -1: not known
  std::string narrator;     // "composer", "narrator" or "performer" tags ("" none)
  std::vector<FileChapter> chapters;  // the file's own (MP4 chapters, ID3 CHAP); empty: none
};
using BookReader = bool (*)(const char* path, BookFileInfo& out);
void setBookReader(BookReader reader);  // at startup, before any scan
bool readBookInfo(const char* path, BookFileInfo& out);  // false: no reader, or not understood
// kept in the library's database as text (one chapter a line: start, end, title, tab-separated)
std::string encodeChapters(const std::vector<FileChapter>& c);
std::vector<FileChapter> decodeChapters(const std::string& s);

// ---- grouping files into books ----

// One file of the library that is a book's (its tags as the library read them).
struct BookFile {
  std::string path;
  std::string title, album, artist, albumArtist;
  int disc = 0, track = 0;
  int64_t durationMs = -1;
  bool ownChapters = false;  // the file has chapters of its own (two or more)
};

// Which book a file belongs to: an .m4b, a file with chapters of its own, or a file right in the
// Audiobooks folder is a book by itself (its path); else the folder it's in is the book, a disc folder
// ("CD 1", "Disc 2", "Part 3") being part of the folder above it.
std::string bookKeyOf(const BookFile& f);
bool isDiscFolder(const std::string& name);
// "Chapter 2" before "Chapter 10": names compared with their numbers as numbers, ignoring case
bool naturalLess(const std::string& a, const std::string& b);
// a book's files in their order: disc, track, then name
void orderBookFiles(std::vector<BookFile>& files);

struct BookGroup {
  std::string key;  // the folder (a folder book) or the file
  std::string title, author;
  std::vector<BookFile> files;  // in order
  int64_t durationMs = 0;       // the files' lengths added up (the known ones)
};
// The books in these files, by title (ignoring case and accents)
std::vector<BookGroup> groupBooks(std::vector<BookFile> files);

// ---- chapters and places ----

// A place in a book: a file of it (its index in the book's order) and a time in that file.
struct Place {
  int file = 0;
  int64_t ms = 0;
  bool operator==(const Place& o) const { return file == o.file && ms == o.ms; }
};

struct Chapter {
  std::string title;
  int file = 0;
  int64_t startMs = 0, endMs = 0;  // in that file
  int64_t bookStartMs = 0;         // from the book's start
};

// The book's chapters: each file's own, or the file itself as one chapter. fileMs: each file's length;
// infos: each file's chapters (empty: none). titles: each file's title, for a file that is a chapter.
std::vector<Chapter> buildChapters(const std::vector<int64_t>& fileMs, const std::vector<std::vector<FileChapter>>& infos,
                                   const std::vector<std::string>& titles);
// A book whose Next and Previous skip time (one file, no chapters of its own) instead of moving a chapter
bool skipsTime(const std::vector<Chapter>& chapters);

int64_t bookMsOf(const std::vector<int64_t>& fileMs, Place p);
Place placeAt(const std::vector<int64_t>& fileMs, int64_t bookMs);
// the chapter a place is in (-1: none)
int chapterAt(const std::vector<Chapter>& chapters, Place p);

// Next: the next chapter's start (a book without chapters: 30 s on); -1 file when there is none (the
// book's end). Previous: within 3 s of the chapter's start, the chapter before (the first stays at its
// start), else this chapter's start (a book without chapters: 15 s back).
Place nextPlace(const std::vector<Chapter>& chapters, const std::vector<int64_t>& fileMs, Place at);
Place previousPlace(const std::vector<Chapter>& chapters, const std::vector<int64_t>& fileMs, Place at);
constexpr int64_t kSkipForwardMs = 30000, kSkipBackMs = 15000, kPreviousRestartMs = 3000;

// Smart rewind: how far back a resume starts after a pause this long (ms): nothing after a moment,
// about 2 s after a short pause, 5 s after ten minutes, 10 s after an hour, 30 s after a day.
int64_t rewindAfterPause(int64_t pausedMs);
// Finished: in the last 30 s, or at the end
constexpr int64_t kFinishedWithinMs = 30000;
inline bool finishedAt(int64_t bookMs, int64_t lengthMs) { return lengthMs > 0 && bookMs >= lengthMs - kFinishedWithinMs; }

// The speeds (percent) Speed offers
constexpr int kSpeeds[] = {75, 100, 125, 150, 175, 200, 250, 300};
int clampSpeed(int percent);  // one of kSpeeds (the nearest)
// "1.5×", "1×", "0.75×"
std::string speedText(int percent);

}  // namespace rondo
