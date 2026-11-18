#include "emoji.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "text.h"

namespace rondo::ui::emoji {

std::function<void()> onChange;

namespace {

std::vector<std::string> g_recent;
int g_tone = 0;
std::vector<int> g_drawn;
bool g_checked = false;
// per toned row and tone: 0 not checked, 1 drawn, 2 not
std::vector<uint8_t> g_toneDrawn;
std::unordered_map<std::string, int> g_index;  // every form (with and without tones) -> its kList index

void indexAll() {
  if (!g_index.empty()) return;
  g_index.reserve(size_t(kCount) * 2);
  for (int i = 0; i < kCount; i++) {
    g_index.emplace(kList[i].text, i);
    if (kList[i].tone)
      for (int t = 0; t < 5; t++) g_index.emplace(kTones[kList[i].tone - 1][t], i);
  }
}

void changed() {
  if (onChange) onChange();
}

// the words of a query, as names' keys have them: lower case, ASCII letters and digits
std::vector<std::string> words(const std::string& q) {
  std::vector<std::string> out;
  std::string w;
  for (char c : q) {
    char l = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) {
      w += l;
    } else if (c == '\'' || uint8_t(c) >= 0x80) {
      continue;  // man's: mans, as the keys have it
    } else if (!w.empty()) {
      out.push_back(w), w.clear();
    }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

// where word w starts a word of key[from, to) (-1: nowhere); 0 when it's the first
int startsWord(const char* key, size_t from, size_t to, const std::string& w) {
  for (size_t i = from; i + w.size() <= to; i++) {
    if (i > from && key[i - 1] != ' ') continue;
    if (strncmp(key + i, w.c_str(), w.size()) == 0) return int(i - from);
  }
  return -1;
}

}  // namespace

const char* groupName(int g) {
  static const char* kNames[kGroups] = {"Recent",          "Smileys & emotion", "People & body", "Animals & nature", "Food & drink",
                                        "Travel & places", "Activities",        "Objects",       "Symbols",          "Flags"};
  return g >= 0 && g < kGroups ? kNames[g] : "";
}

const char* groupIcon(int g) {
  // 🕘 😀 👋 🐻 🍔 🚗 ⚽ 💡 🔣 🏁
  static const char* kIcons[kGroups] = {"\xf0\x9f\x95\x98", "\xf0\x9f\x98\x80", "\xf0\x9f\x91\x8b", "\xf0\x9f\x90\xbb", "\xf0\x9f\x8d\x94",
                                        "\xf0\x9f\x9a\x97", "\xe2\x9a\xbd",     "\xf0\x9f\x92\xa1", "\xf0\x9f\x94\xa3", "\xf0\x9f\x8f\x81"};
  return g >= 0 && g < kGroups ? kIcons[g] : "";
}

const char* toneName(int t) {
  static const char* kNames[kToneCount] = {"none", "light", "medium-light", "medium", "medium-dark", "dark"};
  return t >= 0 && t < kToneCount ? kNames[t] : "";
}

const char* toneHand(int t) {
  static const char* kHands[kToneCount] = {"\xe2\x9c\x8b", "\xe2\x9c\x8b\xf0\x9f\x8f\xbb", "\xe2\x9c\x8b\xf0\x9f\x8f\xbc",
                                           "\xe2\x9c\x8b\xf0\x9f\x8f\xbd", "\xe2\x9c\x8b\xf0\x9f\x8f\xbe", "\xe2\x9c\x8b\xf0\x9f\x8f\xbf"};
  return t >= 0 && t < kToneCount ? kHands[t] : "";
}

const std::vector<int>& drawn(Text& text) {
  if (g_checked) return g_drawn;
  g_checked = true;
  timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  std::unordered_set<uint32_t> seen;
  g_drawn.reserve(size_t(kCount));
  for (int i = 0; i < kCount; i++) {
    uint32_t g = text.emojiGlyph(kList[i].text);
    if (g && seen.insert(g).second) g_drawn.push_back(i);
  }
  // no color font (or none of it drawn): every emoji, as the black-and-white font can
  if (g_drawn.empty())
    for (int i = 0; i < kCount; i++) g_drawn.push_back(i);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  fprintf(stderr, "rondo: emoji: the font draws %d of %d (%lld ms)\n", int(g_drawn.size()), kCount,
          (long long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
  return g_drawn;
}

std::string form(Text& text, int index, int tone) {
  if (index < 0 || index >= kCount) return "";
  const Info& e = kList[index];
  if (!tone || !e.tone || tone >= kToneCount) return e.text;
  size_t at = size_t(e.tone - 1) * 5 + size_t(tone - 1);
  if (g_toneDrawn.size() <= at) g_toneDrawn.resize(at + 1, 0);
  const char* t = kTones[e.tone - 1][tone - 1];
  if (!g_toneDrawn[at]) g_toneDrawn[at] = text.emojiGlyph(t) || !text.emojiGlyph(e.text) ? 1 : 2;
  return g_toneDrawn[at] == 1 ? t : e.text;
}

int indexOf(const std::string& emoji) {
  indexAll();
  auto it = g_index.find(emoji);
  return it == g_index.end() ? -1 : it->second;
}

std::string nameOf(const std::string& emoji) {
  int i = indexOf(emoji);
  if (i < 0) return "";
  std::string n = kList[i].name;
  if (kList[i].tone)
    for (int t = 0; t < 5; t++)
      if (emoji == kTones[kList[i].tone - 1][t]) n += std::string(": ") + toneName(t + 1) + " skin tone";
  return n;
}

const std::vector<std::string>& recent() { return g_recent; }

void used(const std::string& emoji) {
  if (emoji.empty()) return;
  g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), emoji), g_recent.end());
  g_recent.insert(g_recent.begin(), emoji);
  if (g_recent.size() > size_t(kRecentMax)) g_recent.resize(size_t(kRecentMax));
  changed();
}

int tone() { return g_tone; }

void setTone(int t) {
  g_tone = t >= 0 && t < kToneCount ? t : 0;
  changed();
}

void restore(const std::string& list, int t) {
  g_tone = t >= 0 && t < kToneCount ? t : 0;
  g_recent.clear();
  size_t a = 0;
  while (a < list.size() && g_recent.size() < size_t(kRecentMax)) {
    size_t b = list.find(' ', a);
    if (b == std::string::npos) b = list.size();
    std::string e = list.substr(a, b - a);
    if (indexOf(e) >= 0 && std::find(g_recent.begin(), g_recent.end(), e) == g_recent.end()) g_recent.push_back(e);
    a = b + 1;
  }
}

std::string recentList() {
  std::string out;
  for (const std::string& e : g_recent) out += (out.empty() ? "" : " ") + e;
  return out;
}

std::vector<int> find(Text& text, const std::string& query) {
  std::vector<std::string> ws = words(query);
  std::vector<int> out;
  if (ws.empty()) return out;
  std::vector<std::pair<int, int>> scored;  // rank, index
  for (int i : drawn(text)) {
    const char* key = kList[i].key;
    size_t len = strlen(key);
    const char* bar = strchr(key, '|');
    size_t nameEnd = bar ? size_t(bar - key) : len;
    int rank = 0;
    bool all = true;
    for (size_t k = 0; k < ws.size() && all; k++) {
      int at = startsWord(key, 0, nameEnd, ws[k]);
      if (at < 0) {
        if (!bar || startsWord(key, nameEnd + 1, len, ws[k]) < 0) all = false;
        else rank = std::max(rank, 2);  // only in its group's words
      } else if (k == 0 && at > 0) {
        rank = std::max(rank, 1);  // in the name, not at its start
      }
    }
    if (all) scored.push_back({rank, i});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  out.reserve(scored.size());
  for (const auto& s : scored) out.push_back(s.second);
  return out;
}

}  // namespace rondo::ui::emoji
