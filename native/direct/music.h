// Direct mode's YouTube Music: what server/src/browse.ts answers, from WEB_REMIX's own JSON, read the
// way youtubei.js 18.3 reads it (MusicTwoRowItem, MusicResponsiveListItem, PlaylistPanelVideo, the
// page headers). Each returns the route's JSON, or "" when YouTube couldn't be asked.
#pragma once
#include <string>

#include "model.h"

namespace rondo::direct::music {

std::string home();                                            // /music/home: {sections, next}
std::string search(const std::string& q, const std::string& type);  // /music/search: {items, next}
std::string playlist(const std::string& id);                   // /music/playlist/:id: {title, subtitle, thumb, items, next}
std::string album(const std::string& id);                      // /music/album/:id
std::string artist(const std::string& id);                     // /music/artist/:id: {title, thumb, sections}
std::string next(const std::string& videoId);                  // /music/next/:id: {items}
std::string library();  // /music/library, signed in: the account's playlists, Liked music first: {playlists}

// A song's lyrics as YouTube Music has them (timed when it can): {synced, plain} JSON members, or ""
struct Lyrics {
  std::string plain;
  std::vector<std::pair<int, std::string>> synced;  // ms, line ("" a pause); empty: not timed
  bool partial = false;
};
bool lyrics(const std::string& videoId, Lyrics& out);

}  // namespace rondo::direct::music
