// Direct mode's YouTube Music podcasts: what server/src/podcasts.ts answers, from WEB_REMIX's own JSON
// (and WEB's /next for an episode's chapter markers), read the same way. Each returns the route's JSON,
// or "" when YouTube couldn't be asked. An episode is known by its musicVideoType, a show by its page
// type; YouTube's own words (a date, "3 hr 21 min") are passed on to be shown, never read.
#pragma once
#include <string>

#include "json.h"
#include "model.h"

namespace rondo::direct::podcasts {

std::string library();                   // /music/podcasts: {items (shows), later, latest}
std::string list(const std::string& which);  // /music/podcasts/later (SE), /new (RDPN): {items, next}
std::string show(const std::string& id);     // /music/podcast/:id: {title, subtitle, thumb, description, saved, items, next}
std::string episode(const std::string& id);  // /music/episode/:id: {id, title, show, showId, …, chapters}
std::string search(const std::string& q, const std::string& type);  // type podcast or episode: {items, next}
// the account's: Save to library, Episodes for Later, Mark as played (false: YouTube said no)
bool save(const std::string& id, bool saved);
bool later(const std::string& id, bool on);
bool played(const std::string& id, bool on);

// For music.cpp's home shelves (?podcasts=1): a card or row that is a show or an episode, as an Item
// of kind "podcast" or "episode" (not ok() when it is neither)
Item homeItem(J entry);
bool isShowId(const std::string& id);  // MPSP…

}  // namespace rondo::direct::podcasts
