// LyricsSource.kt and EmbeddedLyrics.kt, natively; see lyrics.h.
#include "lyrics.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>

namespace rondo {
namespace {

bool isFile(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool readAll(const std::string& p, std::string& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[16384];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

// Kotlin's trim(): whitespace at both ends (and the NUL terminators tags end with)
std::string trim(const std::string& s) {
  auto space = [](unsigned char c) { return c == 0 || isspace(c); };
  size_t a = 0, b = s.size();
  while (a < b && space((unsigned char)s[a])) a++;
  while (b > a && space((unsigned char)s[b - 1])) b--;
  // a no-break space (U+00A0) counts as whitespace too
  while (b - a >= 2 && (unsigned char)s[a] == 0xc2 && (unsigned char)s[a + 1] == 0xa0) a += 2;
  while (b - a >= 2 && (unsigned char)s[b - 2] == 0xc2 && (unsigned char)s[b - 1] == 0xa0) b -= 2;
  return s.substr(a, b - a);
}

void appendUtf8(std::string& o, uint32_t cp) {
  if (cp < 0x80) {
    o += char(cp);
  } else if (cp < 0x800) {
    o += char(0xc0 | cp >> 6), o += char(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    o += char(0xe0 | cp >> 12), o += char(0x80 | ((cp >> 6) & 0x3f)), o += char(0x80 | (cp & 0x3f));
  } else {
    o += char(0xf0 | cp >> 18), o += char(0x80 | ((cp >> 12) & 0x3f)), o += char(0x80 | ((cp >> 6) & 0x3f)),
        o += char(0x80 | (cp & 0x3f));
  }
}

// Java's "UTF-16" (a byte order mark picks the order, else big-endian) and "UTF-16BE"
std::string fromUtf16(const uint8_t* b, size_t n, bool bomAllowed) {
  bool be = true;
  size_t i = 0;
  if (bomAllowed && n >= 2) {
    if (b[0] == 0xfe && b[1] == 0xff) i = 2;
    else if (b[0] == 0xff && b[1] == 0xfe) be = false, i = 2;
  }
  std::string o;
  for (; i + 1 < n; i += 2) {
    uint32_t u = be ? uint32_t(b[i]) << 8 | b[i + 1] : uint32_t(b[i + 1]) << 8 | b[i];
    if (u >= 0xd800 && u < 0xdc00 && i + 3 < n) {
      uint32_t l = be ? uint32_t(b[i + 2]) << 8 | b[i + 3] : uint32_t(b[i + 3]) << 8 | b[i + 2];
      if (l >= 0xdc00 && l < 0xe000) {
        appendUtf8(o, 0x10000 + ((u - 0xd800) << 10) + (l - 0xdc00));
        i += 2;
        continue;
      }
    }
    appendUtf8(o, u >= 0xd800 && u < 0xe000 ? 0xfffd : u);
  }
  return o;
}

std::string fromLatin1(const uint8_t* b, size_t n) {
  std::string o;
  for (size_t i = 0; i < n; i++) appendUtf8(o, b[i]);
  return o;
}

uint32_t le32(const uint8_t* b) { return b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24; }
uint32_t be32(const uint8_t* b) { return uint32_t(b[0]) << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
uint32_t syncsafe(const uint8_t* b) { return (b[0] & 0x7f) << 21 | (b[1] & 0x7f) << 14 | (b[2] & 0x7f) << 7 | (b[3] & 0x7f); }

// [mm:ss], [mm:ss.x..xxx]: its end and its time in ms, when one starts at i
bool stampAt(const std::string& s, size_t i, size_t* end, int64_t* ms) {
  size_t n = s.size();
  if (i >= n || s[i] != '[') return false;
  size_t j = i + 1;
  int64_t mins = 0, secs = 0, frac = 0;
  int d = 0;
  while (j < n && isdigit((unsigned char)s[j]) && d < 3) mins = mins * 10 + (s[j] - '0'), j++, d++;
  if (d == 0 || j >= n || s[j] != ':') return false;
  j++;
  d = 0;
  while (j < n && isdigit((unsigned char)s[j]) && d < 2) secs = secs * 10 + (s[j] - '0'), j++, d++;
  if (d == 0 || j >= n) return false;
  int fd = 0;
  if (s[j] == '.' || s[j] == ':') {
    size_t k = j + 1;
    while (k < n && isdigit((unsigned char)s[k]) && fd < 3) frac = frac * 10 + (s[k] - '0'), k++, fd++;
    if (fd == 0 || k >= n || s[k] != ']') return false;
    j = k;
  } else if (s[j] != ']') {
    return false;
  }
  *end = j + 1;
  int64_t f = fd == 0 ? 0 : fd == 1 ? frac * 100 : fd == 2 ? frac * 10 : frac;
  *ms = (mins * 60 + secs) * 1000 + f;
  return true;
}

bool hasStamp(const std::string& s) {
  size_t end;
  int64_t ms;
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] == '[' && stampAt(s, i, &end, &ms)) return true;
  return false;
}

// [offset:+/-ms], ignoring case, anywhere
int64_t lrcOffset(const std::string& text) {
  for (size_t i = 0; i + 8 < text.size(); i++) {
    if (text[i] != '[' || strncasecmp(text.c_str() + i + 1, "offset:", 7) != 0) continue;
    size_t j = i + 8;
    while (j < text.size() && isspace((unsigned char)text[j])) j++;
    bool neg = false;
    if (j < text.size() && (text[j] == '+' || text[j] == '-')) neg = text[j] == '-', j++;
    size_t digits = j;
    int64_t v = 0;
    while (j < text.size() && isdigit((unsigned char)text[j])) v = v * 10 + (text[j] - '0'), j++;
    if (j == digits) continue;
    while (j < text.size() && isspace((unsigned char)text[j])) j++;
    if (j < text.size() && text[j] == ']') return neg ? -v : v;
  }
  return 0;
}

// ---- tags ----

std::string vorbis(const std::vector<uint8_t>& b) {
  if (b.size() < 8) return "";
  size_t at = 4 + size_t(le32(b.data()));
  if (at + 4 > b.size()) return "";
  uint32_t count = le32(b.data() + at);
  at += 4;
  std::vector<std::string> found;
  for (uint32_t i = 0; i < count && at + 4 <= b.size(); i++) {
    size_t len = le32(b.data() + at);
    at += 4;
    if (at + len > b.size()) break;
    std::string c(reinterpret_cast<const char*>(b.data() + at), len);
    at += len;
    size_t eq = c.find('=');
    std::string key;
    for (char ch : eq == std::string::npos ? c : c.substr(0, eq))
      if (ch != ' ') key += char(toupper((unsigned char)ch));
    if (key == "LYRICS" || key == "UNSYNCEDLYRICS" || key == "SYNCEDLYRICS" || key.rfind("LYRICS-", 0) == 0)
      found.push_back(eq == std::string::npos ? c : c.substr(eq + 1));
  }
  for (auto& f : found)  // a timed version wins over a plain one
    if (hasStamp(f)) return f;
  return found.empty() ? "" : found[0];
}

std::string flac(FILE* f) {
  uint8_t magic[4];
  if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4)) return "";
  bool last = false;
  while (!last) {
    uint8_t h[4];
    if (fread(h, 1, 4, f) != 4) return "";
    uint32_t header = be32(h);
    last = header >> 31;
    uint32_t type = (header >> 24) & 0x7f, length = header & 0xffffff;
    if (type == 4) {
      std::vector<uint8_t> b(length);
      if (fread(b.data(), 1, length, f) != length) return "";
      return vorbis(b);
    }
    if (fseek(f, long(length), SEEK_CUR)) return "";  // STREAMINFO, PICTURE (covers can be large), ...
  }
  return "";
}

// the body [start, end) of the first child atom named `name` between start and end
bool atom(FILE* f, int64_t start, int64_t end, const char* name, int64_t* bs, int64_t* be) {
  int64_t at = start;
  while (at + 8 <= end) {
    if (fseeko(f, at, SEEK_SET)) return false;
    uint8_t h[8];
    if (fread(h, 1, 8, f) != 8) return false;
    int64_t size = be32(h), header = 8;
    if (size == 1) {
      uint8_t l[8];
      if (fread(l, 1, 8, f) != 8) return false;
      size = int64_t(be32(l)) << 32 | be32(l + 4);
      header = 16;
    } else if (size == 0) {
      size = end - at;
    }
    if (size < header) return false;
    if (!memcmp(h + 4, name, 4)) return *bs = at + header, *be = at + size, true;
    at += size;
  }
  return false;
}

std::string mp4(FILE* f) {
  if (fseeko(f, 0, SEEK_END)) return "";
  int64_t len = ftello(f), a, b, c, d, e, g, h, i, j, k, l, m;
  if (!atom(f, 0, len, "moov", &a, &b) || !atom(f, a, b, "udta", &c, &d) || !atom(f, c, d, "meta", &e, &g) ||
      !atom(f, e + 4, g, "ilst", &h, &i) || !atom(f, h, i, "\xa9lyr", &j, &k) || !atom(f, j, k, "data", &l, &m))
    return "";
  int64_t n = std::max<int64_t>(0, m - l - 8);
  std::string out(size_t(n), '\0');
  if (fseeko(f, l + 8, SEEK_SET) || fread(&out[0], 1, size_t(n), f) != size_t(n)) return "";
  return out;
}

std::string uslt(const uint8_t* b, size_t n) {
  if (n < 4) return "";
  int enc = b[0];
  bool wide = enc == 1 || enc == 2;
  size_t at = 4;
  if (wide) {
    while (at + 1 < n && !(b[at] == 0 && b[at + 1] == 0)) at += 2;
    at += 2;
  } else {
    while (at < n && b[at] != 0) at++;
    at++;
  }
  if (at >= n) return "";
  if (enc == 1) return fromUtf16(b + at, n - at, true);
  if (enc == 2) return fromUtf16(b + at, n - at, false);
  if (enc == 3) return std::string(reinterpret_cast<const char*>(b + at), n - at);
  return fromLatin1(b + at, n - at);
}

std::string id3(FILE* f) {
  uint8_t h[10];
  if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3)) return "";
  int major = h[3];
  uint32_t size = syncsafe(h + 6);
  std::vector<uint8_t> tag(size);
  if (fread(tag.data(), 1, size, f) != size) return "";
  size_t at = 0;
  while (at + 10 <= tag.size()) {
    if (tag[at] == 0) break;
    uint32_t len = major >= 4 ? syncsafe(&tag[at + 4]) : be32(&tag[at + 4]);
    if (len == 0 || at + 10 + len > tag.size()) break;
    if (!memcmp(&tag[at], "USLT", 4)) return uslt(&tag[at + 10], len);
    at += 10 + len;
  }
  return "";
}

std::string lowerExt(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string x = dot == std::string::npos ? path : path.substr(dot + 1);
  for (char& c : x) c = char(tolower((unsigned char)c));
  return x;
}

}  // namespace

std::vector<LyricLine> parseLrc(const std::string& text) {
  int64_t offset = lrcOffset(text);
  std::vector<LyricLine> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find_first_of("\r\n", start);
    std::string raw = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    start = nl == std::string::npos ? text.size() + 1 : nl + (text[nl] == '\r' && nl + 1 < text.size() && text[nl + 1] == '\n' ? 2 : 1);
    size_t first = 0;
    while (first < raw.size() && isspace((unsigned char)raw[first])) first++;
    std::vector<int64_t> stamps;
    size_t lastEnd = 0, firstStamp = std::string::npos;
    for (size_t i = 0; i < raw.size();) {
      size_t end;
      int64_t ms;
      if (raw[i] == '[' && stampAt(raw, i, &end, &ms)) {
        if (firstStamp == std::string::npos) firstStamp = i;
        stamps.push_back(ms);
        lastEnd = end;
        i = end;
      } else {
        i++;
      }
    }
    if (stamps.empty() || firstStamp != first) continue;
    std::string words = trim(raw.substr(lastEnd));
    for (int64_t ms : stamps) out.push_back({std::max<int64_t>(0, ms - offset), words});
  }
  std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
  return out;
}

std::string embeddedLyrics(const std::string& path) {
  std::string ext = lowerExt(path), text;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return "";
  if (ext == "flac") text = flac(f);
  else if (ext == "m4a" || ext == "mp4" || ext == "aac") text = mp4(f);
  else if (ext == "mp3") text = id3(f);
  fclose(f);
  return trim(text);
}

Lyrics lyricsForFile(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string base = dot == std::string::npos ? path : path.substr(0, dot);
  std::vector<std::string> found;
  for (const char* ext : {".lrc", ".LRC"}) {
    std::string p = base + ext, t;
    if (isFile(p)) {
      if (readAll(p, t)) found.push_back(t);
      break;
    }
  }
  std::string tag = embeddedLyrics(path);
  if (!tag.empty()) found.push_back(tag);
  Lyrics l;
  for (auto& t : found) {  // timed beats plain, wherever it is
    l.synced = parseLrc(t);
    if (!l.synced.empty()) return l;
  }
  if (!found.empty()) l.plain = found[0];
  return l;
}

}  // namespace rondo
