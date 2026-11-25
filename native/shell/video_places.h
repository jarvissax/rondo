#pragma once
// Where a video was left (4.3.0; core/video_place.h has the rules): a card video or a YouTube video 10
// minutes or longer opens where it was left, its row shows how far it got, and the ones started are in
// Continue watching (Library › Videos for the card's, YouTube's Home for YouTube's). A YouTube video the
// Y1 has no place for opens at about YouTube's own progress (the phone's red bar), read from its lists.

#include <cstdint>
#include <string>
#include <vector>

#include "../core/api.h"

namespace rondo::shell {

// a video's place key ("" none: a podcast episode's address, whose place rss.h keeps)
std::string videoPlaceKey(const Item& item);

// where it opens (ms; 0 from the beginning): its own place, else YouTube's progress (watchedPct)
int64_t videoOpensAt(const std::string& key, int64_t lengthMs, int watchedPct);
// its place now: every 10 s while it plays and as it closes (kept only for one 10 minutes or longer)
void videoPlaceSave(const std::string& key, const Item& item, int64_t ms, int64_t lengthMs, bool ended);
// Start again: the place forgotten, so it opens at the beginning
void videoStartAgain(const std::string& key);
bool videoHasPlace(const std::string& key);  // started and not watched to the end

// a row's progress (0–1; -1 none): its own place, else YouTube's (a full bar once watched to the end)
float videoProgress(const std::string& key, int lengthSec, int watchedPct);
// "23 min left" for a started one, else ""
std::string videoLeftText(const std::string& key, int lengthSec);

// Continue watching, the last watched first: the card's videos (a download among them), and the
// YouTube videos (a downloaded one as its file, so it plays without a connection)
std::vector<Item> continueWatchingCard();
std::vector<Item> continueWatchingYouTube();

}  // namespace rondo::shell
