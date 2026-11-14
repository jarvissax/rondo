// The dashboard's Fill in (Library › Fill in on the dashboard, not on the Y1) and Organize's lookup:
// what the card's songs and videos don't say, looked up online (core/lookup.h) on a thread of its own.
//   - Song details: the genre, year, album and track number missing from songs, by album, from the
//     iTunes Store then MusicBrainz; shown for review, then saved into the files (core/tag_write.h).
//   - Lyrics: LRCLIB's for every song without any, saved into the file (an .lrc beside it when its
//     kind can't hold them).
//   - Shows and movies: a series' years and a movie's year (TVmaze, Wikidata), for Organize's folders.
// One Fill in job at a time (the titles' lookup apart); the Y1 stays awake while one runs.
#pragma once
#include <set>
#include <string>
#include <vector>

#include "../core/lookup.h"
#include "../core/organize.h"

namespace rondo::shell {

struct FillSummary {  // UI thread
  int songs = 0, noGenre = 0, noYear = 0, noAlbum = 0, noTrack = 0;
  int groups = 0;       // the albums (and loose songs) a details lookup asks about
  int unsupported = 0;  // songs of a kind Rondo can't save tags into (their lyrics go beside them)
};
FillSummary fillInSummary();

struct FillProposal {  // what a details lookup found for an album
  int id = 0;
  std::string artist, album;              // as the songs have them
  std::string foundArtist, foundAlbum, source;
  std::string genre, newAlbum;            // what changes ("" none)
  int year = 0;                           // 0: none
  int songs = 0, tracks = 0;              // songs it changes; of them, ones given a track number
};
struct FillStatus {
  std::string kind;  // "" (none yet), "details", "save", "lyrics"
  bool running = false, finished = false, canceled = false;
  int done = 0, total = 0;
  std::string stopped;  // why it ended early ("" when it didn't)
  // details
  std::vector<FillProposal> proposals;
  int notFound = 0, failed = 0;
  // save and lyrics
  int saved = 0, found = 0, synced = 0, had = 0, instrumental = 0, beside = 0;
  std::vector<std::string> errors;  // "<file>: <why>"
};
FillStatus fillInStatus();
// "" when started, else why not. countries: the iTunes Stores to ask besides the songs' own ("us").
std::string fillInDetails(const std::vector<std::string>& countries);
std::string fillInLyrics();
std::string fillInSave(const std::set<int>& skip);  // the details found, but those skipped
void fillInCancel();
bool fillInActive();  // a job running: the CPU stays up (power_screens.cpp)
// saving into the songs (details or lyrics): Organize waits (a file it moved mid-save would come back)
bool fillInWriting();

// Organize's lookup: the card's series and movies whose years the names don't say (a series still
// running is asked again: it may have ended)
std::vector<VideoTitle> titlesToLookUp();  // UI thread
struct TitleMatch {
  VideoTitle title;
  std::vector<lookup::Title> found;
  int pick = -1;  // the one taken unless the user picks another (-1: none matches by name)
  bool failed = false;
};
struct TitlesStatus {
  bool running = false, finished = false;
  int done = 0, total = 0;
  std::vector<TitleMatch> matches;
};
std::string titlesLookupStart();  // "" when started
TitlesStatus titlesStatus();

}  // namespace rondo::shell
