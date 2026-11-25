// core/video_place.h on the Mac: which videos keep a place, their keys, where one opens.
//   clang++ -std=c++17 -I native native/core/books.cpp native/core/fold.cpp native/tests/host/video_place_test.cpp
#include <stdio.h>

#include <string>

#include "core/video_place.h"

using namespace rondo;

static int failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                        \
    }                                                                    \
  } while (0)

int main() {
  const int64_t min = 60000, hour = 60 * min;

  // ---- which videos keep a place: 10 minutes or longer ----
  CHECK(!videoKeepsPlace(9 * min + 59000));
  CHECK(videoKeepsPlace(10 * min));
  CHECK(videoKeepsPlace(2 * hour));

  // ---- a download's YouTube id, from its name ----
  CHECK(youtubeIdInName("/storage/sdcard0/Videos/YouTube/Yeralti 21. Bolum [23mAJLrcloM].mp4") == "23mAJLrcloM");
  CHECK(youtubeIdInName("/x/Some - Talk [a_b-C1d2E3f].mkv") == "a_b-C1d2E3f");
  CHECK(youtubeIdInName("Title [dQw4w9WgXcQ]") == "dQw4w9WgXcQ");
  CHECK(youtubeIdInName("/x/Movie (2023).mp4").empty());
  CHECK(youtubeIdInName("/x/Movie [2023].mp4").empty());          // not 11 characters
  CHECK(youtubeIdInName("/x/Weird [abc def ghij].mp4").empty());  // a space isn't in an id
  CHECK(youtubeIdInName("/x/[dQw4w9WgXcQ] first.mp4").empty());   // only at the end

  // ---- keys ----
  CHECK(videoKeyFor("dQw4w9WgXcQ") == "ytv:dQw4w9WgXcQ");
  CHECK(videoKeyFor("/storage/sdcard0/Videos/Silo/S01E01.mkv") == "/storage/sdcard0/Videos/Silo/S01E01.mkv");
  CHECK(videoKeyFor("/storage/sdcard0/Videos/YouTube/Talk [dQw4w9WgXcQ].mp4") == "ytv:dQw4w9WgXcQ");  // shares the stream's
  CHECK(videoKeyFor("https://host.example/ep.mp4").empty());  // a podcast episode: rss.h's
  CHECK(videoKeyFor("").empty());

  // ---- where it opens ----
  // its own place, a little earlier after a pause
  CHECK(videoStartMs(true, 20 * min, false, 1000, hour, -1) == 20 * min);
  CHECK(videoStartMs(true, 20 * min, false, 5 * min, hour, -1) == 20 * min - 2000);
  CHECK(videoStartMs(true, 20 * min, false, 2 * 24 * hour, hour, -1) == 20 * min - 30000);
  CHECK(videoStartMs(true, 10000, false, 2 * 24 * hour, hour, -1) == 0);  // not before the start
  // watched to the end, or in its last 30 s: from the beginning
  CHECK(videoStartMs(true, 59 * min + 40000, true, 1000, hour, -1) == 0);
  CHECK(videoStartMs(true, hour - 10000, false, 1000, hour, -1) == 0);
  // its own place wins over YouTube's
  CHECK(videoStartMs(true, 20 * min, false, 1000, hour, 70) == 20 * min);
  // with none: YouTube's progress, from the start past 95 %
  CHECK(videoStartMs(false, 0, false, 0, hour, 50) == 30 * min);
  CHECK(videoStartMs(false, 0, false, 0, hour, 95) == 0);
  CHECK(videoStartMs(false, 0, false, 0, hour, 100) == 0);
  CHECK(videoStartMs(false, 0, false, 0, hour, -1) == 0);
  CHECK(videoStartMs(false, 0, false, 0, 0, 50) == 0);  // no length known yet
  // short videos start over, whatever was kept
  CHECK(videoStartMs(true, 2 * min, false, 1000, 4 * min, 50) == 0);
  CHECK(videoStartMs(false, 0, false, 0, 4 * min, 50) == 0);

  if (failures) {
    fprintf(stderr, "%d failed\n", failures);
    return 1;
  }
  printf("video_place_test: all passed\n");
  return 0;
}
