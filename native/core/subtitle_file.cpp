// Subtitle files; see subtitle_file.h.
#include "subtitle_file.h"

#include <string.h>
#include <strings.h>

#include <algorithm>

namespace rondo {
namespace {

void putUtf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += char(0xe0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
  } else {
    out += char(0xf0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3f)), out += char(0x80 | ((c >> 6) & 0x3f));
    out += char(0x80 | (c & 0x3f));
  }
}

bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (n < 0) return false;
    for (int k = 1; k <= n; k++)
      if (i + size_t(k) >= s.size() || ((unsigned char)s[i + size_t(k)] >> 6) != 2) return false;
    i += size_t(n) + 1;
  }
  return true;
}

// Older subtitle files are often in Windows' Western code page (or UTF-16, from Windows' Notepad)
std::string toUtf8(const std::string& s) {
  if (s.size() >= 2 && (((unsigned char)s[0] == 0xff && (unsigned char)s[1] == 0xfe) ||
                        ((unsigned char)s[0] == 0xfe && (unsigned char)s[1] == 0xff))) {
    bool le = (unsigned char)s[0] == 0xff;
    std::string out;
    for (size_t i = 2; i + 1 < s.size(); i += 2) {
      uint32_t u = le ? uint32_t((unsigned char)s[i]) | uint32_t((unsigned char)s[i + 1]) << 8
                      : uint32_t((unsigned char)s[i]) << 8 | uint32_t((unsigned char)s[i + 1]);
      if (u >= 0xd800 && u < 0xdc00 && i + 3 < s.size()) {
        uint32_t lo = le ? uint32_t((unsigned char)s[i + 2]) | uint32_t((unsigned char)s[i + 3]) << 8
                         : uint32_t((unsigned char)s[i + 2]) << 8 | uint32_t((unsigned char)s[i + 3]);
        u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
        i += 2;
      }
      putUtf8(out, u);
    }
    return out;
  }
  if (s.compare(0, 3, "\xef\xbb\xbf") == 0) return s.substr(3);
  if (validUtf8(s)) return s;
  static const uint16_t k1252[32] = {0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021, 0x2c6,  0x2030, 0x160,
                                     0x2039, 0x152,  0x8d,   0x17d,  0x8f,   0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022,
                                     0x2013, 0x2014, 0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (unsigned char c : s) putUtf8(out, c >= 0x80 && c < 0xa0 ? k1252[c - 0x80] : c);
  return out;
}

// "01:02:03.456", "02:03.456", "01:02:03,456" (SubRip), "1:02:03.45" (SubStation's hundredths): ms, or -1
int64_t clockMs(const char* s, const char** end = nullptr) {
  while (*s == ' ' || *s == '\t') s++;
  int64_t parts[3] = {0, 0, 0};
  int n = 0;
  const char* p = s;
  while (n < 3) {
    if (*p < '0' || *p > '9') return -1;
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    parts[n++] = v;
    if (*p != ':') break;
    p++;
  }
  int64_t ms = 0;
  if (*p == '.' || *p == ',') {
    p++;
    int64_t scale = 100;
    while (*p >= '0' && *p <= '9') {
      ms += (*p++ - '0') * scale;
      scale /= 10;
    }
  }
  if (end) *end = p;
  int64_t sec = n == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : n == 2 ? parts[0] * 60 + parts[1] : parts[0];
  return sec * 1000 + ms;
}

// a line's markup gone: <i>, <c.color>, <00:00:01.520> (YouTube's word timings), {\an8}, and entities
std::string plain(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '<') {
      size_t e = s.find('>', i);
      if (e != std::string::npos) {
        i = e;
        continue;
      }
    }
    if (c == '{' && i + 1 < s.size() && s[i + 1] == '\\') {
      size_t e = s.find('}', i);
      if (e != std::string::npos) {
        i = e;
        continue;
      }
    }
    if (c == '&') {
      static const struct {
        const char* name;
        const char* text;
      } kEntities[] = {{"&amp;", "&"},  {"&lt;", "<"},  {"&gt;", ">"},   {"&nbsp;", " "}, {"&quot;", "\""},
                       {"&#39;", "'"},  {"&apos;", "'"}, {"&lrm;", ""},  {"&rlm;", ""}};
      bool hit = false;
      for (auto& en : kEntities) {
        size_t l = strlen(en.name);
        if (s.compare(i, l, en.name) == 0) {
          out += en.text;
          i += l - 1;
          hit = true;
          break;
        }
      }
      if (hit) continue;
    }
    out += c;
  }
  size_t a = out.find_first_not_of(" \t"), b = out.find_last_not_of(" \t");
  return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

void addCue(std::vector<Cue>& cues, int64_t start, int64_t end, const std::string& text) {
  // YouTube's speech recognition rolls its two lines with 10 ms cues in between: those are left out
  if (start < 0 || end - start < 50 || text.empty()) return;
  cues.push_back({start, end, text});
}

// WebVTT and SubRip: blocks with a "start --> end" line, then the text
void parseTimed(const std::vector<std::string>& lines, std::vector<Cue>& cues) {
  for (size_t i = 0; i < lines.size(); i++) {
    size_t arrow = lines[i].find("-->");
    if (arrow == std::string::npos) continue;
    int64_t start = clockMs(lines[i].c_str()), end = clockMs(lines[i].c_str() + arrow + 3);
    std::string text;
    for (i++; i < lines.size() && !lines[i].empty() && lines[i].find("-->") == std::string::npos; i++) {
      std::string l = plain(lines[i]);
      if (l.empty()) continue;
      text += (text.empty() ? "" : "\n") + l;
    }
    if (i < lines.size() && lines[i].find("-->") != std::string::npos) i--;  // a cue straight after (no blank line)
    addCue(cues, start, end, text);
  }
}

// SubStation Alpha and Advanced SubStation: the [Events] section's Dialogue lines in its Format's order
void parseSsa(const std::vector<std::string>& lines, std::vector<Cue>& cues) {
  int startAt = 1, endAt = 2, textAt = 9, fields = 10;
  bool events = false;
  for (const std::string& l : lines) {
    if (!l.empty() && l[0] == '[') {
      events = strncasecmp(l.c_str(), "[events]", 8) == 0;
      continue;
    }
    if (!events) continue;
    if (strncasecmp(l.c_str(), "Format:", 7) == 0) {
      int k = 0;
      size_t a = 7;
      while (a <= l.size()) {
        size_t b = l.find(',', a);
        std::string f = l.substr(a, b == std::string::npos ? std::string::npos : b - a);
        f.erase(0, f.find_first_not_of(' '));
        f.erase(f.find_last_not_of(' ') + 1);
        if (strcasecmp(f.c_str(), "Start") == 0) startAt = k;
        if (strcasecmp(f.c_str(), "End") == 0) endAt = k;
        if (strcasecmp(f.c_str(), "Text") == 0) textAt = k;
        k++;
        if (b == std::string::npos) break;
        a = b + 1;
      }
      fields = k;
      continue;
    }
    if (strncasecmp(l.c_str(), "Dialogue:", 9) != 0) continue;
    std::vector<std::string> f;
    size_t a = 9;
    while (int(f.size()) < fields - 1) {
      size_t b = l.find(',', a);
      if (b == std::string::npos) break;
      f.push_back(l.substr(a, b - a));
      a = b + 1;
    }
    f.push_back(l.substr(std::min(a, l.size())));
    if (int(f.size()) <= std::max(textAt, std::max(startAt, endAt))) continue;
    std::string raw = f[size_t(textAt)], text;
    for (size_t i = 0; i < raw.size(); i++) {
      if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == 'N' || raw[i + 1] == 'n')) {
        text += '\n';
        i++;
      } else if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'h') {
        text += ' ';
        i++;
      } else if (raw[i] == '{') {
        size_t e = raw.find('}', i);
        if (e == std::string::npos) break;
        i = e;
      } else {
        text += raw[i];
      }
    }
    std::string clean;
    size_t s = 0;
    while (s <= text.size()) {
      size_t e = text.find('\n', s);
      std::string line = plain(text.substr(s, e == std::string::npos ? std::string::npos : e - s));
      if (!line.empty()) clean += (clean.empty() ? "" : "\n") + line;
      if (e == std::string::npos) break;
      s = e + 1;
    }
    addCue(cues, clockMs(f[size_t(startAt)].c_str()), clockMs(f[size_t(endAt)].c_str()), clean);
  }
}

}  // namespace

namespace {
// two-letter codes first: a three-letter one's entry leads to its language's two-letter one
const SubLang kLanguages[] = {
    {"en", "English"},     {"az", "Azerbaijani"}, {"tr", "Turkish"},   {"ru", "Russian"},    {"de", "German"},
    {"fr", "French"},      {"es", "Spanish"},     {"it", "Italian"},   {"pt", "Portuguese"}, {"nl", "Dutch"},
    {"ar", "Arabic"},      {"fa", "Persian"},     {"he", "Hebrew"},    {"iw", "Hebrew"},     {"ja", "Japanese"},
    {"ko", "Korean"},      {"zh", "Chinese"},     {"hi", "Hindi"},     {"uk", "Ukrainian"},  {"pl", "Polish"},
    {"sv", "Swedish"},     {"no", "Norwegian"},   {"nb", "Norwegian"}, {"da", "Danish"},     {"fi", "Finnish"},
    {"el", "Greek"},       {"cs", "Czech"},       {"hu", "Hungarian"}, {"ro", "Romanian"},   {"bg", "Bulgarian"},
    {"ka", "Georgian"},    {"hy", "Armenian"},    {"kk", "Kazakh"},    {"uz", "Uzbek"},      {"id", "Indonesian"},
    {"vi", "Vietnamese"},  {"th", "Thai"},        {"ms", "Malay"},     {"sr", "Serbian"},    {"hr", "Croatian"},
    {"eng", "English"},    {"aze", "Azerbaijani"}, {"tur", "Turkish"}, {"rus", "Russian"},   {"ger", "German"},
    {"deu", "German"},     {"fre", "French"},     {"fra", "French"},   {"spa", "Spanish"},   {"ita", "Italian"},
    {"por", "Portuguese"}, {"jpn", "Japanese"},   {"kor", "Korean"},   {"chi", "Chinese"},   {"zho", "Chinese"},
    {"ara", "Arabic"},     {"per", "Persian"},    {"fas", "Persian"},  {"ukr", "Ukrainian"}, {"pol", "Polish"},
};
}  // namespace

const SubLang* findLanguage(const std::string& code) {
  std::string base = code.substr(0, code.find_first_of("-_"));
  for (const SubLang& l : kLanguages)
    if (strcasecmp(l.code, base.c_str()) == 0) return languageNamed(l.name);
  return nullptr;
}

const SubLang* languageNamed(const std::string& name) {
  for (const SubLang& l : kLanguages)
    if (strcasecmp(l.name, name.c_str()) == 0) return &l;
  return nullptr;
}

std::vector<Cue> parseSubtitles(std::string body) {
  body = toUtf8(body);
  std::vector<std::string> lines;
  size_t s = 0;
  while (s < body.size()) {
    size_t n = body.find('\n', s);
    std::string l = body.substr(s, n == std::string::npos ? std::string::npos : n - s);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    lines.push_back(std::move(l));
    if (n == std::string::npos) break;
    s = n + 1;
  }
  std::vector<Cue> cues;
  bool ssa = false;
  for (size_t i = 0; i < lines.size() && i < 40; i++)
    if (strncasecmp(lines[i].c_str(), "[Script Info]", 13) == 0 || strncasecmp(lines[i].c_str(), "[Events]", 8) == 0) ssa = true;
  if (ssa) parseSsa(lines, cues);
  else parseTimed(lines, cues);
  std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
  return cues;
}

std::string languageName(const std::string& code) {
  const SubLang* l = findLanguage(code);
  return l ? l->name : code;
}

}  // namespace rondo
