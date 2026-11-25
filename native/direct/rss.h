// Podcasts from RSS feeds (the main menu's Podcasts, shell/rss.h): what its screens ask the Y1's own
// local server, in both modes. The Y1 reads the feeds and streams the episodes from the podcast hosts
// itself, so podcasts work with the Rondo server down, old or not set up; a Rondo server only helps when
// it can (its Podcast Index key for search, and big feeds read for the Y1: server/src/rssFeed.ts).
//
//   GET  /rss/search?q=&country=us   shows from Apple's directory, and Podcast Index's when there's a
//                                    key (the server's, else the Y1's own), duplicates dropped
//   GET  /rss/top?country=us&genre=  Apple's charts for the country (a genre's, or all), 50 shows
//   GET  /rss/feed?url=&private=1&fresh=1&since=   a show's feed read (kept on the Y1 as metadata,
//                                    looked at again after 30 min or with fresh); since: only the
//                                    episodes after that time (unix s). 202 {"pending":true}: still
//                                    being read, asked again in a moment; urls=0: without the
//                                    episodes' links (a long show's page: the file route finds them)
//   GET  /rss/notes?url=&guid=       an episode's show notes, as plain text
//   GET  /rss/file?s=&e=&u=          an episode's file from its host (u: its link; without it, the feed
//                                    of show s kept here names episode e's); Range requests (the
//                                    player's pieces, a download going on), its redirects followed once
//   GET  /rss/art?u=&w=&h=           a show's or an episode's picture
//   GET  /rss/chapters?u=            a Podcasting 2.0 chapters file: {"chapters":[{"ms","title"}]}
//   GET  /rss/id3?u=                 an MP3's own chapters (ID3 CHAP), the same way
//   GET  /rss/key, POST /rss/key {"key","secret"}   the Y1's own Podcast Index key (set, or cleared
//                                    with an empty body); never shown back
#pragma once
#include <string>

#include "routes.h"

namespace rondo::direct::rss {

// The Rondo server connected ("" none), whatever YouTube's source: its /podcasts/ routes are asked
// when it has them (an older one answers 404 and isn't asked again for an hour)
void configure(const std::string& server, const std::string& token);
// A path under /rss/: answered (true)
bool route(const Request& r, Response& out);

}  // namespace rondo::direct::rss
