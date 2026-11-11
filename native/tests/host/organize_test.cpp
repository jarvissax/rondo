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
  };
  OrganizePlan plan = planOrganize(items, card);
  for (const OrganizeMove& m : plan.moves) printf("  %d %s\n    -> %s\n", int(m.kind), m.from.substr(card.size()).c_str(), m.to.substr(card.size()).c_str());
  check(plan.songs == 6 && plan.videos == 2, "8 files to move (one already there, one off the card)");
  check(plan.alreadyThere == 1, "the one already in place stays");
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
  check(there(card + "/Shows/Stranger Things (2016)/Season 01/Stranger Things S01E01.mp4"), "an episode into Shows");
  check(there(card + "/Shows/Stranger Things (2016)/Season 01/Stranger Things S01E01.en.srt"), "its subtitles too");
  check(there(card + "/Movies/Interstellar (2014)/Interstellar (2014).mkv"), "a movie into Movies");
  check(!there(card + "/Albums/Disc"), "the folders left empty are gone");
  check(there(card + "/Videos"), "the card's top folders stay");
  check(there(card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].mp4") && there(card + "/Videos/YouTube/Me at the zoo [jNQXAC9IVRw].en.vtt"),
        "YouTube downloads stay where they are");
  std::string cmd = "rm -r '" + card + "'";
  if (card.compare(0, 20, "/tmp/rondo-organize-") == 0) (void)!system(cmd.c_str());
  printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures != 0;
}
