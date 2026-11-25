// rss_parse.h: a feed's bytes go through a small XML tokenizer (tags, text, CDATA; comments, processing
// instructions and DOCTYPE skipped) that keeps only a token cut by a chunk's end; the elements RSS needs
// are known by their place (channel/…, channel/item/…) and only their text is kept. What a feed reads
// as follows the server's server/src/rssFeed.ts (its plain text, dates, numbers, entities) and its
// feedText() in feedFetch.ts (the encoding).
#include "rss_parse.h"

#include <limits.h>
#include <string.h>

#include <algorithm>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace rondo::direct::rss {
namespace {

using u8 = unsigned char;
constexpr size_t npos = size_t(-1);

constexpr size_t kLongRaw = 64 * 1024;  // content:encoded, summaries and descriptions as read, before plain text
constexpr size_t kTextRaw = 8 * 1024;   // titles, names, links, guids
constexpr size_t kShortRaw = 256;       // dates, durations, numbers, flags, the language
constexpr size_t kMaxTag = 64 * 1024;   // a '<' with no '>' this far on is text
constexpr size_t kDeclLook = 300;       // the first bytes looked through for the XML declaration (as the server)
constexpr int kMaxDepth = 256;          // elements nested deeper are skipped
constexpr size_t kTitleMax = 500, kDescriptionMax = 2000, kNotesMax = 4000;

const char kReplacement[] = "\xEF\xBF\xBD";  // U+FFFD
const char kBullet[] = "\xE2\x80\xA2";       // •
const char kEllipsis[] = "\xE2\x80\xA6";     // …

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
inline bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
inline bool isNameStart(char c) { return isAlpha(c) || c == '_' || c == ':' || u8(c) >= 0x80; }

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
  return true;
}
bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

std::string lowered(std::string s) {
  for (char& c : s) c = lowerAscii(c);
  return s;
}
std::string trimmed(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isSpace(s[a])) a++;
  while (b > a && isSpace(s[b - 1])) b--;
  return s.substr(a, b - a);
}
bool blank(const std::string& s) {
  for (char c : s)
    if (!isSpace(c)) return false;
  return true;
}

const char* findStr(const char* p, size_t n, const char* pat, size_t m) {
  const char* e = p + n;
  while (size_t(e - p) >= m) {
    const char* q = static_cast<const char*>(memchr(p, pat[0], size_t(e - p) - m + 1));
    if (!q) return nullptr;
    if (memcmp(q, pat, m) == 0) return q;
    p = q + 1;
  }
  return nullptr;
}

size_t utf8(uint32_t cp, char* o) {
  if (cp < 0x80) {
    o[0] = char(cp);
    return 1;
  }
  if (cp < 0x800) {
    o[0] = char(0xC0 | (cp >> 6)), o[1] = char(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    o[0] = char(0xE0 | (cp >> 12)), o[1] = char(0x80 | ((cp >> 6) & 0x3F)), o[2] = char(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = char(0xF0 | (cp >> 18)), o[1] = char(0x80 | ((cp >> 12) & 0x3F)), o[2] = char(0x80 | ((cp >> 6) & 0x3F));
  o[3] = char(0x80 | (cp & 0x3F));
  return 4;
}

// The UTF-8 character at p: its length (at least 1) and code point; U+FFFD for bytes that aren't one, each
// broken sequence once, as a browser's decoder reads it (the byte that broke it is read again)
size_t utf8At(const char* p, const char* e, uint32_t& cp) {
  u8 c = u8(*p);
  if (c < 0x80) {
    cp = c;
    return 1;
  }
  int need;
  u8 lo = 0x80, hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) {
    need = 1, cp = c & 0x1F;
  } else if (c >= 0xE0 && c <= 0xEF) {
    need = 2, cp = c & 0x0F;
    if (c == 0xE0) lo = 0xA0;
    else if (c == 0xED) hi = 0x9F;
  } else if (c >= 0xF0 && c <= 0xF4) {
    need = 3, cp = c & 0x07;
    if (c == 0xF0) lo = 0x90;
    else if (c == 0xF4) hi = 0x8F;
  } else {
    cp = 0xFFFD;
    return 1;
  }
  size_t n = 1;
  for (int k = 0; k < need; k++, n++) {
    if (p + n >= e || u8(p[n]) < lo || u8(p[n]) > hi) {
      cp = 0xFFFD;
      return n;
    }
    lo = 0x80, hi = 0xBF;
    cp = (cp << 6) | (u8(p[n]) & 0x3F);
  }
  return n;
}

// s as valid UTF-8 without control characters (\n and \t stay)
std::string clean(const char* p, size_t n) {
  std::string out;
  out.reserve(n);
  const char* e = p + n;
  while (p < e) {
    const char* q = p;
    while (q < e && u8(*q) >= 0x20 && u8(*q) < 0x7F) q++;
    out.append(p, size_t(q - p));
    if ((p = q) >= e) break;
    if (u8(*p) < 0x80) {
      if (*p == '\n' || *p == '\t') out += *p;
      p++;
      continue;
    }
    uint32_t cp;
    size_t len = utf8At(p, e, cp);
    if (cp == 0xFFFD) out.append(kReplacement, 3);
    else if (cp >= 0xA0) out.append(p, len);  // (U+0080–U+009F are control characters)
    p += len;
  }
  return out;
}
// a field's text: clean and trimmed
std::string field(const std::string& raw) { return trimmed(clean(raw.data(), raw.size())); }

// a character cut at the end of s (by a size cap) is taken off
void dropCut(std::string& s) {
  size_t n = s.size();
  for (size_t k = 1; k <= 3 && k <= n; k++) {
    u8 c = u8(s[n - k]);
    if ((c & 0xC0) == 0x80) continue;
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (need > k) s.resize(n - k);
    return;
  }
}

// windows-1252's 0x80–0x9F (the five it leaves undefined stay C1 control characters, dropped later)
const uint16_t kCp1252[32] = {0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                              0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

void latinToUtf8(const char* d, size_t n, std::string& out) {
  out.reserve(out.size() + n + n / 8);
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && u8(d[j]) < 0x80) j++;
    out.append(d + i, j - i);
    if (j == n) break;
    u8 c = u8(d[j]);
    char b[4];
    out.append(b, utf8(c < 0xA0 ? kCp1252[c - 0x80] : c, b));
    i = j + 1;
  }
}

// ---- entities ----

// HTML's named entities as the server knows them (server/src/rssFeed.ts HTML_ENTITIES): XML's five, the
// Latin-1 set (U+00A0–U+00FF, in order) and the common others
const char* const kLatin1Names[96] = {
    "nbsp",   "iexcl",  "cent",   "pound",  "curren", "yen",    "brvbar", "sect",   "uml",    "copy",   "ordf",   "laquo",
    "not",    "shy",    "reg",    "macr",   "deg",    "plusmn", "sup2",   "sup3",   "acute",  "micro",  "para",   "middot",
    "cedil",  "sup1",   "ordm",   "raquo",  "frac14", "frac12", "frac34", "iquest", "Agrave", "Aacute", "Acirc",  "Atilde",
    "Auml",   "Aring",  "AElig",  "Ccedil", "Egrave", "Eacute", "Ecirc",  "Euml",   "Igrave", "Iacute", "Icirc",  "Iuml",
    "ETH",    "Ntilde", "Ograve", "Oacute", "Ocirc",  "Otilde", "Ouml",   "times",  "Oslash", "Ugrave", "Uacute", "Ucirc",
    "Uuml",   "Yacute", "THORN",  "szlig",  "agrave", "aacute", "acirc",  "atilde", "auml",   "aring",  "aelig",  "ccedil",
    "egrave", "eacute", "ecirc",  "euml",   "igrave", "iacute", "icirc",  "iuml",   "eth",    "ntilde", "ograve", "oacute",
    "ocirc",  "otilde", "ouml",   "divide", "oslash", "ugrave", "uacute", "ucirc",  "uuml",   "yacute", "thorn",  "yuml"};
struct Named {
  const char* name;
  uint32_t cp;
};
const Named kOtherNames[] = {
    {"amp", 38},      {"lt", 60},       {"gt", 62},       {"quot", 34},     {"apos", 39},     {"OElig", 338},   {"oelig", 339},
    {"Scaron", 352},  {"scaron", 353},  {"Yuml", 376},    {"fnof", 402},    {"circ", 710},    {"tilde", 732},   {"ensp", 8194},
    {"emsp", 8195},   {"thinsp", 8201}, {"zwnj", 8204},   {"zwj", 8205},    {"lrm", 8206},    {"rlm", 8207},    {"ndash", 8211},
    {"mdash", 8212},  {"lsquo", 8216},  {"rsquo", 8217},  {"sbquo", 8218},  {"ldquo", 8220},  {"rdquo", 8221},  {"bdquo", 8222},
    {"dagger", 8224}, {"Dagger", 8225}, {"bull", 8226},   {"hellip", 8230}, {"permil", 8240}, {"prime", 8242},  {"Prime", 8243},
    {"lsaquo", 8249}, {"rsaquo", 8250}, {"oline", 8254},  {"frasl", 8260},  {"euro", 8364},   {"trade", 8482},  {"larr", 8592},
    {"uarr", 8593},   {"rarr", 8594},   {"darr", 8595},   {"harr", 8596},   {"minus", 8722},  {"spades", 9824}, {"clubs", 9827},
    {"hearts", 9829}, {"diams", 9830},
};

using EntityTable = std::vector<std::pair<std::string_view, uint32_t>>;
const EntityTable& htmlEntities() {
  static const EntityTable table = [] {
    EntityTable t;
    for (int i = 0; i < 96; i++) t.push_back({kLatin1Names[i], uint32_t(160 + i)});
    for (const Named& n : kOtherNames) t.push_back({n.name, n.cp});
    std::sort(t.begin(), t.end());
    return t;
  }();
  return table;
}

constexpr size_t kEntityMax = 64;  // a longer "&…;" is text
inline bool entityChar(char c) { return isAlpha(c) || isDigit(c) || c == '#'; }

// The entity at s ('&', n bytes there): its length with the ';' and its code point; 0 when it isn't one
// (the '&' is then text). Numeric ones (U+FFFD when they name no character), and by name XML's five or
// (html) the table's.
size_t entityAt(const char* s, size_t n, bool html, uint32_t& cp) {
  n = std::min(n, kEntityMax);
  if (n < 3) return 0;
  size_t i;
  if (s[1] == '#') {
    bool hex = s[2] == 'x' || s[2] == 'X';
    i = hex ? 3 : 2;
    size_t start = i;
    uint32_t v = 0;
    for (; i < n; i++) {
      char c = lowerAscii(s[i]);
      uint32_t d;
      if (isDigit(c)) d = uint32_t(c - '0');
      else if (hex && c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
      else break;
      if (v <= 0x10FFFF) v = v * (hex ? 16 : 10) + d;
    }
    if (i == start || i >= n || s[i] != ';') return 0;
    cp = v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF) ? 0xFFFD : v;
    return i + 1;
  }
  if (!isAlpha(s[1])) return 0;
  for (i = 2; i < n && (isAlpha(s[i]) || isDigit(s[i])); i++) {
  }
  if (i >= n || s[i] != ';') return 0;
  std::string_view name(s + 1, i - 1);
  if (!html) {
    if (name == "amp") cp = '&';
    else if (name == "lt") cp = '<';
    else if (name == "gt") cp = '>';
    else if (name == "quot") cp = '"';
    else if (name == "apos") cp = '\'';
    else return 0;
    return i + 1;
  }
  const EntityTable& t = htmlEntities();
  auto it = std::lower_bound(t.begin(), t.end(), name, [](const std::pair<std::string_view, uint32_t>& a, std::string_view b) { return a.first < b; });
  if (it == t.end() || it->first != name) return 0;
  cp = it->second;
  return i + 1;
}

// p[0..n) with XML's entities decoded, each piece given to put(const char*, size_t)
template <class Put>
void xmlDecode(const char* p, size_t n, Put&& put) {
  const char* e = p + n;
  while (p < e) {
    const char* amp = static_cast<const char*>(memchr(p, '&', size_t(e - p)));
    if (!amp) {
      put(p, size_t(e - p));
      return;
    }
    if (amp > p) put(p, size_t(amp - p));
    uint32_t cp;
    size_t len = entityAt(amp, size_t(e - amp), false, cp);
    if (len) {
      char u[4];
      put(u, utf8(cp, u));
      p = amp + len;
    } else {
      put("&", 1);
      p = amp + 1;
    }
  }
}

// A tag's attributes (the text after its name): f(name, value, length) for each, the value as written
template <class F>
void eachAttr(const char* s, size_t n, F&& f) {
  size_t i = 0;
  while (i < n) {
    size_t from = i;
    while (i < n && (isSpace(s[i]) || s[i] == '/')) i++;
    size_t a = i;
    while (i < n && !isSpace(s[i]) && s[i] != '=' && s[i] != '/') i++;
    std::string_view name(s + a, i - a);
    while (i < n && isSpace(s[i])) i++;
    const char* v = s + i;
    size_t vn = 0;
    if (i < n && s[i] == '=') {
      i++;
      while (i < n && isSpace(s[i])) i++;
      if (i < n && (s[i] == '"' || s[i] == '\'')) {
        char q = s[i++];
        size_t b = i;
        while (i < n && s[i] != q) i++;
        v = s + b, vn = i - b;
        if (i < n) i++;
      } else {
        size_t b = i;
        while (i < n && !isSpace(s[i])) i++;
        v = s + b, vn = i - b;
      }
    }
    if (!name.empty()) f(name, v, vn);
    if (i == from) i++;
  }
}

// the attribute's value, XML-decoded, clean and trimmed ("" when the tag has none)
std::string attr(const char* s, size_t n, std::string_view want) {
  std::string raw;
  bool found = false;
  eachAttr(s, n, [&](std::string_view name, const char* v, size_t vn) {
    if (found || name != want) return;
    found = true;
    xmlDecode(v, vn, [&](const char* p, size_t k) { raw.append(p, k); });
  });
  return field(raw);
}

// a whole number as the server reads one ("12345678", "3.0" rounded down; anything else 0, as is one past max)
int64_t wholeNumber(const std::string& raw, int64_t max) {
  std::string s = trimmed(raw);
  size_t i = 0;
  int64_t v = 0;
  for (; i < s.size() && isDigit(s[i]); i++) {
    v = v * 10 + (s[i] - '0');
    if (v > max) return 0;
  }
  if (i == 0) return 0;
  if (i < s.size() && s[i] == '.') {
    size_t f = ++i;
    while (i < s.size() && isDigit(s[i])) i++;
    if (i == f) return 0;
  }
  return i == s.size() ? v : 0;
}

// ---- plain text ----

// Plain text as it's written out: spaces and line breaks wait until the next visible text (so runs
// collapse, the ends of lines and of the whole lose them), and writing stops past the limit.
struct TextOut {
  std::string s;
  size_t max;
  bool oneLine;
  int lines = 0;
  bool space = false, full = false;
  void gap() { space = true; }
  void line() {
    if (oneLine) space = true;
    else lines++;
  }
  void put(const char* p, size_t n) {
    if (full || !n) return;
    if (!s.empty()) {
      if (lines) s.append("\n\n", lines > 1 ? 2 : 1);
      else if (space) s += ' ';
    }
    lines = 0, space = false;
    if (s.size() > max) {
      full = true;
      return;
    }
    size_t room = max + 1 - s.size();
    if (n >= room) {
      s.append(p, room);
      full = true;
    } else {
      s.append(p, n);
    }
  }
  void putCodePoint(uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\f' || cp == '\v' || cp == 0xA0) gap();
    else if (cp == '\n') line();
    else if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return;
    else {
      char u[4];
      put(u, utf8(cp, u));
    }
  }
  std::string finish() {
    if (full) {
      size_t cut = max >= 3 ? max - 3 : max;
      while (cut > 0 && (u8(s[cut]) & 0xC0) == 0x80) cut--;
      s.resize(cut);
      while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
      if (max >= 3) s += kEllipsis;
    }
    return std::move(s);
  }
};

bool blockEnd(std::string_view n) {
  return n == "p" || n == "div" || n == "li" || n == "tr" || n == "ul" || n == "ol" || n == "pre" || n == "blockquote" ||
         (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6');
}
inline bool wordChar(char c) { return isAlpha(c) || isDigit(c) || c == '_'; }

// "</name" then spaces and '>' from p on, in any case: its start and the end ('>' + 1)
const char* findClose(const char* p, const char* e, std::string_view name, const char*& after) {
  while (p < e) {
    const char* q = static_cast<const char*>(memchr(p, '<', size_t(e - p)));
    if (!q) return nullptr;
    if (size_t(e - q) >= name.size() + 2 && q[1] == '/' && ieq(std::string_view(q + 2, name.size()), name)) {
      const char* r = q + 2 + name.size();
      while (r < e && isSpace(*r)) r++;
      if (r < e && *r == '>') {
        after = r + 1;
        return q;
      }
    }
    p = q + 1;
  }
  return nullptr;
}

// The markup at p ('<') read as the server reads it (rssFeed.ts plainText: a comment to "-->", script
// and style with what's in them, then a tag: '<', a name, its text to the first '>'): a line break, a
// bullet or nothing written, and where reading goes on. Anything else is a '<' in the text.
const char* htmlTag(const char* p, const char* e, TextOut& w) {
  if (e - p >= 4 && memcmp(p, "<!--", 4) == 0) {
    const char* f = findStr(p + 4, size_t(e - p - 4), "-->", 3);
    return f ? f + 3 : e;
  }
  const char* q = p + 1;
  bool closing = q < e && *q == '/';
  if (closing) q++;
  if (q >= e || !isAlpha(*q)) {
    w.put("<", 1);
    return p + 1;
  }
  auto boundary = [&](const char* x) { return wordChar(x[-1]) != (x < e && wordChar(*x)); };
  for (const char* tag : {"script", "style"}) {
    size_t len = strlen(tag);
    if (closing || size_t(e - q) < len || !ieq(std::string_view(q, len), tag) || !boundary(q + len)) continue;
    const char* gt = static_cast<const char*>(memchr(q + len, '>', size_t(e - q - len)));
    if (!gt) break;
    const char* after = e;
    findClose(gt + 1, e, tag, after);
    return after;
  }
  // the name: letters, digits, ':' and '-', where a word ends (as a regex's \b sees it)
  const char* r = q + 1;
  while (r < e && (isAlpha(*r) || isDigit(*r) || *r == ':' || *r == '-')) r++;
  while (r > q + 1 && !boundary(r)) r--;
  const char* gt = boundary(r) ? static_cast<const char*>(memchr(r, '>', size_t(e - r))) : nullptr;
  if (!gt) {
    w.put("<", 1);
    return p + 1;
  }
  char name[12];
  size_t nl = size_t(r - q);
  if (nl > sizeof name) nl = 0;  // none of ours
  for (size_t k = 0; k < nl; k++) name[k] = lowerAscii(q[k]);
  std::string_view n(name, nl);
  if (closing) {
    if (blockEnd(n)) w.line();
  } else if (n == "br") {
    w.line();
  } else if (n == "li") {
    w.put(kBullet, 3);
    w.gap();
  }
  return gt + 1;
}

// ---- dates, as the server reads them (rssFeed.ts parseDate: its RFC 822 pattern, then its ISO 8601 one) ----

int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

inline bool dateSpace(char c) { return isSpace(c) || c == '\f' || c == '\v'; }

// the run of digits at p (up to e): how many, and the value of the first `most` (-1 when there are more)
int digitsAt(const char* p, const char* e, int most, int& v) {
  int n = 0;
  v = 0;
  for (; p + n < e && isDigit(p[n]); n++)
    if (n < most) v = v * 10 + (p[n] - '0');
  return n > most ? -1 : n;
}

// "+hh", "+hhmm", "+hh:mm" or a name the list has: seconds east of UTC; anything else is UTC
int64_t zoneSeconds(std::string_view z) {
  if (z.size() >= 3 && (z[0] == '+' || z[0] == '-') && isDigit(z[1]) && isDigit(z[2])) {
    size_t i = 3;
    if (i < z.size() && z[i] == ':') i++;
    int mm = 0;
    if (z.size() - i == 2 && isDigit(z[i]) && isDigit(z[i + 1])) mm = (z[i] - '0') * 10 + (z[i + 1] - '0'), i += 2;
    if (i != z.size()) return 0;
    return (z[0] == '-' ? -1 : 1) * (((z[1] - '0') * 10 + (z[2] - '0')) * 3600 + mm * 60);
  }
  static const struct {
    const char* name;
    int hours;
  } zones[] = {{"gmt", 0},  {"ut", 0},  {"utc", 0}, {"z", 0},   {"est", -5}, {"edt", -4},
               {"cst", -6}, {"cdt", -5}, {"mst", -7}, {"mdt", -6}, {"pst", -8}, {"pdt", -7}};
  for (const auto& zone : zones)
    if (ieq(z, zone.name)) return zone.hours * 3600;
  return 0;
}

// unix seconds (month 0–11), 0 when out of range or not after 1970 (as Date.UTC: a year 0–99 is 19xx)
int64_t utcSeconds(int64_t y, int mo, int d, int h, int mi, int se, int64_t zone) {
  if (mo < 0 || mo > 11 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
  if (y >= 0 && y <= 99) y += 1900;
  int64_t t = daysFromCivil(y, mo + 1, d) * 86400 + h * 3600 + mi * 60 + std::min(se, 59) - zone;
  return t > 0 ? t : 0;
}

// "Tue, 05 Nov 2024 10:30:00 +0000": a weekday (letters, '.', ',') or not, day 1–2 digits, month by its
// first three letters, year 2 or 4 digits, H:MM, :SS or not, then a zone or nothing; -1 when it isn't one
int64_t rfcDate(const char* p, const char* e) {
  static const char* const months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
  auto spaces = [&](bool need) {
    if (need && (p >= e || !dateSpace(*p))) return false;
    while (p < e && dateSpace(*p)) p++;
    return true;
  };
  if (p < e && isAlpha(*p)) {
    while (p < e && isAlpha(*p)) p++;
    if (p < e && *p == '.') p++;
    if (p < e && *p == ',') p++;
    spaces(false);
  }
  int d, y, h, mi, se = 0;
  int n = digitsAt(p, e, 2, d);
  if (n < 1) return -1;
  p += n;
  if (!spaces(true)) return -1;
  const char* m = p;
  while (p < e && isAlpha(*p)) p++;
  if (p - m < 3) return -1;
  int mo = -1;
  for (int i = 0; i < 12; i++)
    if (ieq(std::string_view(m, 3), months[i])) mo = i;
  if (p < e && *p == '.') p++;
  if (!spaces(true)) return -1;
  n = digitsAt(p, e, 4, y);
  if (n != 2 && n != 4) return -1;
  if (n == 2) y += y < 70 ? 2000 : 1900;
  p += n;
  if (!spaces(true)) return -1;
  n = digitsAt(p, e, 2, h);
  if (n < 1 || p + n >= e || p[n] != ':') return -1;
  p += n + 1;
  if (digitsAt(p, e, 2, mi) != 2) return -1;
  p += 2;
  if (p < e && *p == ':' && digitsAt(p + 1, e, 2, se) == 2) p += 3;
  std::string_view zone;
  if (p < e) {
    if (!spaces(true)) return -1;
    const char* z = p;
    while (p < e && !dateSpace(*p)) p++;
    zone = std::string_view(z, size_t(p - z));  // anything after it doesn't count
  }
  if (mo < 0) return 0;
  return utcSeconds(y, mo, d, h, mi, se, zoneSeconds(zone));
}

// "2024-05-01", "2024-05-01T10:00:00Z", "2024-05-01T10:00:00.5+02:00", "2024-05-01 10:00"
int64_t isoDate(const char* p, const char* e) {
  int y, mo, d, h = 0, mi = 0, se = 0;
  if (digitsAt(p, e, 4, y) != 4 || p + 4 >= e || p[4] != '-' || digitsAt(p + 5, e, 2, mo) != 2 || p + 7 >= e || p[7] != '-' ||
      digitsAt(p + 8, e, 2, d) != 2)
    return 0;
  p += 10;
  std::string_view zone;
  if (p < e) {
    if (*p != 'T' && *p != 't' && *p != ' ') return 0;
    p++;
    if (digitsAt(p, e, 2, h) != 2 || p + 2 >= e || p[2] != ':' || digitsAt(p + 3, e, 2, mi) != 2) return 0;
    p += 5;
    if (p < e && *p == ':') {
      if (digitsAt(p + 1, e, 2, se) != 2) return 0;
      p += 3;
      if (p < e && (*p == '.' || *p == ',')) {
        const char* f = ++p;
        while (p < e && isDigit(*p)) p++;
        if (p == f) return 0;
      }
    }
    while (p < e && dateSpace(*p)) p++;
    const char* z = p;
    if (p < e && (*p == 'Z' || *p == 'z')) {
      p++;
    } else if (p < e && (*p == '+' || *p == '-')) {  // "+hh", "+hhmm", "+hh:mm"
      if (e - p < 3 || !isDigit(p[1]) || !isDigit(p[2])) return 0;
      p += 3;
      const char* c = p < e && *p == ':' ? p + 1 : p;
      int v;
      if (c < e && digitsAt(c, e, 2, v) == 2) p = c + 2;
    }
    if (p != e) return 0;
    zone = std::string_view(z, size_t(p - z));
  }
  return utcSeconds(y, mo - 1, d, h, mi, se, zoneSeconds(zone));
}

// ---- the elements read ----

enum Kind : uint8_t {
  kOther,
  kRss,
  kChannel,
  kItem,
  kChImage,
  kChOwner,
  kChTitle,
  kChAuthor,
  kChOwnerName,
  kChEditor,
  kChSummary,
  kChDescription,
  kChItunesImage,
  kChImageUrl,
  kChLink,
  kChLanguage,
  kChType,
  kChExplicit,
  kChNewFeedUrl,
  kChComplete,
  kItGuid,
  kItTitle,
  kItItunesTitle,
  kItDate,
  kItDuration,
  kItEnclosure,
  kItImage,
  kItChapters,
  kItSeason,
  kItEpisode,
  kItEpisodeType,
  kItEncoded,
  kItSummary,
  kItDescription,
};

struct ChannelRaw {
  std::string title, author, ownerName, editor, summary, description, itunesImage, imageUrl, link, language, type,
      explicitness, newFeedUrl, complete;
};
struct ItemRaw {
  std::string guid, title, itunesTitle, date, duration, url, type, image, chapters, season, episode, episodeType, encoded,
      summary, description;
  int64_t size = 0;
  bool av = false;
  void clear() {
    for (std::string* s : {&guid, &title, &itunesTitle, &date, &duration, &url, &type, &image, &chapters, &season, &episode,
                           &episodeType, &encoded, &summary, &description})
      s->clear();
    size = 0, av = false;
  }
};

// the first of these whose plain text isn't empty
std::string firstPlain(std::initializer_list<const std::string*> raws, size_t max, bool oneLine) {
  for (const std::string* r : raws) {
    if (blank(*r)) continue;
    std::string t = plainText(*r, max, oneLine);
    if (!t.empty()) return t;
  }
  return "";
}

}  // namespace

struct Parser::Impl {
  explicit Impl(std::function<void(Episode&&)> f) : onEpisode(std::move(f)) {}

  std::function<void(Episode&&)> onEpisode;
  int delivered = 0;

  // input
  bool started = false, notFeed = false, done = false, finished = false, latin = false;
  size_t skip = 0;              // a BOM at the start
  std::string head, buf, conv;  // the first bytes (until the encoding is known); a token cut by a chunk's end; a chunk as UTF-8

  // the tokenizer, between chunks
  enum class St : uint8_t { Text, Comment, CData, Pi, Decl } st = St::Text;
  int declDepth = 0;  // [ ] of a DOCTYPE's internal subset
  std::string declHead;
  size_t tagScan = 0;  // a tag cut by a chunk's end: how far it was looked through
  char tagQuote = 0;
  bool tagAfterEq = false;

  // the open elements
  struct El {
    std::string name;
    Kind kind = kOther;
  };
  std::vector<El> stack;
  int depth = 0, overflow = 0, itemLevel = -1;
  bool rootSeen = false, rss = false, channelSeen = false, ending = false;

  // the text being kept (target null: an element whose text isn't needed after all)
  bool capturing = false, full = false;
  std::string* target = nullptr;
  size_t cap = 0;

  ChannelRaw ch;
  ItemRaw it;

  // The first bytes, as the server's feedText() reads them: a UTF-16 BOM (not read here: not a feed), a
  // UTF-8 one, else the encoding an XML declaration in the first 300 bytes names (windows-1252 for any of
  // its names, as a browser's decoder takes them; any other is read as UTF-8). JSON isn't a feed either.
  // false: more bytes are needed first (300, or a '>' that ends the declaration, unless the first ones
  // can't start one). all: there are no more.
  bool start(bool all) {
    const char* h = head.data();
    size_t n = head.size();
    if (!all && n < 3) return false;
    if (n >= 2 && ((u8(h[0]) == 0xFF && u8(h[1]) == 0xFE) || (u8(h[0]) == 0xFE && u8(h[1]) == 0xFF))) {
      notFeed = true;
      return true;
    }
    bool bom = n >= 3 && memcmp(h, "\xEF\xBB\xBF", 3) == 0;
    if (!bom && !all && n < kDeclLook && !memchr(h, '>', n)) {
      size_t k = 0;
      while (k < n && (isSpace(h[k]) || h[k] == '\f' || h[k] == '\v' || u8(h[k]) == 0xA0)) k++;
      size_t m = std::min<size_t>(n - k, 5);
      if (m == 0 || ieq(std::string_view(h + k, m), std::string_view("<?xml", m))) return false;
    }
    size_t look = std::min(n, kDeclLook), k = bom ? 3 : 0;
    while (k < look && isSpace(h[k])) k++;
    if (k < look && (h[k] == '{' || h[k] == '[')) {
      notFeed = true;
      return true;
    }
    if (!bom) latin = declaresLatin(h, look);
    started = true;
    skip = bom ? 3 : 0;
    return true;
  }
  // /^\s*<\?xml\b[^>]*?\bencoding\s*=\s*["']([\w.:-]+)["']/i naming windows-1252
  static bool declaresLatin(const char* h, size_t n) {
    auto space = [](char c) { return isSpace(c) || c == '\f' || c == '\v' || u8(c) == 0xA0; };
    size_t i = 0;
    while (i < n && space(h[i])) i++;
    if (n - i < 5 || !ieq(std::string_view(h + i, 5), "<?xml")) return false;
    i += 5;
    if (i < n && wordChar(h[i])) return false;
    for (size_t j = i; j < n && h[j] != '>'; j++) {
      if (n - j < 8 || !ieq(std::string_view(h + j, 8), "encoding") || wordChar(h[j - 1])) continue;
      size_t t = j + 8;
      while (t < n && space(h[t])) t++;
      if (t >= n || h[t] != '=') continue;
      for (t++; t < n && space(h[t]); t++) {
      }
      if (t >= n || (h[t] != '"' && h[t] != '\'')) continue;
      size_t a = ++t;
      while (t < n && (wordChar(h[t]) || h[t] == '.' || h[t] == ':' || h[t] == '-')) t++;
      if (t == a || t >= n || (h[t] != '"' && h[t] != '\'')) continue;
      std::string_view label(h + a, t - a);
      for (const char* name : {"ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1", "ibm819", "iso-8859-1", "iso-ir-100",
                               "iso8859-1", "iso88591", "iso_8859-1", "iso_8859-1:1987", "l1", "latin1", "us-ascii", "windows-1252",
                               "x-cp1252"})
        if (ieq(label, name)) return true;
      return false;
    }
    return false;
  }

  // bytes after the first: converted, then read with whatever a chunk's end cut before them
  void push(const char* d, size_t n, bool last) {
    if (latin && n) {
      conv.clear();
      latinToUtf8(d, n, conv);
      d = conv.data(), n = conv.size();
    }
    if (buf.empty()) {
      size_t used = n ? run(d, n, last) : 0;
      if (used < n && !last && !notFeed && !done) buf.assign(d + used, n - used);
    } else {
      if (n) buf.append(d, n);
      size_t used = run(buf.data(), buf.size(), last);
      if (last || notFeed || done) buf.clear();
      else buf.erase(0, used);
      if (buf.capacity() > (1u << 20) && buf.size() < (1u << 16)) buf.shrink_to_fit();
    }
    if (conv.capacity() > (1u << 20)) std::string().swap(conv);
  }

  // Reads d[0..n): what it used (the rest, a token cut by the end, is read again with the next bytes)
  size_t run(const char* d, size_t n, bool last) {
    size_t i = 0;
    while (i < n && !notFeed && !done) {
      switch (st) {
        case St::Text: {
          const char* lt = static_cast<const char*>(memchr(d + i, '<', n - i));
          size_t end = lt ? size_t(lt - d) : n;
          if (end > i) {
            i += text(d + i, end - i, !lt && !last);
            if (i < end || notFeed) return i;  // an entity cut by the end: read with the next bytes
          }
          if (!lt) return n;
          if (i + 1 >= n) {
            if (!last) return i;
            literalLt();
            return n;
          }
          char c = d[i + 1];
          if (c == '!') {
            size_t avail = n - i;
            bool comment = avail >= 4 && memcmp(d + i, "<!--", 4) == 0;
            bool cdata = avail >= 9 && memcmp(d + i, "<![CDATA[", 9) == 0;
            if (!comment && !cdata && !last &&
                (memcmp(d + i, "<!--", std::min<size_t>(avail, 4)) == 0 || memcmp(d + i, "<![CDATA[", std::min<size_t>(avail, 9)) == 0))
              return i;  // which one isn't known yet
            if (comment) {
              st = St::Comment, i += 4;
            } else if (cdata) {
              st = St::CData, i += 9;
            } else {
              st = St::Decl, declDepth = 0, i += 2;
              declHead.clear();
            }
          } else if (c == '?') {
            st = St::Pi, i += 2;
          } else if (c == '/' || isNameStart(c)) {
            size_t limit = std::min(n, i + kMaxTag);
            size_t j = scanTag(d, i, limit);
            if (j == npos) {
              if (limit == n && !last) return i;  // the rest is in the next bytes
              tagScan = 0, tagQuote = 0, tagAfterEq = false;
              literalLt();  // too long, or cut by the end of the feed
              i++;
              break;
            }
            tagScan = 0, tagQuote = 0, tagAfterEq = false;
            tag(d + i + 1, j - i - 1, d[j] == '>');
            i = d[j] == '>' ? j + 1 : j;
          } else {
            literalLt();
            i++;
          }
          break;
        }
        case St::Comment: {
          const char* f = findStr(d + i, n - i, "-->", 3);
          if (!f) return last ? n : std::max(i, n > 2 ? n - 2 : 0);
          i = size_t(f - d) + 3, st = St::Text;
          break;
        }
        case St::CData: {
          const char* f = findStr(d + i, n - i, "]]>", 3);
          if (!f) {
            size_t keep = last ? n : std::max(i, n > 2 ? n - 2 : 0);
            cdata(d + i, keep - i);
            return keep;
          }
          cdata(d + i, size_t(f - d) - i);
          i = size_t(f - d) + 3, st = St::Text;
          break;
        }
        case St::Pi: {
          const char* f = findStr(d + i, n - i, "?>", 2);
          if (!f) return last ? n : std::max(i, n - 1);
          i = size_t(f - d) + 2, st = St::Text;
          break;
        }
        case St::Decl: {
          size_t j = i;
          for (; j < n; j++) {
            char c = d[j];
            if (declHead.size() < 32) declHead += c;
            if (c == '[') declDepth++;
            else if (c == ']' && declDepth > 0) declDepth--;
            else if (c == '>' && declDepth == 0) break;
          }
          if (j == n) return n;
          i = j + 1, st = St::Text;
          endDecl();
          break;
        }
      }
    }
    return notFeed || done ? n : i;
  }

  // a tag from d[i] ('<'): where it ends (its '>', or a '<' that cuts it short), npos when not before limit
  size_t scanTag(const char* d, size_t i, size_t limit) {
    size_t j = i + std::max<size_t>(tagScan, 1);
    for (; j < limit; j++) {
      char c = d[j];
      if (c == '<') return j;
      if (tagQuote) {
        if (c == tagQuote) tagQuote = 0;
        continue;
      }
      if (c == '>') return j;
      if ((c == '"' || c == '\'') && tagAfterEq) {
        tagQuote = c, tagAfterEq = false;
        continue;
      }
      if (c == '=') tagAfterEq = true;
      else if (!isSpace(c)) tagAfterEq = false;
    }
    tagScan = j - i;
    return npos;
  }

  void endDecl() {
    if (rootSeen) return;
    std::string_view s(declHead);
    if (s.size() < 7 || !ieq(s.substr(0, 7), "DOCTYPE")) return;
    size_t i = 7;
    while (i < s.size() && isSpace(s[i])) i++;
    size_t a = i;
    while (i < s.size() && isAlpha(s[i])) i++;
    if (ieq(s.substr(a, i - a), "html")) notFeed = true;
  }

  // ---- text ----

  void put(const char* p, size_t n) {
    if (!target || full || !n) return;
    size_t room = cap - target->size();
    if (n <= room) {
      target->append(p, n);
      return;
    }
    target->append(p, room);
    dropCut(*target);
    full = true;
  }

  // text between tags (before the first element it's ignored, as the server does): what it used (an
  // entity cut by the end of the bytes so far waits for the rest)
  size_t text(const char* p, size_t n, bool mayBeCut) {
    if (!target || full) return n;
    size_t keep = n;
    if (mayBeCut)
      for (size_t k = n, lim = n > kEntityMax ? n - kEntityMax : 0; k > lim; k--) {
        if (p[k - 1] == '&') {
          keep = k - 1;
          break;
        }
        if (!entityChar(p[k - 1])) break;
      }
    xmlDecode(p, keep, [this](const char* s, size_t k) { put(s, k); });
    return keep;
  }
  void literalLt() { put("<", 1); }
  void cdata(const char* p, size_t n) { put(p, n); }

  // ---- elements ----

  // a tag's text between '<' and '>' (closed: false when a '<' cut it short)
  void tag(const char* s, size_t n, bool closed) {
    if (n && s[0] == '/') {
      size_t k = 1;
      while (k < n && !isSpace(s[k])) k++;
      endTag(std::string_view(s + 1, k - 1), s, n, closed);
      return;
    }
    size_t k = 0;
    while (k < n && !isSpace(s[k]) && s[k] != '/') k++;
    bool selfClosing = closed && n && s[n - 1] == '/';
    startTag(std::string_view(s, k), s + k, n - k - (selfClosing && n > k ? 1 : 0), selfClosing, s, n, closed);
  }

  Kind classify(Kind parent, std::string_view n) const {
    switch (parent) {
      case kRss:
        return n == "channel" && !channelSeen ? kChannel : kOther;
      case kChannel:
        if (n == "item") return kItem;
        if (n == "title") return kChTitle;
        if (n == "description") return kChDescription;
        if (n == "link") return kChLink;
        if (n == "language") return kChLanguage;
        if (n == "image") return kChImage;
        if (n == "managingEditor") return kChEditor;
        if (startsWith(n, "itunes:")) {
          std::string_view t = n.substr(7);
          if (t == "author") return kChAuthor;
          if (t == "owner") return kChOwner;
          if (t == "summary") return kChSummary;
          if (t == "image") return kChItunesImage;
          if (t == "type") return kChType;
          if (t == "explicit") return kChExplicit;
          if (t == "new-feed-url") return kChNewFeedUrl;
          if (t == "complete") return kChComplete;
        }
        return kOther;
      case kChImage:
        return n == "url" ? kChImageUrl : kOther;
      case kChOwner:
        return n == "itunes:name" ? kChOwnerName : kOther;
      case kItem:
        if (n == "title") return kItTitle;
        if (n == "guid") return kItGuid;
        if (n == "pubDate") return kItDate;
        if (n == "enclosure") return kItEnclosure;
        if (n == "description") return kItDescription;
        if (n == "content:encoded") return kItEncoded;
        if (n == "podcast:chapters") return kItChapters;
        if (startsWith(n, "itunes:")) {
          std::string_view t = n.substr(7);
          if (t == "title") return kItItunesTitle;
          if (t == "duration") return kItDuration;
          if (t == "image") return kItImage;
          if (t == "season") return kItSeason;
          if (t == "episode") return kItEpisode;
          if (t == "episodeType") return kItEpisodeType;
          if (t == "summary") return kItSummary;
        }
        return kOther;
      default:
        return kOther;
    }
  }

  void beginCapture(Kind k) {
    std::string* t = nullptr;
    size_t c = kShortRaw;
    switch (k) {
      case kChTitle: t = &ch.title, c = kTextRaw; break;
      case kChAuthor: t = &ch.author, c = kTextRaw; break;
      case kChOwnerName: t = &ch.ownerName, c = kTextRaw; break;
      case kChEditor: t = &ch.editor, c = kTextRaw; break;
      case kChSummary: t = &ch.summary, c = kLongRaw; break;
      case kChDescription: t = &ch.description, c = kLongRaw; break;
      case kChImageUrl: t = &ch.imageUrl, c = kTextRaw; break;
      case kChLink: t = &ch.link, c = kTextRaw; break;
      case kChLanguage: t = &ch.language; break;
      case kChType: t = &ch.type; break;
      case kChExplicit: t = &ch.explicitness; break;
      case kChNewFeedUrl: t = &ch.newFeedUrl, c = kTextRaw; break;
      case kChComplete: t = &ch.complete; break;
      case kItGuid: t = &it.guid, c = kTextRaw; break;
      case kItTitle: t = &it.title, c = kTextRaw; break;
      case kItItunesTitle: t = &it.itunesTitle, c = kTextRaw; break;
      case kItDate: t = &it.date; break;
      case kItDuration: t = &it.duration; break;
      case kItSeason: t = &it.season; break;
      case kItEpisode: t = &it.episode; break;
      case kItEpisodeType: t = &it.episodeType; break;
      case kItEncoded: t = &it.encoded, c = kLongRaw; break;
      case kItSummary: t = &it.summary, c = kLongRaw; break;
      case kItDescription: t = &it.description, c = kLongRaw; break;
      default: return;
    }
    capturing = true, full = false, cap = c;
    if (blank(*t)) {  // the first one with text counts
      t->clear();
      target = t;
    } else {
      target = nullptr;
    }
  }
  void endCapture() { capturing = false, target = nullptr, full = false; }

  void pushEl(std::string_view name, Kind k) {
    if (stack.size() <= size_t(depth)) stack.emplace_back();
    stack[size_t(depth)].name.assign(name.data(), name.size());
    stack[size_t(depth)].kind = k;
    depth++;
  }
  void closeTop() {
    Kind k = stack[size_t(--depth)].kind;
    if (k == kItem) {
      itemLevel = -1;
      if (!ending) endItem();
    }
    if (depth == 0) done = true;
  }
  void popTo(int level) {
    while (depth > level) closeTop();
  }
  int find(std::string_view name) const {
    for (int k = depth - 1; k >= 0; k--)
      if (stack[size_t(k)].name == name) return k;
    return -1;
  }
  // a tag inside a text field (HTML written in without CDATA): kept, decoded, for plainText()
  void putRaw(const char* s, size_t n, bool closed) {
    put("<", 1);
    xmlDecode(s, n, [this](const char* p, size_t k) { put(p, k); });
    if (closed) put(">", 1);
  }

  void startTag(std::string_view name, const char* attrs, size_t an, bool selfClosing, const char* raw, size_t rn, bool closed) {
    if (capturing) {
      putRaw(raw, rn, closed);
      return;
    }
    if (!rootSeen) {
      rootSeen = true;
      if (name != "rss") {
        notFeed = true;
        return;
      }
      rss = true;
      if (selfClosing) done = true;
      else pushEl(name, kRss);
      return;
    }
    if (overflow || depth >= kMaxDepth) {
      if (!selfClosing) overflow++;
      return;
    }
    if (depth == 0) return;
    if (itemLevel >= 0 && name == "item") popTo(itemLevel);  // the item before it wasn't closed
    Kind k = classify(stack[size_t(depth - 1)].kind, name);
    switch (k) {
      case kChannel: channelSeen = true; break;
      case kItem:
        it.clear();
        itemLevel = depth;
        break;
      case kChItunesImage:
        if (blank(ch.itunesImage)) ch.itunesImage = attr(attrs, an, "href");
        break;
      case kItImage:
        if (blank(it.image)) it.image = attr(attrs, an, "href");
        break;
      case kItChapters:
        if (blank(it.chapters)) it.chapters = attr(attrs, an, "url");
        break;
      case kItEnclosure: enclosure(attrs, an); break;
      default: beginCapture(k); break;
    }
    if (!selfClosing) {
      pushEl(name, k);
      return;
    }
    if (capturing) endCapture();
    if (k == kItem) {
      itemLevel = -1;
      endItem();
    }
  }

  void endTag(std::string_view name, const char* raw, size_t rn, bool closed) {
    if (overflow) {
      overflow--;
      return;
    }
    int at = find(name);
    if (capturing) {
      if (at < 0) {
        putRaw(raw, rn, closed);
        return;
      }
      endCapture();
    }
    if (at >= 0) popTo(at);  // closing what's still open in it too
  }

  // ---- items ----

  // the first enclosure with a URL whose type says audio/ or video/, else the first with a URL
  void enclosure(const char* attrs, size_t an) {
    std::string url = attr(attrs, an, "url");
    if (url.empty()) return;
    std::string type = lowered(attr(attrs, an, "type"));
    bool av = startsWith(type, "audio/") || startsWith(type, "video/");
    if (!it.url.empty() && (it.av || !av)) return;
    it.url = std::move(url);
    it.type = std::move(type);
    it.size = wholeNumber(attr(attrs, an, "length"), (int64_t(1) << 53) - 1);
    it.av = av;
  }

  void endItem() {
    if (it.url.empty()) return;
    Episode e;
    e.url = it.url;
    e.type = it.type.empty() ? guessType(e.url) : it.type;
    e.size = it.size;
    e.guid = field(it.guid);
    if (e.guid.empty()) e.guid = e.url;
    e.title = firstPlain({&it.title, &it.itunesTitle}, kTitleMax, true);
    e.date = parseDate(it.date);
    e.duration = parseDuration(it.duration);
    e.image = it.image;
    e.chapters = it.chapters;
    e.season = int(wholeNumber(it.season, INT_MAX));
    e.number = int(wholeNumber(it.episode, INT_MAX));
    std::string type = lowered(field(it.episodeType));
    if (type == "trailer" || type == "bonus") e.episodeType = type;
    e.notes = firstPlain({&it.encoded, &it.summary, &it.description}, kNotesMax, false);
    delivered++;
    if (onEpisode) onEpisode(std::move(e));
  }

  // The end of the bytes: what's still open is closed (a feed without its last closing tags reads as it
  // is), but as the server reads it, a text element or an item that never closed counts for nothing (a
  // feed cut short ends with the last whole item).
  void end() {
    if (capturing && target) target->clear();
    endCapture();
    ending = true;
    popTo(0);
  }

  void fill(Channel& out) const {
    out.title = firstPlain({&ch.title}, kTitleMax, true);
    out.author = firstPlain({&ch.author, &ch.ownerName, &ch.editor}, kTitleMax, true);
    out.description = firstPlain({&ch.summary, &ch.description}, kDescriptionMax, false);
    out.art = field(ch.itunesImage);
    if (out.art.empty()) out.art = field(ch.imageUrl);
    out.link = field(ch.link);
    out.language = lowered(field(ch.language));
    out.serial = ieq(field(ch.type), "serial");
    std::string ex = field(ch.explicitness);
    out.explicitContent = ieq(ex, "yes") || ieq(ex, "true") || ieq(ex, "explicit");
    out.newFeedUrl = field(ch.newFeedUrl);
    out.complete = ieq(field(ch.complete), "yes");
  }
};

Parser::Parser(std::function<void(Episode&&)> onEpisode) : impl_(new Impl(std::move(onEpisode))) {}
Parser::~Parser() = default;

bool Parser::feed(const char* data, size_t n) {
  Impl& p = *impl_;
  if (p.notFeed) return false;
  if (p.done || p.finished || !n) return true;
  if (!p.started) {
    p.head.append(data, n);
    if (!p.start(false)) return true;
    std::string h;
    h.swap(p.head);
    if (!p.notFeed) p.push(h.data() + p.skip, h.size() - p.skip, false);
    return !p.notFeed;
  }
  p.push(data, n, false);
  return !p.notFeed;
}

bool Parser::finish(Channel& out) {
  Impl& p = *impl_;
  if (!p.finished) {
    p.finished = true;
    if (!p.notFeed && !p.started) {
      p.start(true);
      std::string h;
      h.swap(p.head);
      if (!p.notFeed) p.push(h.data() + p.skip, h.size() - p.skip, true);
    } else if (!p.notFeed && !p.done) {
      p.push(nullptr, 0, true);
    }
    if (!p.notFeed) p.end();
  }
  out = Channel();
  if (p.notFeed || !p.rss || !p.channelSeen) return false;
  p.fill(out);
  return true;
}

int Parser::count() const { return impl_->delivered; }

// ---- plain text, dates, durations, types ----

std::string plainText(const std::string& html, size_t maxBytes, bool oneLine) {
  TextOut w{std::string(), maxBytes, oneLine};
  w.s.reserve(std::min(html.size(), maxBytes + 4));
  const char* p = html.data();
  const char* e = p + html.size();
  while (p < e && !w.full) {
    char c = *p;
    switch (c) {
      case '<': p = htmlTag(p, e, w); break;
      case '&': {
        uint32_t cp;
        size_t len = entityAt(p, size_t(e - p), true, cp);
        if (len) {
          w.putCodePoint(cp);
          p += len;
        } else {
          w.put("&", 1);
          p++;
        }
        break;
      }
      case ' ':
      case '\t':
      case '\f':
      case '\v': w.gap(), p++; break;
      case '\n': w.line(), p++; break;
      case '\r':
        if (p + 1 >= e || p[1] != '\n') w.line();
        p++;
        break;
      default: {
        if (u8(c) >= 0x80) {
          uint32_t cp;
          size_t len = utf8At(p, e, cp);
          if (cp == 0xFFFD) w.put(kReplacement, 3);
          else if (cp == 0xA0) w.gap();
          else if (cp > 0x9F) w.put(p, len);
          p += len;
          break;
        }
        if (u8(c) < 0x20 || c == 0x7F) {  // control characters
          p++;
          break;
        }
        const char* q = p + 1;
        while (q < e && u8(*q) > 0x20 && u8(*q) < 0x7F && *q != '<' && *q != '&') q++;
        w.put(p, size_t(q - p));
        p = q;
      }
    }
  }
  return w.finish();
}

int64_t parseDate(const std::string& raw) {
  const char* p = raw.data();
  const char* e = p + raw.size();
  while (p < e && dateSpace(*p)) p++;
  while (e > p && dateSpace(e[-1])) e--;
  int64_t t = rfcDate(p, e);
  return t >= 0 ? t : isoDate(p, e);
}

// "H:MM:SS", "MM:SS" or seconds, the last with a fraction or not (rounded down), as the server reads them
int parseDuration(const std::string& raw) {
  std::string s = trimmed(raw);
  const char* p = s.data();
  const char* e = p + s.size();
  int64_t total = 0;
  for (int parts = 1;; parts++) {
    if (p >= e || !isDigit(*p)) return 0;
    int64_t v = 0;
    for (int nd = 0; p < e && isDigit(*p); p++, nd++) {
      if (nd >= 12) return 0;
      v = v * 10 + (*p - '0');
    }
    total = total * 60 + v;
    if (p < e && *p == ':') {
      if (parts == 3) return 0;
      p++;
      continue;
    }
    if (p < e && *p == '.') {
      const char* f = ++p;
      while (p < e && isDigit(*p)) p++;
      if (p == f) return 0;
    }
    if (p != e) return 0;
    break;
  }
  return total > INT_MAX ? 0 : int(total);
}

std::string guessType(const std::string& url) {
  size_t end = url.find_first_of("?#");
  if (end == std::string::npos) end = url.size();
  size_t start = 0;
  size_t scheme = url.find("://");
  if (scheme != std::string::npos && scheme < end) {
    start = url.find('/', scheme + 3);
    if (start == std::string::npos || start >= end) return "";
  }
  std::string_view path(url.data() + start, end - start);
  size_t slash = path.rfind('/');
  std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
  size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return "";
  std::string_view ext = name.substr(dot + 1);
  static const struct {
    const char* ext;
    const char* type;
  } types[] = {{"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"m4b", "audio/mp4"},       {"aac", "audio/aac"}, {"ogg", "audio/ogg"},
               {"opus", "audio/ogg"}, {"mp4", "video/mp4"}, {"m4v", "video/mp4"},       {"mov", "video/quicktime"}};
  for (const auto& t : types)
    if (ieq(ext, t.ext)) return t.type;
  return "";
}

}  // namespace rondo::direct::rss
