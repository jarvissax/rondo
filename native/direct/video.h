// Direct mode's YouTube: what server/src/browse.ts and tv.ts answer for the YouTube screens, from
// YouTube's own JSON. Without an account (direct mode signs in later): the home is the TV app's topic
// pages, as the server's signed-out home is. Each returns the route's JSON, or "" on failure.
#pragma once
#include <string>

namespace rondo::direct::video {

std::string home();                          // /video/home: {items}
std::string search(const std::string& q);    // /video/search: {items, next}
std::string shorts();                        // /video/shorts: {items, next}
std::string shortInfo(const std::string& id);  // /video/short/:id: {id, title, channel, subtitle, durationSec}

}  // namespace rondo::direct::video
