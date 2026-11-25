#pragma once
// Where a video was left (4.3.0): a video 10 minutes or longer, from the card or YouTube, opens where it
// was left. Its place is a BookState (library.h) like an audiobook's or an episode's: the card video's
// path is its key, a YouTube video's "ytv:<id>", and a download named "Title [<id>].mp4" shares its
// YouTube video's key. Shorts and short clips start over, as an iPod's music videos did.

#include <cctype>
#include <cstdint>
#include <string>

#include "books.h"

namespace rondo {

constexpr int64_t kVideoPlaceMinMs = 10 * 60 * 1000;
// past this much of YouTube's own progress (the phone's red bar), a video starts from the beginning
constexpr int kVideoWatchedPct = 95;

inline bool videoKeepsPlace(int64_t lengthMs) { return lengthMs >= kVideoPlaceMinMs; }

inline bool youtubeIdChar(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_'; }

// The YouTube id a downloaded video's name ends with ("Title [dQw4w9WgXcQ].mp4"), "" none
inline std::string youtubeIdInName(const std::string& path) {
  size_t slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = name.rfind('.');
  if (dot != std::string::npos) name.resize(dot);
  if (name.size() < 13 || name.back() != ']' || name[name.size() - 13] != '[') return "";
  std::string id = name.substr(name.size() - 12, 11);
  for (char c : id)
    if (!youtubeIdChar(c)) return "";
  return id;
}

inline std::string youtubeVideoKey(const std::string& id) { return "ytv:" + id; }

// A video's place key: a card file's path (or its YouTube id's key, for a download), a YouTube id's
inline std::string videoKeyFor(const std::string& idOrPath) {
  if (idOrPath.empty()) return "";
  if (idOrPath[0] == '/') {
    std::string yt = youtubeIdInName(idOrPath);
    return yt.empty() ? idOrPath : youtubeVideoKey(yt);
  }
  if (idOrPath.compare(0, 4, "http") == 0) return "";  // an episode's address: rss.h keeps those
  return youtubeVideoKey(idOrPath);
}

// Where a video opens (ms): its own place, a little earlier after a pause (rewindAfterPause); with none,
// at YouTube's own progress (percent of lengthMs, under kVideoWatchedPct); else, or once it was watched
// to the end, from the beginning. Short videos always start at 0.
inline int64_t videoStartMs(bool hasPlace, int64_t placeMs, bool finished, int64_t pausedForMs, int64_t lengthMs,
                            int youtubePct) {
  if (lengthMs > 0 && !videoKeepsPlace(lengthMs)) return 0;
  if (hasPlace) {
    if (finished || placeMs <= 0) return 0;
    int64_t at = placeMs - rewindAfterPause(pausedForMs);
    if (lengthMs > 0 && at >= lengthMs - kFinishedWithinMs) return 0;
    return at > 0 ? at : 0;
  }
  if (youtubePct <= 0 || youtubePct >= kVideoWatchedPct || lengthMs <= 0) return 0;
  return lengthMs * youtubePct / 100;
}

}  // namespace rondo
