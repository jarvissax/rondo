// Tags and durations read straight from the files (FLAC, MP4/M4A, MP3 with ID3v2, Ogg Vorbis/Opus,
// WAV), the fields today's Rondo takes from MediaMetadataRetriever.
#pragma once
#include <string>

namespace rondo {

struct Tags {
  std::string title, artist, albumArtist, album, genre;
  int track = 0;
  int disc = 0;  // 0: not set
  int year = 0;  // 0: not set
  int durationSec = -1;  // -1: unknown
  bool read = false;     // the file was understood
};

Tags readTags(const char* path);

}  // namespace rondo
