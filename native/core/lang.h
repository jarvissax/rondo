// The languages Rondo speaks (lang/*.json in the repo, beside the fonts on the Y1): one file per language,
// with a section per surface ("device" here, "dashboard", "admin"). English (lang/en.json) is the source
// of every text; another language fills in what it has, and English what it lacks. docs/translating.md
// is the guide for translators.
//
// A text is either a string with named blanks ("{count} of {total} shown") or its plural forms
// ({"one": "{count} song", "other": "{count} songs"}), picked by the blank named "count" with the
// language's plural rule (CLDR's categories: zero, one, two, few, many, other).
//
//   tr("settings.title")                                   "Settings"
//   tr("theme.menuShown", {{"shown", 7}, {"total", 9}})    "7 of 9 shown"
//   tr("library.songCount", {{"count", n}})                "1 song", "12 songs"
//
// Thread-safe: the dashboard's threads translate too. A language once loaded stays in memory until
// Rondo stops, so a text taken a moment before a switch is never freed under its reader.
#pragma once
#include <stdint.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rondo {
namespace lang {

// A blank's value: text, or a whole number (written as digits; "count" also picks the plural form)
struct Arg {
  std::string text;
  long long number = 0;
  bool isNumber = false;
  Arg(const char* s) : text(s ? s : "") {}
  Arg(const std::string& s) : text(s) {}
  Arg(std::string&& s) : text(std::move(s)) {}
  Arg(std::string_view s) : text(s) {}
  template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>, int> = 0>
  Arg(T n) : text(std::to_string(n)), number((long long)n), isNumber(true) {}
};
struct Blank {
  const char* name;
  Arg value;
};
using Args = std::initializer_list<Blank>;

struct Language {
  std::string code, name;  // "es", "Español" (its own name for itself, the file's "_name")
};

// Reads <dir>/en.json and <dir>/<code>.json (their `section`) and speaks `code` from now on. false: no
// such language; English stays.
bool init(const std::string& dir, const std::string& section, const std::string& code);
// Another language, at once (Settings › Language, the dashboard). false: no such file.
bool use(const std::string& code);
std::string current();  // the language spoken now: "en", "es"
// Every language in the folder, English first, then by name
std::vector<Language> available();
std::string nameOf(const std::string& code);  // "Español"; "" when there's no such file
const std::string& folder();                  // the lang/ folder init() read
// The plural form `n` takes in a language: "one", "few", "many", "other" (CLDR's rules, for the
// languages that have a file and those likely to)
const char* pluralForm(const std::string& code, long long n);
// A text in a language other than the one spoken (the picker's title in the highlighted language);
// English's where that one has none
std::string trIn(const std::string& code, std::string_view key);
// Fills a text's blanks with args ("{name}" -> its value); a blank without an arg stays as it is.
std::string fill(std::string_view text, Args args);
// The whole section of a language with English's texts where it has none, as JSON
// {"code", "name", "languages": [{code, name}], "strings": {...}}: what a page takes (the dashboard,
// from the Y1). Read from the files each time.
std::string pageStrings(const std::string& section, const std::string& code);

}  // namespace lang

// The text for `key` in the language spoken now (English where it has none; the key itself when not
// even English has it, so a missing text shows up instead of vanishing).
std::string tr(std::string_view key);
std::string tr(std::string_view key, lang::Args args);

}  // namespace rondo
