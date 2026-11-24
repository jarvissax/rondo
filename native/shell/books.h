// Audiobooks playing (Library › Audiobooks; core/books.h has the rules): a book plays as its own queue
// at its own speed, and the music queue waits (queue.txt keeps it) until the book is left or finished.
// Where each book is is kept (every 10 s while it plays, on a pause, the screen going off, the end),
// a resume starts a little earlier the longer the pause was, and Next and Previous move by chapter.
// The sleep timer (for music too) is here: a deadline or the end of what plays, a 10 s fade, a pause.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "../core/books.h"
#include "../core/library.h"

namespace rondo::shell {

// A book as it plays: its files, their lengths, its chapters
struct BookPlay {
  std::string key, title, author, narrator;
  std::vector<std::string> paths;
  std::vector<int64_t> fileMs;
  std::vector<Chapter> chapters;
  int64_t lengthMs = 0;
  bool skips = false;  // no chapters: Next and Previous skip 30 s and 15 s
};
BookPlay bookDetails(const Book& b);
// a chapter's name: its own, the file's, or "Chapter 3"
std::string chapterName(const BookPlay& b, int chapter);

// Where a book is, from what's kept (or the player, for the one playing)
struct BookProgress {
  Place place;
  int64_t ms = 0;  // from the book's start
  bool started = false, finished = false;
  int speed = 100;
  int64_t lastPlayed = 0;  // ms since 1970
};
BookProgress bookProgress(const Book& b, const BookPlay& d);
int64_t wallNowMs();

// What the screens say: listening time at a speed ("2 h 10 min", "45 min", "under a minute"), and
// a speed in the language's numbers ("1.5×", "1,5×")
std::string listenTime(int64_t ms, int speed);
std::string speedLabel(int percent);

// The book playing (its files are the player's queue), null while anything else plays
const BookPlay* bookNow();
Place bookPlace();  // where in it now (bookNow() only)
int bookSpeedNow();

// Resume (from where it was, a little earlier after a pause), or Start for a book never started
void playBook(const Book& b);
// from a place: Start again ({0, 0}), a chapter, a bookmark
void playBookFrom(const Book& b, Place p, bool play = true);
void playBookFile(const Book& b, const std::string& path);  // a book's file opened in Files
void setBookSpeed(const Book& b, int percent);
void markBookFinished(const Book& b, bool finished);
// a bookmark where the book playing is (false: no book plays)
bool addBookmarkNow();
// the music queue waits for the book (Back to music brings it back, playing)
bool musicLent();
void backToMusic();

// What the controls do (the wheel's buttons, a headset's, Bluetooth's, the dashboard's): a book's
// chapters and its resume rewind, else the player's own
void transportToggle();
void transportPlay();
void transportNext();
void transportPrev();
void transportHold(int dir);  // a held Next/Prev outside Now Playing: 30 s in a book

// The loop's
void booksStart();  // the library opened: the player's hooks, the book restored at startup completed
bool booksTick(int64_t now);  // true: something shown changed
int64_t booksWakeAt();
void booksSaveNow();  // a pause, the screen off, Rondo ending
// At startup (resume.cpp): a book that was playing when Rondo stopped comes back, the music queue
// staying in queue.txt. Waiting: its card isn't mounted yet (asked again when it is).
enum class BookResume { None, Restored, Waiting };
BookResume booksRestore();
bool booksOwnQueue();  // the player's queue is a book's: resume.cpp leaves queue.txt to the music
// files moved while a book plays (the dashboard): its queue follows them (true: it was the book's)
bool booksQueueMoved(const std::vector<std::string>& queue);
// Play next / Add to queue while a book plays: into the music queue that waits (true: taken)
bool booksTakeQueueAdd(const std::string& path, bool next);

// ---- the sleep timer (Now Playing's options) ----
enum class SleepMode { Off, Minutes, EndOfItem };  // EndOfItem: the chapter (a book) or the song
void sleepStart(SleepMode mode, int minutes = 0);
SleepMode sleepMode();
int64_t sleepLeftMs();  // Minutes: what's left
std::string sleepMark();  // what Now Playing shows beside its moon ("28 min", "End"); "" off
std::string sleepEndName();  // "End of chapter", "End of book" or "End of song", for what plays

}  // namespace rondo::shell
