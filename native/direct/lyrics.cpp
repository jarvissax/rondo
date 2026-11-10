// lyrics.h. The function names follow lyrics.ts.
#include "lyrics.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "../core/fold.h"
#include "innertube.h"
#include "json.h"
#include "model.h"
#include "music.h"

namespace rondo::direct {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(tolower((unsigned char)c));
  return s;
}

std::string squeeze(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (isspace((unsigned char)c)) {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += c;
    }
  }
  return trim(out);
}

// accent-, case- and punctuation-blind, for comparing names
std::string folded(const std::string& s) {
  std::string f = rondo::fold(s), out;
  for (unsigned char c : f)
    if (c >= 0x80 || isalnum(c)) out += char(c);
  return out;
}

bool sameArtist(const std::string& a, const std::string& b) {
  std::string x = folded(a), y = folded(b);
  if (x.empty() || y.empty()) return false;
  return x == y || (std::min(x.size(), y.size()) >= 3 && (x.find(y) != std::string::npos || y.find(x) != std::string::npos));
}

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::string w;
  for (char c : lower(s)) {
    if (isspace((unsigned char)c) || strchr(",.:;!_-", c)) {
      if (!w.empty()) out.push_back(w), w.clear();
    } else {
      w += c;
    }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

// a bracket that describes the upload, never the song: "(Official Music Video)", "[Lyrics]", "(2011)"
bool isNoise(const std::string& inner) {
  static const std::set<std::string> kNoise = {
      "official", "music", "video", "videoclip", "audio", "lyric", "lyrics", "visualizer", "visualiser", "remaster",
      "remastered", "hd", "hq", "4k", "8k", "uhd", "1080p", "720p", "mv", "m/v", "clip", "explicit", "resmi", "klip",
      "oficial", "officiel", "letra", "paroles", "\xc5\x9f" "ark\xc4\xb1", "s\xc3\xb6zleri"};
  std::vector<std::string> w = words(inner);
  if (w.empty()) return false;
  for (auto& x : w) {
    bool year = x.size() == 4 && (x.compare(0, 2, "19") == 0 || x.compare(0, 2, "20") == 0) && isdigit((unsigned char)x[2]) && isdigit((unsigned char)x[3]);
    if (!kNoise.count(x) && !year && x != "&") return false;
  }
  return true;
}

// a bracket that credits people: "(feat. X)", "(with X)", "(prod. X)"
bool isCredit(const std::string& inner) {
  std::string l = lower(trim(inner)) + " ";
  for (const char* p : {"feat. ", "feat ", "ft. ", "ft ", "featuring ", "with ", "prod. ", "prod ", "produced by "})
    if (l.compare(0, strlen(p), p) == 0) return true;
  return false;
}

// the title without its brackets' noise (or without any bracket at all)
std::string dropBrackets(const std::string& t, bool all) {
  std::string out;
  size_t i = 0;
  while (i < t.size()) {
    const char* close = nullptr;
    size_t open = 0;
    if (t[i] == '(') close = ")", open = 1;
    else if (t[i] == '[') close = "]", open = 1;
    else if (t.compare(i, 3, "\xe3\x80\x90") == 0) close = "\xe3\x80\x91", open = 3;  // 【 】
    size_t end = close ? t.find(close, i + open) : std::string::npos;
    if (close && end != std::string::npos) {
      std::string inner = t.substr(i + open, end - i - open);
      if (all || isNoise(inner) || isCredit(inner)) {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        i = end + strlen(close);
        continue;
      }
    }
    out += t[i++];
  }
  return squeeze(out);
}

bool endsWithI(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && lower(s.substr(s.size() - tail.size())) == tail;
}

// cleanTitle: noise brackets, featured artists, " | …" tails, HD/4K and " - Remastered" suffixes
std::string cleanTitle(const std::string& raw) {
  std::string t = raw;
  for (const char* dash : {" \xe2\x80\x93 ", " \xe2\x80\x94 "}) {  // – —
    size_t at;
    while ((at = t.find(dash)) != std::string::npos) t.replace(at, strlen(dash), " - ");
  }
  t = squeeze(t);
  size_t bar = t.find(" | ");
  if (bar != std::string::npos) t = t.substr(0, bar);
  t = dropBrackets(t, false);
  // " feat. X" up to " - ", a bracket or the end
  std::string l = lower(t);
  for (const char* f : {" feat. ", " feat ", " ft. ", " ft ", " featuring "}) {
    size_t at = l.find(f);
    if (at == std::string::npos) continue;
    size_t stop = l.find(" - ", at + 1);
    size_t br = l.find_first_of("([", at + 1);
    stop = std::min(stop, br);
    t = t.substr(0, at) + (stop == std::string::npos ? "" : t.substr(stop));
    l = lower(t);
  }
  for (std::string prev; prev != t;) {
    prev = t;
    for (const char* s : {" hd", " hq", " 4k", " 8k", " uhd", " 1080p", " 720p", " official music video", " official lyric video",
                          " official video", " official audio", " official music audio", " - remastered", " - remaster", " - radio edit",
                          " - single version", " - album version", " - mono", " - stereo"})
      if (endsWithI(t, s)) t = trim(t.substr(0, t.size() - strlen(s)));
    // " - 2011 Remaster", " - Remastered 2009"
    size_t d = t.rfind(" - ");
    if (d != std::string::npos) {
      std::vector<std::string> w = words(t.substr(d + 3));
      bool remaster = !w.empty() && std::all_of(w.begin(), w.end(), [](const std::string& x) {
        return x == "remaster" || x == "remastered" || x == "version" || (x.size() == 4 && isdigit((unsigned char)x[0]));
      }) && std::any_of(w.begin(), w.end(), [](const std::string& x) { return x.compare(0, 8, "remaster") == 0; });
      if (remaster) t = trim(t.substr(0, d));
    }
  }
  t = squeeze(t);
  return t.empty() ? trim(raw) : t;
}

// mainArtist: the first of several, without YouTube's channel suffixes ("X - Topic", "XVEVO")
std::string mainArtist(const std::string& raw) {
  std::string a = trim(raw);
  if (endsWithI(a, " - topic")) a = trim(a.substr(0, a.size() - 8));
  if (endsWithI(a, "vevo")) a = trim(a.substr(0, a.size() - 4));
  std::string l = lower(a);
  size_t cut = std::string::npos;
  for (const char* sep : {",", " & ", "&", "\xe2\x80\xa2", "\xc2\xb7", " x ", " feat. ", " feat ", " ft. ", " ft ", " featuring "}) {
    size_t at = l.find(sep);
    if (at != std::string::npos && at > 0) cut = std::min(cut, at);
  }
  return trim(cut == std::string::npos ? a : a.substr(0, cut));
}

std::string unquote(const std::string& s) {
  std::string t = trim(s);
  for (const char* q : {"\"", "\xe2\x80\x9c", "\xe2\x80\x9d", "\xc2\xab", "\xc2\xbb"}) {
    size_t l = strlen(q);
    while (t.size() >= l && t.compare(0, l, q) == 0) t.erase(0, l);
    while (t.size() >= l && t.compare(t.size() - l, l, q) == 0) t.erase(t.size() - l);
  }
  return trim(t);
}

bool versionWord(const std::string& s) {
  for (auto& w : words(dropBrackets(s, true)))
    for (const char* v : {"live", "acoustic", "remix", "mix", "edit", "version", "instrumental", "demo", "session", "unplugged", "cover"})
      if (w == v) return true;
  return false;
}

struct Query {
  std::string title, artist;
};

// lyricsQueries: the cleaned title (split from "Artist - Title" where it is), an alternative reading,
// the title without any brackets, and the title and artist as given
std::vector<Query> queries(const std::string& rawTitle, const std::string& rawArtist) {
  std::string title = cleanTitle(rawTitle), artist = mainArtist(rawArtist);
  std::vector<Query> out;
  size_t dash = title.find(" - ");
  if (dash != std::string::npos && dash > 0) {
    std::string left = trim(title.substr(0, dash)), right = unquote(title.substr(dash + 3));
    if (artist.empty() || sameArtist(left, artist)) out.push_back({right, mainArtist(left)});
    else if (sameArtist(right, artist)) out.push_back({unquote(left), artist});
    else if (versionWord(right)) out.push_back({unquote(title), artist});
    else out.push_back({right, mainArtist(left)}), out.push_back({unquote(title), artist});
  } else {
    out.push_back({unquote(title), artist});
  }
  std::string bare = dropBrackets(out[0].title, true);
  if (!bare.empty()) out.push_back({bare, out[0].artist});
  out.push_back({squeeze(rawTitle), squeeze(rawArtist)});
  std::set<std::string> seen;
  std::vector<Query> unique;
  for (auto& q : out) {
    std::string k = folded(q.title) + "|" + folded(q.artist);
    if (!folded(q.title).empty() && seen.insert(k).second) unique.push_back(q);
  }
  return unique;
}

struct LrcLine {
  int t;
  std::string text;
};

// parseLrc: lines in time order; tags dropped, [offset:] applied, a line at each of its stamps
std::vector<LrcLine> parseLrc(const std::string& lrc) {
  std::vector<LrcLine> lines;
  int offset = 0;
  size_t a = 0;
  while (a <= lrc.size()) {
    size_t b = lrc.find('\n', a);
    std::string rest = trim(lrc.substr(a, b == std::string::npos ? std::string::npos : b - a));
    a = b == std::string::npos ? lrc.size() + 1 : b + 1;
    std::vector<int> stamps;
    for (;;) {  // [mm:ss], [mm:ss.x], [mm:ss.xx], [mm:ss.xxx], ':' before the fraction too
      if (rest.empty() || rest[0] != '[') break;
      size_t close = rest.find(']');
      if (close == std::string::npos) break;
      std::string in = rest.substr(1, close - 1);
      size_t colon = in.find(':');
      if (colon == std::string::npos || colon == 0 || colon > 3) break;
      std::string mm = in.substr(0, colon), ss = in.substr(colon + 1), frac;
      size_t dot = ss.find_first_of(".:");
      if (dot != std::string::npos) frac = ss.substr(dot + 1), ss = ss.substr(0, dot);
      auto digits = [](const std::string& s, size_t lo, size_t hi) {
        return s.size() >= lo && s.size() <= hi && s.find_first_not_of("0123456789") == std::string::npos;
      };
      if (!digits(mm, 1, 3) || !digits(ss, 1, 2) || (dot != std::string::npos && !digits(frac, 1, 3))) break;
      while (frac.size() < 3) frac += '0';
      stamps.push_back((atoi(mm.c_str()) * 60 + atoi(ss.c_str())) * 1000 + (dot != std::string::npos ? atoi(frac.c_str()) : 0));
      rest = trim(rest.substr(close + 1));
    }
    if (!stamps.empty()) {
      std::string text;
      for (size_t k = 0; k < rest.size(); k++) {  // enhanced LRC's word stamps <00:12.34>
        if (rest[k] == '<') {
          size_t e = rest.find('>', k);
          if (e != std::string::npos && e - k <= 12 && isdigit((unsigned char)rest[k + 1])) {
            k = e;
            continue;
          }
        }
        text += rest[k];
      }
      text = squeeze(text);
      for (int t : stamps) lines.push_back({t, text});
      continue;
    }
    std::string l = lower(rest);
    if (l.compare(0, 8, "[offset:") == 0 && !l.empty() && l.back() == ']') offset = atoi(l.c_str() + 8);
  }
  for (auto& l : lines) l.t = std::max(0, l.t - offset);
  std::stable_sort(lines.begin(), lines.end(), [](const LrcLine& x, const LrcLine& y) { return x.t < y.t; });
  return lines;
}

std::string tidy(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r') {
      if (i + 1 < s.size() && s[i + 1] == '\n') continue;
      out += '\n';
    } else {
      out += s[i];
    }
  }
  std::string lines, line;  // trailing spaces off every line
  size_t a = 0;
  while (a <= out.size()) {
    size_t b = out.find('\n', a);
    line = out.substr(a, b == std::string::npos ? std::string::npos : b - a);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    lines += (a ? "\n" : "") + line;
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return trim(lines);
}

std::string syncedJson(const std::vector<LrcLine>& lines) {
  std::string s = "[";
  for (size_t k = 0; k < lines.size(); k++) s += (k ? "," : "") + std::string("{\"t\":") + std::to_string(lines[k].t) + ",\"text\":" + quote(lines[k].text) + "}";
  return s + "]";
}

std::string answer(const std::vector<LrcLine>* synced, const std::string* plain, bool instrumental, const char* source) {
  return std::string("{\"synced\":") + (synced ? syncedJson(*synced) : "null") + ",\"plain\":" + (plain ? quote(*plain) : "null") +
         ",\"instrumental\":" + (instrumental ? "true" : "false") + ",\"source\":" + (source ? quote(source) : "null") + "}";
}

const char* kLrclibUa = "Rondo (https://github.com/jarvissax/rondo)";

std::string lrclibUrl(const char* path, const std::vector<std::pair<const char*, std::string>>& params) {
  std::string u = std::string("https://lrclib.net") + path;
  char sep = '?';
  for (auto& [k, v] : params) {
    if (v.empty()) continue;
    u += sep + std::string(k) + "=" + uriComponent(v);
    sep = '&';
  }
  return u;
}

// fromLrclib: an LRCLIB record as the answer; "" when it holds nothing usable
std::string fromLrclib(J e) {
  std::string syncedText = str(key(e, "syncedLyrics")), plainText = str(key(e, "plainLyrics"));
  std::vector<LrcLine> parsed = trim(syncedText).empty() ? std::vector<LrcLine>() : parseLrc(syncedText);
  bool synced = std::any_of(parsed.begin(), parsed.end(), [](const LrcLine& l) { return !l.text.empty(); });
  std::string plain;
  if (!trim(plainText).empty()) plain = tidy(plainText);
  else if (synced) {
    for (size_t k = 0; k < parsed.size(); k++) plain += (k ? "\n" : "") + parsed[k].text;
    plain = tidy(plain);
  }
  if (synced || !plain.empty()) return answer(synced ? &parsed : nullptr, plain.empty() ? nullptr : &plain, false, "lrclib");
  if (key(e, "instrumental") && yyjson_is_true(key(e, "instrumental"))) return answer(nullptr, nullptr, true, "lrclib");
  return "";
}

// pickMatch: synced lyrics within ±3 s, any synced lyrics, plain lyrics (closest first), then an
// instrumental record within ±3 s
J pickMatch(J list, int durationSec) {
  auto gap = [&](J e) -> double {
    if (durationSec <= 0) return 0;
    J d = key(e, "duration");
    return d && yyjson_is_num(d) ? fabs(yyjson_get_num(d) - durationSec) : 1e9;
  };
  auto near = [&](J e) { return durationSec > 0 && gap(e) <= 3; };
  auto has = [](J e, const char* k) { return !trim(str(key(e, k))).empty(); };
  for (int tier = 0; tier < 4; tier++) {
    J best = nullptr;
    each(list, [&](J e) {
      bool ok = tier == 0 ? has(e, "syncedLyrics") && near(e) : tier == 1 ? has(e, "syncedLyrics") : tier == 2 ? has(e, "plainLyrics")
                                                                                         : yyjson_is_true(key(e, "instrumental")) && near(e);
      if (ok && (!best || gap(e) < gap(best))) best = e;
    });
    if (best) return best;
  }
  return nullptr;
}

// lrclibLookup: /api/get with the first query, then /api/search with each; false when LRCLIB failed
bool lrclib(const std::vector<Query>& qs, const std::string& album, int durationSec, std::string& out) {
  std::vector<std::string> hs = {std::string("User-Agent: ") + kLrclibUa, "Accept: application/json"};
  if (!qs.empty() && !qs[0].artist.empty()) {
    std::string d = durationSec >= 1 && durationSec <= 3600 ? std::to_string(durationSec) : "";
    Fetched f = get(lrclibUrl("/api/get", {{"track_name", qs[0].title}, {"artist_name", qs[0].artist}, {"album_name", album}, {"duration", d}}), hs);
    if (f.status == 200) {
      Doc doc(yyjson_read(f.body.data(), f.body.size(), 0));
      std::string found = fromLrclib(root(doc));
      if (!found.empty()) return out = found, true;
    } else if (f.status != 404) {
      return false;
    }
  }
  for (const Query& q : qs) {
    Fetched f = get(lrclibUrl("/api/search", {{"track_name", q.title}, {"artist_name", q.artist}}), hs);
    if (f.status != 200) return false;
    Doc doc(yyjson_read(f.body.data(), f.body.size(), 0));
    J match = pickMatch(root(doc), durationSec);
    std::string found = match ? fromLrclib(match) : "";
    if (!found.empty()) return out = found, true;
  }
  return true;
}

struct CachedLyrics {
  std::string json;
  int64_t until;
};
std::mutex g_mu;
std::map<std::string, CachedLyrics> g_cache;

}  // namespace

std::string lyricsJson(const std::string& title, const std::string& artist, const std::string& album, int durationSec, const std::string& id) {
  std::string k = lower(squeeze(title)) + '\0' + lower(squeeze(artist)) + '\0' + lower(squeeze(album)) + '\0' + std::to_string(durationSec) + '\0' + id;
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_cache.find(k);
    if (it != g_cache.end() && nowMs() < it->second.until) return it->second.json;
  }
  bool degraded = false;
  std::string out;
  if (isVideoId(id)) {
    music::Lyrics y;
    if (music::lyrics(id, y) && !trim(y.plain).empty()) {
      std::vector<LrcLine> lines;
      for (auto& [t, text] : y.synced) lines.push_back({t, text});
      std::string plain = tidy(y.plain);
      out = answer(lines.empty() ? nullptr : &lines, &plain, false, "youtube");
      degraded = y.partial;
    }
  }
  if (out.empty() && !lrclib(queries(title, artist), album, durationSec, out)) degraded = true;
  bool hit = !out.empty();
  if (!hit) out = answer(nullptr, nullptr, false, nullptr);
  std::lock_guard<std::mutex> l(g_mu);
  if (g_cache.size() > 500) g_cache.clear();
  g_cache[k] = {out, nowMs() + (degraded ? 5 * 60000 : hit ? 24 * 3600000 : 3600000)};
  return out;
}

}  // namespace rondo::direct
