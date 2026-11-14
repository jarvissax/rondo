// writeTags (core/tag_write.h) on files ffmpeg makes, read back with readTags and embeddedLyrics, and
// each one decoded by ffmpeg afterwards (the sound untouched):
//   Y=native/third_party/src/yyjson-0.12.0/src; clang -c $Y/yyjson.c -o /tmp/yy.o
//   clang++ -std=c++17 -Dpread64=pread -Dlseek64=lseek -Doff64_t=off_t -I native -I $Y native/core/tag_write.cpp native/core/tags.cpp native/core/lyrics.cpp \
//     native/core/fold.cpp /tmp/yy.o native/tests/host/tag_write_test.cpp -o /tmp/tw && /tmp/tw
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#include "core/lyrics.h"
#include "core/tag_write.h"
#include "core/tags.h"

using namespace rondo;

static int failures = 0;
static void check(bool ok, const std::string& what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) failures++;
}
static bool run(const std::string& cmd) { return system((cmd + " >/dev/null 2>&1").c_str()) == 0; }
static long sizeOf(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? long(st.st_size) : -1;
}
static bool decodes(const std::string& p) { return run("ffmpeg -v error -xerror -i '" + p + "' -f null -"); }

int main() {
  char tmpl[] = "/tmp/rondo-tags-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  const std::string tone = "ffmpeg -y -f lavfi -i sine=frequency=440:duration=3 ";
  const std::string lyrics = "[00:00.50]Sarı çizmeli Mehmet Ağa\n[00:01.80]Second line";
  std::string longLyrics;
  for (int i = 0; i < 400; i++) longLyrics += "[00:0" + std::to_string(i % 10) + ".00]A line of words that goes on for a while " + std::to_string(i) + "\n";
  struct Case {
    std::string name, make;
  };
  Case cases[] = {
      {"padded.flac", tone + "-metadata title=Tone -metadata artist=Tester '" + dir + "/padded.flac'"},
      {"tight.flac", tone + "-metadata title=Tone -metadata artist=Tester '" + dir + "/tight.flac' && metaflac --remove --block-type=PADDING --dont-use-padding '" + dir + "/tight.flac'"},
      {"v23.mp3", tone + "-id3v2_version 3 -metadata title=Tone -metadata artist=Tester '" + dir + "/v23.mp3'"},
      {"v24.mp3", tone + "-id3v2_version 4 -metadata title=Tone -metadata artist=Tester '" + dir + "/v24.mp3'"},
      {"bare.mp3", tone + "-write_id3v1 0 -id3v2_version 0 '" + dir + "/bare.mp3'"},
      {"front.m4a", tone + "-c:a aac -movflags +faststart -metadata title=Tone -metadata artist=Tester '" + dir + "/front.m4a'"},
      {"end.m4a", tone + "-c:a aac -metadata title=Tone -metadata artist=Tester '" + dir + "/end.m4a'"},
  };
  for (const Case& c : cases) {
    std::string p = dir + "/" + c.name;
    if (!run(c.make)) {
      check(false, c.name + ": made by ffmpeg");
      continue;
    }
    long before = sizeOf(p);
    TagChanges ch;
    ch.genre = "Turkish Pop", ch.year = 1975, ch.album = "Dönence", ch.albumArtist = "Barış Manço", ch.track = 3, ch.lyrics = lyrics;
    std::string why = writeTags(p, ch);
    check(why.empty(), c.name + ": saved" + (why.empty() ? "" : " (" + why + ")"));
    Tags t = readTags(p.c_str());
    check(t.genre == "Turkish Pop" && t.year == 1975 && t.album == "Dönence" && t.albumArtist == "Barış Manço" && t.track == 3,
          c.name + ": the new fields read back (" + t.genre + ", " + std::to_string(t.year) + ", " + t.album + ", " + t.albumArtist + ", " + std::to_string(t.track) + ")");
    check(c.name == "bare.mp3" || (t.title == "Tone" && t.artist == "Tester"), c.name + ": the old ones kept");
    check(embeddedLyrics(p) == lyrics, c.name + ": its lyrics");
    check(decodes(p), c.name + ": still plays");
    printf("     %ld -> %ld bytes\n", before, sizeOf(p));
    // again with much longer lyrics: no room left where the tags are, the file is written again
    TagChanges more;
    more.lyrics = longLyrics, more.genre = "Rock";
    why = writeTags(p, more);
    check(why.empty(), c.name + ": saved again, bigger" + (why.empty() ? "" : " (" + why + ")"));
    t = readTags(p.c_str());
    check(t.genre == "Rock" && t.year == 1975 && t.track == 3 && embeddedLyrics(p) == lyricsForFile(p).plain + embeddedLyrics(p).substr(lyricsForFile(p).plain.size()),
          c.name + ": the rest kept");
    check(embeddedLyrics(p).size() + 1 >= longLyrics.size() - 1, c.name + ": the long lyrics");
    check(decodes(p), c.name + ": still plays");
    printf("     -> %ld bytes\n", sizeOf(p));
  }
  // a YouTube Music download's: an .m4a from ffmpeg (its index at the end), its cover, link and track of total
  {
    std::string p = dir + "/song.m4a", jpg = dir + "/cover.jpg";
    if (run(tone + "-c:a aac '" + p + "'") && run("ffmpeg -y -f lavfi -i color=c=red:s=64x64 -frames:v 1 '" + jpg + "'")) {
      std::string cover;
      if (FILE* f = fopen(jpg.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) cover.append(b, n);
        fclose(f);
      }
      TagChanges ch;
      ch.title = "Air Born", ch.artist = "Camel", ch.albumArtist = "Camel", ch.album = "Moonmadness", ch.year = 1976, ch.track = 6, ch.trackTotal = 7;
      ch.comment = "https://music.youtube.com/watch?v=KNJGkyCqtTU", ch.cover = cover, ch.lyrics = lyrics;
      std::string why = writeTags(p, ch);
      check(why.empty() && !cover.empty(), "a download's tags and cover saved" + (why.empty() ? "" : " (" + why + ")"));
      Tags t = readTags(p.c_str());
      check(t.title == "Air Born" && t.track == 6 && t.year == 1976 && t.albumArtist == "Camel", "they read back");
      check(run("ffprobe -v error -select_streams v -show_entries stream=codec_name:stream_disposition=attached_pic -of csv=p=0 '" + p + "' | grep -q 'mjpeg,1'"),
            "ffprobe sees the cover as its picture");
      check(run("ffprobe -v error -show_entries format_tags=comment,track -of csv=p=0 '" + p + "' | grep -q '6/7'"), "track 6 of 7");
      check(decodes(p), "still plays");
    } else {
      check(false, "song.m4a made by ffmpeg");
    }
  }
  check(!writeTags(dir + "/none.ogg", TagChanges()).empty(), "an Ogg file: refused, not broken");
  std::string cmd = "rm -r '" + dir + "'";
  if (getenv("KEEP")) printf("kept in %s\n", dir.c_str());
  else if (dir.compare(0, 16, "/tmp/rondo-tags-") == 0) (void)!system(cmd.c_str());
  printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures != 0;
}
