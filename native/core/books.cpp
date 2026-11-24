// books.h: what's a book, how files group into books, chapters, and the spoken-word rules.
#include "books.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <unordered_map>

#include "fold.h"

namespace rondo {
namespace {

std::string asciiLower(std::string s) {
  for (char& c : s) c = char(tolower((unsigned char)c));
  return s;
}

// a folder or genre name as compared: lower case, without spaces, dashes and underscores
std::string squeezed(const std::string& s) {
  std::string out;
  for (char c : asciiLower(s))
    if (c != ' ' && c != '-' && c != '_' && c != '.') out += c;
  return out;
}

bool bookWord(const std::string& s) {
  std::string q = squeezed(s);
  return q == "audiobooks" || q == "audiobook" || q == "audiolibros" || q == "audiolibro";
}

std::string parentOf(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
std::string nameOf(const std::string& path) {
  size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string stem(const std::string& name) {
  size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// the storage a path is on: its first two parts ("/storage/sdcard0")
std::string storageOf(const std::string& path) {
  if (path.empty() || path[0] != '/') return "";
  size_t a = path.find('/', 1);
  if (a == std::string::npos) return "";
  size_t b = path.find('/', a + 1);
  return b == std::string::npos ? "" : path.substr(0, b);
}

// a disc folder's name with its "CD 2" part taken off ("Dune (Disc 1)" -> "Dune"); "" when the whole
// name is the disc's ("CD 1")
std::string withoutDisc(const std::string& name, bool* matched) {
  std::string n = asciiLower(name);
  // trailing ")" or "]", then digits, then separators, then the word
  size_t end = n.size();
  while (end > 0 && (n[end - 1] == ')' || n[end - 1] == ']' || n[end - 1] == ' ')) end--;
  size_t digits = end;
  while (digits > 0 && isdigit((unsigned char)n[digits - 1])) digits--;
  *matched = false;
  if (digits == end) return name;
  size_t w = digits;
  while (w > 0 && (n[w - 1] == ' ' || n[w - 1] == '-' || n[w - 1] == '_' || n[w - 1] == '.' || n[w - 1] == '#')) w--;
  for (const char* word : {"cd", "disc", "disk", "part", "parte", "disco", "vol", "volume"}) {
    size_t len = strlen(word);
    if (w < len || n.compare(w - len, len, word) != 0) continue;
    size_t start = w - len;
    // the word stands alone: the name's start, or after a space, a bracket or a dash
    if (start > 0 && isalnum((unsigned char)n[start - 1])) continue;
    *matched = true;
    std::string rest = name.substr(0, start);
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '(' || rest.back() == '[' || rest.back() == '-' || rest.back() == ','))
      rest.pop_back();
    return rest;
  }
  return name;
}

BookReader g_reader = nullptr;

// the value most of them have ("" none)
std::string mostCommon(const std::vector<std::string>& values) {
  std::map<std::string, int> count;
  std::string best;
  int most = 0;
  for (const std::string& v : values) {
    if (v.empty()) continue;
    int n = ++count[v];
    if (n > most) most = n, best = v;
  }
  return best;
}

std::string trimmed(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

std::string bookRoot(const std::string& path) {
  std::string storage = storageOf(path);
  if (storage.empty() || path.size() <= storage.size() + 1) return "";
  size_t end = path.find('/', storage.size() + 1);
  if (end == std::string::npos) return "";  // a file at the storage's root
  std::string folder = path.substr(storage.size() + 1, end - storage.size() - 1);
  return bookWord(folder) ? path.substr(0, end) : "";
}

bool inBookFolder(const std::string& path) { return !bookRoot(path).empty(); }

bool isM4b(const std::string& path) {
  size_t dot = path.rfind('.');
  return dot != std::string::npos && asciiLower(path.substr(dot + 1)) == "m4b";
}

bool isBookGenre(const std::string& genre) { return bookWord(genre); }

void setBookReader(BookReader reader) { g_reader = reader; }

bool readBookInfo(const char* path, BookFileInfo& out) { return g_reader && g_reader(path, out); }

std::string encodeChapters(const std::vector<FileChapter>& c) {
  std::string s;
  for (const FileChapter& f : c) {
    std::string t = f.title;
    for (char& ch : t)
      if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    s += std::to_string(f.startMs) + "\t" + std::to_string(f.endMs) + "\t" + t + "\n";
  }
  return s;
}

std::vector<FileChapter> decodeChapters(const std::string& s) {
  std::vector<FileChapter> out;
  size_t at = 0;
  while (at < s.size()) {
    size_t nl = s.find('\n', at);
    if (nl == std::string::npos) nl = s.size();
    std::string line = s.substr(at, nl - at);
    at = nl + 1;
    size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
    if (t2 == std::string::npos) continue;
    FileChapter f;
    f.startMs = atoll(line.substr(0, t1).c_str());
    f.endMs = atoll(line.substr(t1 + 1, t2 - t1 - 1).c_str());
    f.title = line.substr(t2 + 1);
    out.push_back(std::move(f));
  }
  return out;
}

bool isDiscFolder(const std::string& name) {
  bool matched;
  withoutDisc(name, &matched);
  return matched;
}

std::string bookKeyOf(const BookFile& f) {
  if (isM4b(f.path) || f.ownChapters) return f.path;
  std::string dir = parentOf(f.path), root = bookRoot(f.path);
  if (dir.empty() || dir == root || dir == storageOf(f.path)) return f.path;  // a file by itself
  bool disc;
  std::string rest = withoutDisc(nameOf(dir), &disc);
  if (!disc) return dir;
  std::string up = parentOf(dir);
  // "Book/CD 1": the book is the folder above; "Audiobooks/Book (CD 1)": the folders of one name,
  // whatever their disc ("Audiobooks/Book")
  if (up != root && up != storageOf(f.path) && !up.empty() && rest.empty()) return up;
  if (rest.empty()) return dir;
  return up + "/" + rest;
}

bool naturalLess(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    unsigned char x = (unsigned char)a[i], y = (unsigned char)b[j];
    if (isdigit(x) && isdigit(y)) {
      size_t i2 = i, j2 = j;
      while (i2 < a.size() && a[i2] == '0') i2++;
      while (j2 < b.size() && b[j2] == '0') j2++;
      size_t ie = i2, je = j2;
      while (ie < a.size() && isdigit((unsigned char)a[ie])) ie++;
      while (je < b.size() && isdigit((unsigned char)b[je])) je++;
      if (ie - i2 != je - j2) return ie - i2 < je - j2;
      int c = a.compare(i2, ie - i2, b, j2, je - j2);
      if (c) return c < 0;
      i = ie, j = je;
      continue;
    }
    int lx = tolower(x), ly = tolower(y);
    if (lx != ly) return lx < ly;
    i++, j++;
  }
  return a.size() - i < b.size() - j;
}

void orderBookFiles(std::vector<BookFile>& files) {
  std::stable_sort(files.begin(), files.end(), [](const BookFile& a, const BookFile& b) {
    if (a.disc != b.disc) return a.disc < b.disc;
    if (a.track != b.track) return a.track < b.track;
    return naturalLess(nameOf(a.path), nameOf(b.path));
  });
}

std::vector<BookGroup> groupBooks(std::vector<BookFile> files) {
  std::unordered_map<std::string, size_t> at;
  std::vector<BookGroup> books;
  for (BookFile& f : files) {
    std::string key = bookKeyOf(f);
    auto it = at.find(key);
    if (it == at.end()) {
      at[key] = books.size();
      books.push_back({key, "", "", {}, 0});
      it = at.find(key);
    }
    books[it->second].files.push_back(std::move(f));
  }
  for (BookGroup& b : books) {
    orderBookFiles(b.files);
    std::vector<std::string> albums, authors;
    for (const BookFile& f : b.files) {
      albums.push_back(trimmed(f.album));
      authors.push_back(trimmed(!f.albumArtist.empty() ? f.albumArtist : f.artist));
      if (f.durationMs > 0) b.durationMs += f.durationMs;
    }
    b.title = mostCommon(albums);
    b.author = mostCommon(authors);
    bool single = b.files.size() == 1 && b.key == b.files[0].path;
    if (b.title.empty() && single) b.title = trimmed(b.files[0].title);
    if (b.title.empty()) {
      // the folder's (or the file's) own name; "Author - Title" gives the author too
      std::string name = single ? stem(nameOf(b.key)) : nameOf(b.key);
      size_t sep = name.find(" - ");
      if (sep != std::string::npos && sep > 0) {
        if (b.author.empty()) b.author = trimmed(name.substr(0, sep));
        name = name.substr(sep + 3);
      }
      b.title = trimmed(name);
      if (b.title.empty()) b.title = nameOf(b.key);
    }
  }
  std::vector<std::pair<std::string, size_t>> order;
  for (size_t i = 0; i < books.size(); i++) order.push_back({fold(books[i].title), i});
  std::stable_sort(order.begin(), order.end(), [&](const auto& x, const auto& y) {
    if (x.first != y.first) return naturalLess(x.first, y.first);
    return books[x.second].key < books[y.second].key;
  });
  std::vector<BookGroup> sorted;
  sorted.reserve(books.size());
  for (auto& o : order) sorted.push_back(std::move(books[o.second]));
  return sorted;
}

std::vector<Chapter> buildChapters(const std::vector<int64_t>& fileMs, const std::vector<std::vector<FileChapter>>& infos,
                                   const std::vector<std::string>& titles) {
  std::vector<Chapter> out;
  int64_t before = 0;
  for (size_t i = 0; i < fileMs.size(); i++) {
    int64_t len = std::max<int64_t>(0, fileMs[i]);
    const std::vector<FileChapter>* own = i < infos.size() && infos[i].size() >= 2 ? &infos[i] : nullptr;
    if (own) {
      std::vector<FileChapter> c = *own;
      std::stable_sort(c.begin(), c.end(), [](const FileChapter& a, const FileChapter& b) { return a.startMs < b.startMs; });
      for (size_t k = 0; k < c.size(); k++) {
        int64_t s = std::max<int64_t>(0, c[k].startMs);
        if (len > 0 && s >= len) break;  // past the end the file has
        int64_t e = k + 1 < c.size() ? c[k + 1].startMs : (len > 0 ? len : c[k].endMs);
        if (len > 0) e = std::min(e, len);
        if (e <= s) continue;  // empty (two at one time)
        Chapter ch;
        ch.title = trimmed(c[k].title);
        ch.file = int(i);
        ch.startMs = s, ch.endMs = e;
        ch.bookStartMs = before + s;
        out.push_back(std::move(ch));
      }
    } else {
      Chapter ch;
      ch.title = i < titles.size() ? trimmed(titles[i]) : std::string();
      ch.file = int(i);
      ch.startMs = 0, ch.endMs = len;
      ch.bookStartMs = before;
      out.push_back(std::move(ch));
    }
    before += len;
  }
  return out;
}

bool skipsTime(const std::vector<Chapter>& chapters) { return chapters.size() <= 1; }

int64_t bookMsOf(const std::vector<int64_t>& fileMs, Place p) {
  int64_t ms = 0;
  for (int i = 0; i < p.file && i < int(fileMs.size()); i++) ms += std::max<int64_t>(0, fileMs[size_t(i)]);
  return ms + std::max<int64_t>(0, p.ms);
}

Place placeAt(const std::vector<int64_t>& fileMs, int64_t bookMs) {
  Place p;
  bookMs = std::max<int64_t>(0, bookMs);
  for (size_t i = 0; i < fileMs.size(); i++) {
    int64_t len = std::max<int64_t>(0, fileMs[i]);
    if (bookMs < len || i + 1 == fileMs.size()) {
      p.file = int(i);
      p.ms = len > 0 ? std::min(bookMs, len) : bookMs;
      return p;
    }
    bookMs -= len;
  }
  return p;
}

int chapterAt(const std::vector<Chapter>& chapters, Place p) {
  int found = chapters.empty() ? -1 : 0;
  for (int i = 0; i < int(chapters.size()); i++) {
    const Chapter& c = chapters[size_t(i)];
    if (c.file < p.file || (c.file == p.file && c.startMs <= p.ms)) found = i;
    else break;
  }
  return found;
}

Place nextPlace(const std::vector<Chapter>& chapters, const std::vector<int64_t>& fileMs, Place at) {
  if (skipsTime(chapters)) {
    int64_t len = at.file < int(fileMs.size()) ? fileMs[size_t(at.file)] : -1;
    Place p = at;
    p.ms = at.ms + kSkipForwardMs;
    if (len > 0) p.ms = std::min(p.ms, std::max<int64_t>(0, len - 1000));
    return p;
  }
  int c = chapterAt(chapters, at);
  if (c < 0 || c + 1 >= int(chapters.size())) return {-1, 0};
  return {chapters[size_t(c + 1)].file, chapters[size_t(c + 1)].startMs};
}

Place previousPlace(const std::vector<Chapter>& chapters, const std::vector<int64_t>& fileMs, Place at) {
  (void)fileMs;
  if (skipsTime(chapters)) return {at.file, std::max<int64_t>(0, at.ms - kSkipBackMs)};
  int c = chapterAt(chapters, at);
  if (c < 0) return {at.file, 0};
  const Chapter& ch = chapters[size_t(c)];
  if (at.ms - ch.startMs > kPreviousRestartMs || c == 0) return {ch.file, ch.startMs};
  return {chapters[size_t(c - 1)].file, chapters[size_t(c - 1)].startMs};
}

int64_t rewindAfterPause(int64_t pausedMs) {
  if (pausedMs < 3000) return 0;
  if (pausedMs < 10 * 60000LL) return 2000;
  if (pausedMs < 60 * 60000LL) return 5000;
  if (pausedMs < 24 * 3600000LL) return 10000;
  return 30000;
}

int clampSpeed(int percent) {
  int best = 100, gap = 1 << 30;
  for (int s : kSpeeds)
    if (abs(s - percent) < gap) gap = abs(s - percent), best = s;
  return best;
}

std::string speedText(int percent) {
  percent = clampSpeed(percent);
  int whole = percent / 100, part = percent % 100;
  std::string s = std::to_string(whole);
  if (part) {
    s += '.';
    s += char('0' + part / 10);
    if (part % 10) s += char('0' + part % 10);
  }
  return s + "\xc3\x97";  // ×
}

}  // namespace rondo
