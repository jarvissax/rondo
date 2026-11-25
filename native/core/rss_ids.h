// Podcasts from RSS feeds: the ids a show and an episode are known by on the Y1, the same in the local
// server's answers (direct/rss.h) and in the Podcasts screens (shell/rss.h). A show is its feed's
// address (http and https, a trailing slash and the host's letter case not counting); an episode is its
// show and its guid. Both are 16 hex digits of FNV-1a.
#pragma once
#include <stdint.h>

#include <string>

namespace rondo::rssid {

inline uint64_t fnv64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
  return h;
}

inline std::string hex16(uint64_t v) {
  static const char* d = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; i--, v >>= 4) s[size_t(i)] = d[v & 15];
  return s;
}

// "HTTPS://Example.com/feed/" -> "example.com/feed"
inline std::string normalizeFeed(const std::string& url) {
  std::string u = url;
  size_t a = u.find_first_not_of(" \t\r\n"), b = u.find_last_not_of(" \t\r\n");
  u = a == std::string::npos ? "" : u.substr(a, b - a + 1);
  size_t scheme = u.find("://");
  if (scheme != std::string::npos && scheme < 8) u = u.substr(scheme + 3);
  size_t slash = u.find('/');
  for (size_t i = 0; i < (slash == std::string::npos ? u.size() : slash); i++)
    if (u[i] >= 'A' && u[i] <= 'Z') u[i] = char(u[i] - 'A' + 'a');
  while (!u.empty() && u.back() == '/') u.pop_back();
  return u;
}

inline std::string showId(const std::string& feedUrl) { return hex16(fnv64(normalizeFeed(feedUrl))); }
inline std::string episodeId(const std::string& show, const std::string& guid) { return hex16(fnv64(show + "|" + guid)); }

}  // namespace rondo::rssid
