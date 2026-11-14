// planOrganize / applyOrganize (core/organize.h) on a card made of empty files in a temporary folder:
//   clang++ -std=c++17 -I native native/core/organize.cpp native/core/fold.cpp native/tests/host/organize_test.cpp -o /tmp/ot && /tmp/ot
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "core/organize.h"

using namespace rondo;

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}
static void touch(const std::string& p) {
  for (size_t i = 1; i < p.size(); i++)
    if (p[i] == '/') mkdir(p.substr(0, i).c_str(), 0775);
  if (FILE* f = fopen(p.c_str(), "w")) fclose(f);
}
static bool there(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

int main() {
  char tmpl[] = "/tmp/rondo-organize-XXXXXX";
  std::string card = mkdtemp(tmpl);
  touch(card + "/Music/Pink Floyd - Comfortably Numb.flac");
  touch(card + "/Music/Pink Floyd - Comfortably Numb.lrc");
  touch(card + "/Music/Pink Floyd - Hey You.flac");
  touch(card + "/Albums/Disc/a1.mp3");
  touch(card + "/Albums/Disc/b1.mp3");
  touch(card + "/Albums/Disc/cover.jpg");
  touch(card + "/Music/untagged.mp3");
  touch(card + "/Music/AC-DC: Live? Track.m4a");
  touch(card + "/Videos/Stranger Things (2016-2025)/Stranger.Things.S01E01.mp4");
  touch(card + "/Videos/Stranger Things (2016-2025)/Stranger.Things.S01E01.en.srt");
  touch(card + "/Videos/Interstellar.2014.1080p.BluRay.x264.mkv");
  touch(card + "/Music/Already/Right (2001)/01 - Done.flac");  // a song that moves next to one in place
  touch(card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].mp4");  // YouTube › Download's: stays
  touch(card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].en.vtt");
  touch(card + "/Shows/Silo/Season 03/Silo S03E07.mp4");  // where 3.0.2 put shows
  touch(card + "/Videos/Shows/Silo (2023-)/Season 03/Silo S03E08.mp4");  // in place already
  touch(card + "/Shows/The Walking Dead/Season 01/The Walking Dead S01E01.mkv");  // years from a lookup
  touch(card + "/Movies/Arrival (2016)/movie.mkv");  // named by its folder
  std::vector<OrganizeItem> items = {
      {card + "/Music/Pink Floyd - Comfortably Numb.flac", "Comfortably Numb", "Pink Floyd", "Pink Floyd", "The Wall", 6, 2, 1979, false},
      {card + "/Music/Pink Floyd - Hey You.flac", "Hey You", "Pink Floyd", "Pink Floyd", "The Wall", 1, 2, 1979, false},
      {card + "/Albums/Disc/a1.mp3", "Intro", "Various", "Some Band", "Double", 1, 1, 2020, false},
      {card + "/Albums/Disc/b1.mp3", "Outro", "Various", "Some Band", "Double", 1, 2, 2020, false},
      {card + "/Music/untagged.mp3", "", "", "", "", 0, 0, 0, false},
      {card + "/Music/AC-DC: Live? Track.m4a", "Live: Track?", "AC/DC", "", "Live", 3, 0, 1992, false},
      {card + "/Videos/Stranger Things (2016-2025)/Stranger.Things.S01E01.mp4", "Chapter One", "", "", "", 0, 0, 0, true},
      {card + "/Videos/Interstellar.2014.1080p.BluRay.x264.mkv", "Interstellar", "", "", "", 0, 0, 0, true},
      {card + "/Music/Already/Right (2001)/01 - Done.flac", "Done", "Already", "", "Right", 1, 0, 2001, false},
      {"/elsewhere/not-on-card.mp3", "X", "Y", "", "Z", 1, 0, 0, false},
      {card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].mp4", "Me at the zoo", "", "", "", 0, 0, 0, true},
      {card + "/Shows/Silo/Season 03/Silo S03E07.mp4", "Radio", "", "", "", 0, 0, 0, true},
      {card + "/Videos/Shows/Silo (2023-)/Season 03/Silo S03E08.mp4", "", "", "", "", 0, 0, 0, true},
      {card + "/Shows/The Walking Dead/Season 01/The Walking Dead S01E01.mkv", "", "", "", "", 0, 0, 0, true},
      {card + "/Movies/Arrival (2016)/movie.mkv", "", "", "", "", 0, 0, 0, true},
  };
  std::vector<VideoTitle> titles = videoTitles(items, card);
  for (const VideoTitle& t : titles) printf("  %s %s (%s) key %s, %zu files\n", t.series ? "series" : "movie", t.name.c_str(), t.years.c_str(), t.key.c_str(), t.files.size());
  auto titleOf = [&](const std::string& name) -> const VideoTitle* {
    for (const VideoTitle& t : titles)
      if (t.name == name) return &t;
    return nullptr;
  };
  check(titles.size() == 5, "five titles: three series, two movies (YouTube's left out)");
  check(titleOf("Silo") && titleOf("Silo")->series && titleOf("Silo")->years == "2023-" && titleOf("Silo")->files.size() == 2,
        "a series' episodes together, its years from the folder above Season 03");
  check(titleOf("Stranger Things") && titleOf("Stranger Things")->years == "2016-2025", "a series' years from its folder");
  check(titleOf("Interstellar") && !titleOf("Interstellar")->series && titleOf("Interstellar")->years == "2014", "a movie's year from its name");
  check(titleOf("Arrival") && titleOf("Arrival")->years == "2016", "a movie named by its folder");
  check(titleOf("The Walking Dead") && titleOf("The Walking Dead")->years.empty(), "a series whose years aren't known");
  OrganizeNames names;
  names.titles[titleOf("The Walking Dead")->key] = {"The Walking Dead", "2010-2022"};
  OrganizePlan plan = planOrganize(items, card, names);
  for (const OrganizeMove& m : plan.moves) printf("  %d %s\n    -> %s\n", int(m.kind), m.from.substr(card.size()).c_str(), m.to.substr(card.size()).c_str());
  check(plan.songs == 6 && plan.videos == 5, "11 files to move (two already there, one off the card)");
  check(plan.alreadyThere == 2, "the ones already in place stay");
  check(plan.sidecars == 2, "the .lrc and the .srt go with their files");
  check(plan.covers == 1, "a one-album folder's cover goes with it");
  check(there(card + "/Music/Pink Floyd - Comfortably Numb.flac"), "the plan moved nothing");
  OrganizeResult r = applyOrganize(plan, card);
  for (const std::string& e : r.errors) printf("  error: %s\n", e.c_str());
  check(r.errors.empty() && r.done.size() == plan.moves.size(), "every move done");
  check(there(card + "/Music/Pink Floyd/The Wall (1979)/06 - Comfortably Numb.flac"), "Artist/Album (Year)/NN - Title");
  check(there(card + "/Music/Pink Floyd/The Wall (1979)/06 - Comfortably Numb.lrc"), "its lyrics beside it");
  check(there(card + "/Music/Some Band/Double (2020)/Disc 1/01 - Intro.mp3") && there(card + "/Music/Some Band/Double (2020)/Disc 2/01 - Outro.mp3"),
        "a two-disc album in Disc folders");
  check(there(card + "/Music/Some Band/Double (2020)/cover.jpg"), "its cover in the album's folder");
  check(there(card + "/Music/Unknown Artist/Unknown Album/untagged.mp3"), "an untagged song by its file name");
  check(there(card + "/Music/AC-DC/Live (1992)/03 - Live - Track.m4a"), "names made safe for the card");
  check(there(card + "/Videos/Shows/Stranger Things (2016-2025)/Season 01/Stranger Things S01E01.mp4"), "an episode into Videos/Shows, with its years");
  check(there(card + "/Videos/Shows/Stranger Things (2016-2025)/Season 01/Stranger Things S01E01.en.srt"), "its subtitles too");
  check(there(card + "/Videos/Shows/Silo (2023-)/Season 03/Silo S03E07.mp4"), "an episode joins its series' folder");
  check(there(card + "/Videos/Shows/The Walking Dead (2010-2022)/Season 01/The Walking Dead S01E01.mkv"), "a looked-up series' years");
  check(there(card + "/Videos/Movies/Interstellar (2014)/Interstellar (2014).mkv"), "a movie into Videos/Movies");
  check(there(card + "/Videos/Movies/Arrival (2016)/Arrival (2016).mkv"), "a movie named by its folder");
  check(!there(card + "/Albums/Disc"), "the folders left empty are gone");
  check(!there(card + "/Shows") && !there(card + "/Movies"), "3.0.2's Shows and Movies folders go once empty");
  check(there(card + "/Videos") && there(card + "/Music"), "the card's top folders stay");
  // organized once, a series keeps the years the lookup found: they're in its folder's name now
  std::vector<OrganizeItem> after = {{card + "/Videos/Shows/The Walking Dead (2010-2022)/Season 01/The Walking Dead S01E01.mkv", "", "", "", "", 0, 0, 0, true}};
  OrganizePlan again = planOrganize(after, card);
  check(again.moves.empty() && again.alreadyThere == 1, "organized once, it stays (no lookup needed again)");
  check(there(card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].mp4") && there(card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].en.vtt"),
        "YouTube downloads stay where they are");
  std::string cmd = "rm -r '" + card + "'";
  if (card.compare(0, 20, "/tmp/rondo-organize-") == 0) (void)!system(cmd.c_str());
  printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures != 0;
}
