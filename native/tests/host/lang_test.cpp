// The language catalog (core/lang.cpp) on the Mac: English and Spanish from the repo's lang/, a made-up
// language for the fallbacks, blanks and plural forms.
//   Y=native/third_party/src/yyjson-0.12.0/src; clang -c $Y/yyjson.c -o /tmp/yy.o
//   clang++ -std=c++17 -I native -I $Y native/core/lang.cpp /tmp/yy.o native/tests/host/lang_test.cpp -o /tmp/lgt && /tmp/lgt
// Run from the repo's root. The exit code is the number of failures.
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "core/lang.h"

using namespace rondo;

static int failed = 0;
static void expect(const std::string& got, const std::string& want, int line) {
  if (got == want) return;
  fprintf(stderr, "lang_test.cpp:%d: got \"%s\", want \"%s\"\n", line, got.c_str(), want.c_str());
  failed++;
}
#define EQ(a, b) expect((a), (b), __LINE__)

static void write(const std::string& path, const char* body) {
  FILE* f = fopen(path.c_str(), "w");
  fputs(body, f);
  fclose(f);
}

int main() {
  // the repo's own files: English is complete, Spanish speaks
  if (!lang::init("lang", "device", "es")) {
    fprintf(stderr, "lang_test: run from the repo's root (no lang/es.json)\n");
    return 1;
  }
  EQ(lang::current(), "es");
  EQ(tr("language.title"), "Idioma");
  EQ(lang::trIn("en", "language.title"), "Language");
  EQ(lang::nameOf("es"), "Español");
  auto langs = lang::available();
  if (langs.size() < 2 || langs[0].code != "en") fprintf(stderr, "lang_test: English isn't first\n"), failed++;
  lang::use("en");
  EQ(tr("date.short", {{"month", tr("date.month11")}, {"day", 10}}), "Nov 10");
  lang::use("es");
  EQ(tr("date.short", {{"month", tr("date.month11")}, {"day", 10}}), "10 nov");

  // a language of our own: what it lacks comes from English, what English lacks is the key
  char dir[] = "/tmp/rondo-lang-XXXXXX";
  if (!mkdtemp(dir)) return 1;
  std::string d = dir;
  write(d + "/en.json", R"J({"_name": "English", "device": {
    "a.hi": "Hello, {name}", "a.only": "Only in English",
    "a.songs": {"one": "{count} song", "other": "{count} songs"},
    "a.shown": "{shown} of {total} shown"}})J");
  write(d + "/xx.json", R"J({"_name": "Xish", "device": {
    "a.hi": "Xello {name}!",
    "a.songs": {"one": "{count} xong", "few": "{count} xongs (few)", "other": "{count} xongses"},
    "a.shown": "{total}: {shown}"}})J");
  write(d + "/ru.json", R"J({"_name": "Русский", "device": {
    "a.songs": {"one": "{count} песня", "few": "{count} песни", "many": "{count} песен", "other": "{count} песни"}}})J");
  if (!lang::init(d, "device", "xx")) fprintf(stderr, "lang_test: init xx failed\n"), failed++;
  EQ(tr("a.hi", {{"name", "Emma"}}), "Xello Emma!");
  EQ(tr("a.only"), "Only in English");
  EQ(tr("a.missing"), "a.missing");
  EQ(tr("a.shown", {{"shown", 7}, {"total", size_t(9)}}), "9: 7");
  EQ(tr("a.songs", {{"count", 1}}), "1 xong");
  EQ(tr("a.songs", {{"count", 3}}), "3 xongses");  // xx has no rule of its own: one and other
  EQ(tr("a.hi"), "Xello {name}!");                  // no args: the blank stays
  lang::use("en");
  EQ(tr("a.songs", {{"count", 0}}), "0 songs");
  EQ(tr("a.songs", {{"count", 1}}), "1 song");
  EQ(tr("a.songs", {{"count", int64_t(1234)}}), "1234 songs");
  lang::use("ru");
  EQ(tr("a.songs", {{"count", 1}}), "1 песня");
  EQ(tr("a.songs", {{"count", 3}}), "3 песни");
  EQ(tr("a.songs", {{"count", 5}}), "5 песен");
  EQ(tr("a.songs", {{"count", 21}}), "21 песня");
  EQ(tr("a.songs", {{"count", 12}}), "12 песен");
  EQ(lang::use("zz") ? "yes" : "no", "no");  // no file: the language stays
  EQ(lang::current(), "ru");
  EQ(lang::pluralForm("fr", 0), "one");
  EQ(lang::pluralForm("es", 0), "other");
  EQ(lang::pluralForm("pt-BR", 1), "one");
  EQ(lang::pluralForm("ja", 1), "other");
  // what the dashboard loads: English's keys, the language's texts where it has them
  std::string page = lang::pageStrings("device", "xx");
  if (page.find("\"code\":\"xx\"") == std::string::npos || page.find("\"a.only\":\"Only in English\"") == std::string::npos ||
      page.find("\"a.hi\":\"Xello {name}!\"") == std::string::npos || page.find("\"few\":\"{count} xongs (few)\"") == std::string::npos)
    fprintf(stderr, "lang_test: pageStrings: %s\n", page.c_str()), failed++;
  EQ(lang::pageStrings("device", "../en").substr(0, 15), "{\"code\":\"en\",\"n");
  for (const char* f : {"en", "xx", "ru"}) unlink((d + "/" + f + ".json").c_str());
  rmdir(dir);
  printf("%s\n", failed ? "FAIL" : "ok");
  return failed;
}
