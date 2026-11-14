// core/lookup.h: the matching offline, then (with --live) each service asked for real, with the
// Mac's libcurl:
//   Y=native/third_party/src/yyjson-0.12.0/src; clang -c $Y/yyjson.c -o /tmp/yy.o
//   clang++ -std=c++17 -I native -I $Y native/core/lookup.cpp native/core/fold.cpp /tmp/yy.o \
//     native/tests/host/lookup_test.cpp -lcurl -o /tmp/lt && /tmp/lt --live
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "core/lookup.h"

using namespace rondo::lookup;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static size_t collect(char* p, size_t s, size_t n, void* out) {
  static_cast<std::string*>(out)->append(p, s * n);
  return s * n;
}
static int fetch(const std::string& url, std::string& body) {
  CURL* c = curl_easy_init();
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Rondo/test ( https://github.com/jarvissax/rondo )");
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
  long st = 0;
  if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
  curl_easy_cleanup(c);
  return int(st);
}

int main(int argc, char** argv) {
  check(matchName("Moonmadness (2023 Remastered & Expanded Edition)") == "moonmadness", "brackets left out");
  check(matchName("Kelebekler - Single") == matchName("Kelebekler"), "\" - Single\" left out");
  check(matchName("Musiqi Kolleksiyası") == matchName("Musiqi kolleksiyası"), "case and letters folded");
  check(matchName("Rock & Roll") == "rock and roll", "& as and");
  check(sameArtist("AC/DC", "AC-DC") && sameArtist("BLOK3", "Blok 3"), "punctuation and spaces don't count");
  check(sameArtist("Madrigal & Kalben", "Madrigal") && sameArtist("Tarkan feat. X", "Tarkan"), "one of several artists");
  check(!sameArtist("Camel", "Camelphat"), "another artist isn't");
  check(sameArtist("Jimi Hendrix", "The Jimi Hendrix Experience"), "one inside the other, whole words");
  check(titleCase("progressive rock") == "Progressive Rock" && titleCase("hip-hop") == "Hip-Hop" && titleCase("r&b") == "R&B", "genres in title case");
  if (argc > 1 && !strcmp(argv[1], "--live")) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::vector<Title> ts;
    check(findSeries(fetch, "Silo", ts) == Result::Found && ts[0].name == "Silo" && ts[0].years == "2023-", "Silo (2023-)");
    for (auto& t : ts) printf("    %s (%s) %s\n", t.name.c_str(), t.years.c_str(), t.detail.c_str());
    check(findSeries(fetch, "The Walking Dead", ts) == Result::Found && ts[0].years == "2010-2022", "The Walking Dead (2010-2022)");
    check(findSeries(fetch, "Chernobyl", ts) == Result::Found && ts[0].years == "2019", "Chernobyl (2019): one year");
    check(findMovie(fetch, "Interstellar", ts) == Result::Found && ts[0].years == "2014", "Interstellar (2014)");
    for (auto& t : ts) printf("    %s (%s) %s\n", t.name.c_str(), t.years.c_str(), t.detail.c_str());
    check(findMovie(fetch, "Babam ve Oğlum", ts) == Result::Found && ts[0].name == "Babam ve Oğlum" && ts[0].years == "2005", "a Turkish film by its own name");
    check(findMovie(fetch, "Arrival", ts) == Result::Found && ts[0].years == "2016", "Arrival (2016)");
    Album a;
    check(findAlbum(fetch, "Camel", "Moonmadness", "", {"us"}, true, a) == Result::Found && a.genre == "Rock" && a.year == 1976 && !a.tracks.empty(),
          "Camel's Moonmadness: Rock, 1976, its tracks");
    printf("    %s / %s [%s] %d %s, %zu tracks\n", a.artist.c_str(), a.album.c_str(), a.genre.c_str(), a.year, a.source.c_str(), a.tracks.size());
    a = Album();
    check(findAlbum(fetch, "Sezen Aksu", "Firuze", "Firuze", {"us", "tr"}, false, a) == Result::Found && !a.genre.empty(), "Sezen Aksu's Firuze from the Turkish store");
    printf("    %s / %s [%s] %d %s\n", a.artist.c_str(), a.album.c_str(), a.genre.c_str(), a.year, a.source.c_str());
    a = Album();
    check(findAlbum(fetch, "Madrigal", "Kelebekler", "Kelebekler", {"us"}, false, a) == Result::Found && a.genre == "Alternative", "a single");
    a = Album();
    check(findAlbum(fetch, "Jimi Hendrix", "Electric Ladyland", "", {"us"}, false, a) == Result::Found && !a.genre.empty(), "Electric Ladyland (The Jimi Hendrix Experience)");
    a = Album();
    check(findAlbum(fetch, "The Beatles", "A Hard Day's Night (Remastered 2009 Deluxe Mono Edition)", "", {"us"}, false, a) == Result::Found && a.genre == "Rock", "an album named with what its brackets say");
    a = Album();
    check(findAlbum(fetch, "Nobody Here At All", "Nothing Of The Sort 9134", "", {"us"}, false, a) == Result::NotFound, "an album nobody has");
    Lyrics l;
    check(findLyrics(fetch, "Adele", "Hello", "25", 295, l) == Result::Found && l.synced && l.text.find("[00:") != std::string::npos, "Adele's Hello, timed");
    printf("    %s…\n", l.text.substr(0, 60).c_str());
  }
  printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures != 0;
}
