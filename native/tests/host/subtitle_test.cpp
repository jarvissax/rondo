// The subtitle file parser (core/subtitle_file.cpp) on the Mac:
//   clang++ -std=c++17 -I native native/core/subtitle_file.cpp native/tests/host/subtitle_test.cpp -o /tmp/st && /tmp/st
#include <stdio.h>

#include <string>

#include "core/subtitle_file.h"

using namespace rondo;

static int failed = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);     \
      failed++;                                                   \
    }                                                             \
  } while (0)

int main() {
  // WebVTT as YouTube's speech recognition writes it: word timings, the 10 ms roll cues, a blank line
  auto v = parseSubtitles(
      "WEBVTT\nKind: captions\nLanguage: tr\n\n"
      "00:00:00.120 --> 00:00:01.990 align:start position:0%\n \nKaniler,<00:00:00.520><c> yapay</c>\n\n"
      "00:00:01.990 --> 00:00:02.000 align:start position:0%\nKaniler, yapay\n \n\n"
      "00:00:02.000 --> 00:00:03.949\nKaniler, yapay\nneredeyse<00:00:02.440><c> hepsi</c> &amp; &lt;b&gt;\n");
  CHECK(v.size() == 2);
  CHECK(v[0].start == 120 && v[0].end == 1990 && v[0].text == "Kaniler, yapay");
  CHECK(v[1].text == "Kaniler, yapay\nneredeyse hepsi & <b>");

  // SubRip with Windows line ends, italics, a font tag and an SSA position tag
  auto s = parseSubtitles("1\r\n00:00:01,000 --> 00:00:04,500\r\n<i>Hello</i> <font color=\"#fff\">there</font>\r\n\r\n"
                          "2\r\n00:01:02,250 --> 00:01:03,000\r\n{\\an8}Top line\r\n");
  CHECK(s.size() == 2);
  CHECK(s[0].start == 1000 && s[0].end == 4500 && s[0].text == "Hello there");
  CHECK(s[1].start == 62250 && s[1].text == "Top line");

  // Advanced SubStation: the Format line's order, commas in the text, override blocks, \N
  auto a = parseSubtitles(
      "[Script Info]\nTitle: x\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
      "Dialogue: 0,0:00:05.10,0:00:07.00,Default,,0,0,0,,{\\i1}One, two{\\i0}\\Nthree\n"
      "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,First\n");
  CHECK(a.size() == 2);
  CHECK(a[0].start == 1000 && a[0].text == "First");  // sorted by start
  CHECK(a[1].start == 5100 && a[1].end == 7000 && a[1].text == "One, two\nthree");

  // the Western code page (a Windows .srt): é and the curly quote come out as UTF-8
  auto w = parseSubtitles("1\n00:00:01,000 --> 00:00:02,000\nCaf\xe9 \x93ok\x94\n");
  CHECK(w.size() == 1 && w[0].text == "Caf\xc3\xa9 \xe2\x80\x9cok\xe2\x80\x9d");

  // UTF-16 little-endian with its mark
  std::string u = "\xff\xfe";
  for (char c : std::string("1\n00:00:01,000 --> 00:00:02,000\nHi\n")) u += c, u += '\0';
  auto x = parseSubtitles(u);
  CHECK(x.size() == 1 && x[0].text == "Hi");

  // the languages: two and three letters, a region, a name
  CHECK(languageName("en") == "English" && languageName("eng") == "English" && languageName("pt-BR") == "Portuguese");
  CHECK(std::string(findLanguage("ger")->code) == "de");
  CHECK(std::string(languageNamed("turkish")->code) == "tr");
  CHECK(languageName("xx") == "xx");

  if (failed) return fprintf(stderr, "%d failed\n", failed), 1;
  puts("subtitle_test: all passed");
}
