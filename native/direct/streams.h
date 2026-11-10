// Direct mode's streams: a video id resolved into googlevideo links by the VISIONOS client (no player
// JavaScript and no PO token, Nov 2026), and the server's /s range proxy (server/src/stream.ts) over
// them, so the player reads /s/<id>/<itag> as it does from a Rondo server. Links are tied to this Y1's
// address, which is the one that asked for them; nothing is stored but the links themselves.
#pragma once
#include <stdint.h>

#include <string>

#include "routes.h"

namespace rondo::direct {

struct VideoDetails {
  std::string title, author;
  int64_t views = -1;
  int seconds = 0;
};
// a video's title, channel, views and length (the player request's videoDetails)
bool details(const std::string& id, VideoDetails& out);
// resolves the video in the background: its streams are about to be asked for
void prepare(const std::string& id);
// the last resolve's outcome, for /healthz: -1 none yet, 0 failed, 1 worked
int lastResolveOk();

void streamRoute(const Request& r, Response& out);   // /s/:id/:itag
void mpdRoute(const Request& r, Response& out);      // /mpd/:id: resolved (a warm-up): 200, or 404 when unplayable
void resolveRoute(const Request& r, Response& out);  // /resolve/:id
void thumbRoute(const Request& r, Response& out);    // /thumb?u=&w=&h=
// a picture from Google's image hosts, from memory when fetched before (thumbRoute)
bool thumbBytes(const std::string& want, const std::string& original, std::string& type, std::string& body);

}  // namespace rondo::direct
