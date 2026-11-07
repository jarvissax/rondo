// Lyrics of a song on the card (LyricsSource.kt and EmbeddedLyrics.kt in 1.0.x): an .lrc file next to it
// or the lyrics in its tags, offline only. Timed (LRC) lyrics are preferred wherever they are.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace rondo {

struct LyricLine {
  int64_t t = 0;  // ms
  std::string text;
};

struct Lyrics {
  std::vector<LyricLine> synced;  // timed lines, or empty
  std::string plain;              // the text, when there are no timed lines
  bool instrumental = false;
};

// LyricsSource.fromFile(): the .lrc next to the file, then the tags; empty when there are none.
Lyrics lyricsForFile(const std::string& path);
// [mm:ss], [mm:ss.xx], [mm:ss.xxx], several stamps per line; [offset:+/-ms] applied; sorted by time.
std::vector<LyricLine> parseLrc(const std::string& text);
// FLAC Vorbis comments (LYRICS, UNSYNCEDLYRICS, SYNCEDLYRICS, LYRICS-*), MP4 ©lyr, ID3v2 USLT; trimmed,
// "" when none.
std::string embeddedLyrics(const std::string& path);
// The server's /lyrics answer: {synced: [{t, text}], plain, instrumental, source} (LRCLIB first, then
// YouTube Music's own).
bool parseLyrics(const std::string& json, Lyrics& out);

}  // namespace rondo
