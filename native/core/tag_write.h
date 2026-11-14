// Saving tags into a song's file (the dashboard's Fill in, shell/fill_in.h): FLAC's Vorbis comments,
// MP3's ID3v2 (2.3 or 2.4, as the file has it; a file without one gets 2.3) and MP4/M4A's ilst. Only
// the fields given change; the rest of the file's tags (its picture, ReplayGain, anything else) stay
// as they are. In place when the tags fit where they were (the padding FLAC and MP3 files keep for
// this); else the file is written again beside itself, with room to spare, and put in its place.
#pragma once
#include <string>

namespace rondo {

struct TagChanges {
  std::string title, artist, albumArtist, album, genre;  // "" leaves a field as it is
  std::string lyrics;  // LRC (timed) or plain lines: FLAC's LYRICS, MP3's USLT, MP4's ©lyr
  std::string comment;  // MP4's ©cmt (a YouTube Music download's link)
  std::string cover;    // a JPEG or PNG picture: MP4's covr (FLAC and MP3 keep theirs)
  int year = 0, track = 0, disc = 0;  // 0 leaves it
  int trackTotal = 0;                 // with track: "6 of 7" (MP4's trkn, FLAC's TRACKTOTAL)
};

// "" when saved, else why not ("Rondo can't save tags into .ogg files yet", "not enough space")
std::string writeTags(const std::string& path, const TagChanges& changes);
// whether writeTags knows the file's kind (by its extension)
bool canWriteTags(const std::string& path);

}  // namespace rondo
