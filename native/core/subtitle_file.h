// Subtitle files: the cues in WebVTT, SubRip and SubStation (ASS/SSA) files (any encoding Windows'
// Notepad saves: UTF-8, UTF-16, the Western code page), and the languages' names. No drawing here
// (shell/subtitles.h draws them).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace rondo {

struct Cue {
  int64_t start = 0, end = 0;  // ms
  std::string text;            // its lines, split by '\n'
};
// The cues in a subtitle file (any of the three kinds, told by what it holds), in the order they start
std::vector<Cue> parseSubtitles(std::string body);

struct SubLang {
  const char* code;
  const char* name;
};
// "en", "eng" or "en-GB" -> its entry, with the two-letter code (nullptr: not one of the common languages)
const SubLang* findLanguage(const std::string& code);
// "English" -> its entry (nullptr: no common language by that name)
const SubLang* languageNamed(const std::string& name);
// "en" -> "English" (the code itself for one not known)
std::string languageName(const std::string& code);

}  // namespace rondo
