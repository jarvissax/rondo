// core/books.h on the Mac: what's a book, grouping, chapters, Next/Previous, smart rewind, speeds.
//   clang++ -std=c++17 -I native native/core/books.cpp native/core/fold.cpp native/tests/host/books_test.cpp
#include <stdio.h>

#include <string>
#include <vector>

#include "core/books.h"

using namespace rondo;

static int failures = 0;
#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                    \
    }                                                                \
  } while (0)

static BookFile file(const std::string& path, const std::string& album = "", int track = 0, int64_t ms = 60000, int disc = 0) {
  BookFile f;
  f.path = path, f.album = album, f.track = track, f.durationMs = ms, f.disc = disc;
  size_t slash = path.rfind('/'), dot = path.rfind('.');
  f.title = path.substr(slash + 1, dot - slash - 1);
  return f;
}

int main() {
  // ---- what's a book ----
  CHECK(inBookFolder("/storage/sdcard0/Audiobooks/Dune/01.mp3"));
  CHECK(inBookFolder("/storage/sdcard1/audiobooks/x.mp3"));
  CHECK(inBookFolder("/storage/sdcard0/AUDIOBOOKS/a/b/c.m4a"));
  CHECK(inBookFolder("/storage/sdcard0/Audio Books/x.mp3"));
  CHECK(inBookFolder("/storage/sdcard0/Audiolibros/x.mp3"));
  CHECK(!inBookFolder("/storage/sdcard0/Music/Audiobooks/x.mp3"));  // only at the root
  CHECK(!inBookFolder("/storage/sdcard0/Audiobooks.mp3"));
  CHECK(!inBookFolder("/storage/sdcard0/Music/x.mp3"));
  CHECK(bookRoot("/storage/sdcard0/audioBooks/x/y.mp3") == "/storage/sdcard0/audioBooks");
  CHECK(isM4b("/storage/sdcard0/Music/Book.M4B"));
  CHECK(!isM4b("/storage/sdcard0/Music/Song.m4a"));
  CHECK(isBookGenre("Audiobook") && isBookGenre("audiobooks") && isBookGenre("Audio Book") && isBookGenre("Audiolibro"));
  CHECK(!isBookGenre("Rock") && !isBookGenre("") && !isBookGenre("Spoken Word"));

  // ---- disc folders ----
  CHECK(isDiscFolder("CD 1") && isDiscFolder("CD1") && isDiscFolder("Disc 02") && isDiscFolder("disk-3"));
  CHECK(isDiscFolder("Dune (Disc 1)") && isDiscFolder("Part 4"));
  CHECK(!isDiscFolder("Dune") && !isDiscFolder("Abcd1") && !isDiscFolder("1984"));

  // ---- which book a file is in ----
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Dune/01.mp3")) == "/storage/sdcard0/Audiobooks/Dune");
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Herbert/Dune/CD 2/01.mp3")) == "/storage/sdcard0/Audiobooks/Herbert/Dune");
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Dune/CD1/01.mp3")) == "/storage/sdcard0/Audiobooks/Dune");
  // disc folders right in Audiobooks: one book of their name
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Dune (Disc 1)/01.mp3")) == "/storage/sdcard0/Audiobooks/Dune");
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Dune (Disc 2)/01.mp3")) == "/storage/sdcard0/Audiobooks/Dune");
  // a lone disc folder in Audiobooks stays a book of its own
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/CD 1/01.mp3")) == "/storage/sdcard0/Audiobooks/CD 1");
  // a file right in Audiobooks, an .m4b, a file with chapters: each a book
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Lecture.mp3")) == "/storage/sdcard0/Audiobooks/Lecture.mp3");
  CHECK(bookKeyOf(file("/storage/sdcard0/Audiobooks/Author/A.m4b")) == "/storage/sdcard0/Audiobooks/Author/A.m4b");
  BookFile ch = file("/storage/sdcard0/Audiobooks/Author/B.mp3");
  ch.ownChapters = true;
  CHECK(bookKeyOf(ch) == ch.path);
  CHECK(bookKeyOf(file("/storage/sdcard0/Music/Book/01.mp3")) == "/storage/sdcard0/Music/Book");  // by genre

  // ---- order ----
  CHECK(naturalLess("Chapter 2", "Chapter 10"));
  CHECK(!naturalLess("Chapter 10", "Chapter 2"));
  CHECK(naturalLess("01.mp3", "2.mp3"));
  CHECK(naturalLess("a", "B"));
  {
    std::vector<BookFile> v = {file("/b/Book/Chapter 10.mp3"), file("/b/Book/Chapter 2.mp3"), file("/b/Book/Chapter 1.mp3")};
    orderBookFiles(v);
    CHECK(v[0].path == "/b/Book/Chapter 1.mp3" && v[1].path == "/b/Book/Chapter 2.mp3" && v[2].path == "/b/Book/Chapter 10.mp3");
    std::vector<BookFile> t = {file("/b/B/x.mp3", "", 3), file("/b/B/y.mp3", "", 1), file("/b/B/z.mp3", "", 2, 1, 1),
                               file("/b/B/w.mp3", "", 1, 1, 2)};
    orderBookFiles(t);
    CHECK(t[0].path == "/b/B/y.mp3" && t[1].path == "/b/B/x.mp3" && t[2].path == "/b/B/z.mp3" && t[3].path == "/b/B/w.mp3");
  }

  // ---- grouping ----
  {
    std::vector<BookFile> v = {
        file("/storage/sdcard0/Audiobooks/Zed/02.mp3", "Zed Book", 2),
        file("/storage/sdcard0/Audiobooks/Zed/01.mp3", "Zed Book", 1),
        file("/storage/sdcard0/Audiobooks/Frank Herbert - Dune/01.mp3"),
        file("/storage/sdcard0/Audiobooks/Alpha.m4b", "Alpha"),
    };
    v[0].artist = v[1].artist = "Someone";
    std::vector<BookGroup> g = groupBooks(v);
    CHECK(g.size() == 3);
    CHECK(g[0].title == "Alpha");
    CHECK(g[1].title == "Dune" && g[1].author == "Frank Herbert");  // from the folder's name
    CHECK(g[2].title == "Zed Book" && g[2].author == "Someone" && g[2].files.size() == 2);
    CHECK(g[2].files[0].path.find("01.mp3") != std::string::npos);
    CHECK(g[2].durationMs == 120000);
    // a single file without an album: its title tag
    std::vector<BookFile> one = {file("/storage/sdcard0/Audiobooks/Long talk.mp3")};
    one[0].title = "A Long Talk";
    CHECK(groupBooks(one)[0].title == "A Long Talk");
  }

  // ---- chapters ----
  {
    // an .m4b with five chapters
    std::vector<FileChapter> own = {{0, 60000, "One"}, {60000, 120000, "Two"}, {120000, 200000, "Three"}, {200000, 260000, "Four"},
                                    {260000, 300000, "Five"}};
    std::vector<Chapter> c = buildChapters({300000}, {own}, {"Book"});
    CHECK(c.size() == 5 && c[2].title == "Three" && c[2].startMs == 120000 && c[4].endMs == 300000);
    CHECK(!skipsTime(c));
    std::vector<int64_t> ms = {300000};
    CHECK(chapterAt(c, {0, 0}) == 0 && chapterAt(c, {0, 130000}) == 2 && chapterAt(c, {0, 299000}) == 4);
    // Next: the next chapter's start; the last chapter has none
    CHECK(nextPlace(c, ms, {0, 30000}) == (Place{0, 60000}));
    CHECK(nextPlace(c, ms, {0, 270000}).file == -1);
    // Previous: past 3 s, this chapter's start; within 3 s, the chapter before; the first stays
    CHECK(previousPlace(c, ms, {0, 125000}) == (Place{0, 120000}));
    CHECK(previousPlace(c, ms, {0, 122000}) == (Place{0, 60000}));
    CHECK(previousPlace(c, ms, {0, 1000}) == (Place{0, 0}));
    // chapters past the file's end and empty ones are dropped; the last ends with the file
    std::vector<Chapter> d = buildChapters({100000}, {{{0, 0, "A"}, {0, 50000, "B"}, {50000, 90000, "C"}, {150000, 160000, "D"}}}, {""});
    CHECK(d.size() == 2 && d[0].title == "B" && d[1].endMs == 100000);
  }
  {
    // a folder book: each file a chapter, one file with chapters of its own
    std::vector<int64_t> ms = {60000, 90000, 30000};
    std::vector<Chapter> c = buildChapters(ms, {{}, {{0, 45000, "2a"}, {45000, 90000, "2b"}}, {}}, {"One", "Two", "Three"});
    CHECK(c.size() == 4);
    CHECK(c[0].title == "One" && c[1].title == "2a" && c[2].title == "2b" && c[3].title == "Three");
    CHECK(c[2].bookStartMs == 105000 && c[3].bookStartMs == 150000 && c[3].file == 2);
    CHECK(nextPlace(c, ms, {0, 10000}) == (Place{1, 0}));
    CHECK(nextPlace(c, ms, {1, 50000}) == (Place{2, 0}));
    CHECK(previousPlace(c, ms, {2, 2000}) == (Place{1, 45000}));
    CHECK(previousPlace(c, ms, {1, 1000}) == (Place{0, 0}));
    // places and book time
    CHECK(bookMsOf(ms, {1, 1000}) == 61000);
    CHECK(placeAt(ms, 61000) == (Place{1, 1000}));
    CHECK(placeAt(ms, 0) == (Place{0, 0}));
    CHECK(placeAt(ms, 500000) == (Place{2, 30000}));  // past the end: the end
  }
  {
    // one long file without chapters: Next and Previous skip time
    std::vector<int64_t> ms = {3600000};
    std::vector<Chapter> c = buildChapters(ms, {{}}, {"Talk"});
    CHECK(c.size() == 1 && skipsTime(c));
    CHECK(nextPlace(c, ms, {0, 100000}) == (Place{0, 130000}));
    CHECK(nextPlace(c, ms, {0, 3590000}) == (Place{0, 3599000}));  // not past the end
    CHECK(previousPlace(c, ms, {0, 100000}) == (Place{0, 85000}));
    CHECK(previousPlace(c, ms, {0, 5000}) == (Place{0, 0}));
  }
  // chapters kept as text and read back
  {
    std::vector<FileChapter> c = {{0, 1000, "A\tB"}, {1000, 2500, "Ç"}};
    std::vector<FileChapter> d = decodeChapters(encodeChapters(c));
    CHECK(d.size() == 2 && d[0].title == "A B" && d[1].startMs == 1000 && d[1].endMs == 2500 && d[1].title == "Ç");
  }

  // ---- smart rewind, finished ----
  CHECK(rewindAfterPause(500) == 0);
  CHECK(rewindAfterPause(5000) == 2000);
  CHECK(rewindAfterPause(20 * 60000) == 5000);
  CHECK(rewindAfterPause(2 * 3600000) == 10000);
  CHECK(rewindAfterPause(30 * 3600000LL) == 30000);
  CHECK(finishedAt(3570001, 3600000) && finishedAt(3600000, 3600000) && !finishedAt(3500000, 3600000) && !finishedAt(0, 0));

  // ---- speeds ----
  CHECK(speedText(150) == "1.5\xc3\x97" && speedText(100) == "1\xc3\x97" && speedText(75) == "0.75\xc3\x97" &&
        speedText(125) == "1.25\xc3\x97" && speedText(300) == "3\xc3\x97");
  CHECK(clampSpeed(160) == 150 && clampSpeed(10) == 75 && clampSpeed(1000) == 300);

  if (failures) fprintf(stderr, "%d failed\n", failures);
  else printf("books: all passed\n");
  return failures ? 1 : 0;
}
