// Subtitles: cues read from WebVTT, SubRip and SubStation (ASS/SSA) files; the tracks a video has (a
// card video's files beside it, a YouTube video's from /captions); and the cues drawn over the picture
// in the look Settings › Display › Subtitles sets, YouTube's own by default (white Roboto on a 75 %
// black box under each line).
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

#include "../core/subtitle_file.h"
#include "kit.h"

namespace rondo::shell {


struct SubTrack {
  std::string id, lang, name;  // name: what the panel calls it ("English", "Azerbaijani (auto-generated)")
  bool autoGen = false;        // YouTube's speech recognition (or a file marked .auto)
  std::string file;            // a card video's: the file ("": YouTube's, /captions/<video>/<id>)
};
// The subtitle files beside a video on the card: "<name>.srt", "<name>.en.vtt", "<name>.en.forced.ass"…
// (how Jellyfin and Plex name them, and how YouTube › Download saves them)
std::vector<SubTrack> subtitleFiles(const std::string& videoPath);
// The track to show when subtitles are on: the language picked last (the video's own track before the
// speech recognition's), else the video's own first, else the first; -1 when there are none
int pickTrack(const std::vector<SubTrack>& tracks);

// The look's choices (prefs.h keeps the index of each): the same names on the Y1 and the dashboard
struct SubColor {
  const char* name;
  uint32_t rgb;
};
extern const SubColor kSubColors[8];  // White, Yellow, Green, Cyan, Blue, Magenta, Red, Black
extern const char* const kSubSizes[4];  // Small, Normal, Large, Extra large
extern const char* const kSubEdges[5];  // None, Drop shadow, Raised, Depressed, Outline
void resetSubtitleStyle();              // YouTube's own look

// Which cues show at ms (a key that changes when they do: draw again), and when that next changes
uint64_t cuesAt(const std::vector<Cue>& cues, int64_t ms, int64_t* nextChange = nullptr);
// The cues showing at ms, centered in w with the last line's box ending at `bottom`
void drawSubtitles(Env& e, const std::vector<Cue>& cues, int64_t ms, float w, float bottom);
// A sample line in the look as it is now (Settings › Display › Subtitles' preview)
void drawSubtitleSample(Env& e, const std::string& text, float w, float bottom);

}  // namespace rondo::shell
