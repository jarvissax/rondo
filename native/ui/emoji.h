// The emoji the keyboard offers: Unicode's list in its order and groups (emoji_data.cpp, made by
// tools/emoji/make-list.cjs), less what the Y1's emoji font can't draw (Apple's or Noto's: each Y1 shows
// what it draws, no empty boxes), the recent ones and the skin tone (kept in prefs.json), and finding
// them by name.
#pragma once
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace rondo::ui {
class Text;
}

namespace rondo::ui::emoji {

struct Info {
  const char* text;  // fully qualified, without a skin tone
  const char* name;  // "Grinning face", "Flag: Japan"
  const char* key;   // the words it is found by: its name's, then "|" and its subgroup's ("food fruit")
  uint8_t group;     // 1-9, Unicode's groups (0 is Recent)
  uint16_t tone;     // its row in kTones + 1; 0: it takes no tone
};
extern const Info kList[];
extern const int kCount;
extern const char* const kTones[][5];

// the tabs: Recent, then Unicode's nine groups
constexpr int kGroups = 10, kRecentMax = 30;
const char* groupName(int g);
const char* groupIcon(int g);
// 0 (none), then light to dark
constexpr int kToneCount = 6;
const char* toneName(int t);
const char* toneHand(int t);  // ✋ in that tone, for the tone key

// What the font draws, worked out once (a shape of each emoji); the indexes into kList, in order. Two
// emoji the font draws with the same picture (Apple's draws every family as one) are offered once.
const std::vector<int>& drawn(Text& text);
// An emoji in a tone (0: as it is): the toned form when it has one and the font draws it
std::string form(Text& text, int index, int tone);
// the kList index of an emoji, with a tone or without (-1: not one of them)
int indexOf(const std::string& emoji);
// its name as shown: "Thumbs up: medium skin tone"
std::string nameOf(const std::string& emoji);

// most recent first, at most kRecentMax
const std::vector<std::string>& recent();
void used(const std::string& emoji);
int tone();
void setTone(int t);
// from prefs.json at the start, and how they go back there (called on each change)
void restore(const std::string& recentList, int tone);
std::string recentList();  // the recent ones, separated by spaces
extern std::function<void()> onChange;

// Find: the emoji (kList indexes, drawn ones only) with every word of `query` at the start of a word of
// their names or groups, the best first: names that start with it, then names that have it, then groups
std::vector<int> find(Text& text, const std::string& query);

}  // namespace rondo::ui::emoji
