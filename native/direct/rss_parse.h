// Podcast RSS feeds read as they arrive: the channel and its episodes by the rules the Rondo server's
// parser follows too (server/src/rssFeed.ts), from bytes fed in chunks cut anywhere, in flat memory and
// without an XML library (only what RSS needs is read). Host-tested in native/tests/host/rss_test.cpp.
#pragma once
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>

namespace rondo::direct::rss {

struct Episode {
  std::string guid, title, url, type, image, chapters, episodeType = "full", notes;
  int64_t date = 0;  // unix seconds, 0 unknown
  int64_t size = 0;  // the enclosure's length in bytes, 0 unknown
  int duration = 0;  // seconds, 0 unknown
  int season = 0, number = 0;  // itunes:season, itunes:episode (0 none)
};
struct Channel {
  std::string title, author, description, art, link, language, newFeedUrl;
  bool serial = false, explicitContent = false, complete = false;
};

class Parser {
 public:
  // each episode as soon as its </item> is read, in the feed's order
  explicit Parser(std::function<void(Episode&&)> onEpisode);
  ~Parser();
  // the next bytes, cut anywhere (mid-tag, mid-entity, mid-UTF-8 character). false once it's clear this
  // isn't an RSS feed (not XML, an HTML page, an Atom feed): the caller stops sending
  bool feed(const char* data, size_t n);
  // the end: false when it wasn't an RSS feed with a <channel>; the channel's fields (some feeds put them
  // after the items) are complete now
  bool finish(Channel& out);
  int count() const;  // episodes delivered so far
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// HTML (or plain text) as plain text by the spec's rules, at most maxBytes (cut on a UTF-8 boundary, "…" added when cut); oneLine: line breaks become spaces
std::string plainText(const std::string& html, size_t maxBytes, bool oneLine = false);
int64_t parseDate(const std::string& s);   // pubDate: unix seconds, 0 when it can't be read
int parseDuration(const std::string& s);   // itunes:duration: seconds, 0 unknown
std::string guessType(const std::string& url);  // from the path's extension ("" unknown)

}  // namespace rondo::direct::rss
