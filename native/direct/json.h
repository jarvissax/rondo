// Reading YouTube's InnerTube answers (yyjson): paths, texts, endpoints and thumbnails, as youtubei.js
// reads them (its Text, NavigationEndpoint and Thumbnail classes), and writing Rondo's own answers.
#pragma once
#include <yyjson.h>

#include <string>
#include <vector>

namespace rondo::direct {

using J = yyjson_val*;

inline J key(J v, const char* k) { return v && yyjson_is_obj(v) ? yyjson_obj_get(v, k) : nullptr; }
inline J idx(J v, size_t i) { return v && yyjson_is_arr(v) ? yyjson_arr_get(v, i) : nullptr; }
inline J path(J v) { return v; }
template <class... R>
J path(J v, const char* k, R... rest);
template <class... R>
J path(J v, int i, R... rest);
template <class... R>
J path(J v, const char* k, R... rest) {
  return path(key(v, k), rest...);
}
template <class... R>
J path(J v, int i, R... rest) {
  return path(idx(v, size_t(i)), rest...);
}

inline bool isStr(J v) { return v && yyjson_is_str(v); }
inline std::string str(J v) { return isStr(v) ? std::string(yyjson_get_str(v), yyjson_get_len(v)) : std::string(); }
inline size_t count(J a) { return a && yyjson_is_arr(a) ? yyjson_arr_size(a) : 0; }
inline int64_t num(J v, int64_t otherwise = 0) {
  if (!v) return otherwise;
  if (yyjson_is_int(v)) return yyjson_get_sint(v);
  if (yyjson_is_real(v)) return int64_t(yyjson_get_real(v));
  if (yyjson_is_str(v)) return atoll(yyjson_get_str(v));
  return otherwise;
}

template <class F>
void each(J a, F f) {
  if (!a || !yyjson_is_arr(a)) return;
  size_t i, n;
  J x;
  yyjson_arr_foreach(a, i, n, x) f(x);
}

// the name of an object's only (or first) member: a renderer's type ("musicTwoRowItemRenderer")
std::string firstKey(J o);

// Text: {"runs": [{"text": …}]}, {"simpleText": …}, a view model's {"content": …}, or a bare string
std::string text(J t);

// A navigation endpoint as youtubei.js names it: the first member ending in Endpoint or Command
struct Endpoint {
  std::string name;              // "browseEndpoint", "watchEndpoint", "watchPlaylistEndpoint", …
  std::string browseId, videoId, playlistId, params;
  std::string pageType;          // browseEndpointContextMusicConfig.pageType
  std::string musicVideoType;    // watchEndpointMusicConfig.musicVideoType
  J payload = nullptr;
};
Endpoint endpoint(J e);

// A text's runs, each with its own endpoint
struct Run {
  std::string text;
  Endpoint at;
};
std::vector<Run> runs(J t);
// Text.endpoint: navigationEndpoint, titleNavigationEndpoint, else the first run's
Endpoint textEndpoint(J t);

// the widest of a thumbnail list ([{url, width, height}] or {thumbnails: [...]}); "" when none
std::string widest(J thumbs);

// the first object under `name`, depth-first
J find(J v, const char* name, int depth = 24);
// every object under `name` (not looking inside a match)
void findAll(J v, const char* name, std::vector<J>& out, int depth = 24);

// "Album • 2013" -> "Album · 2013"; joined parts with " · "
std::string bullets(const std::string& s);
std::string joinDot(const std::vector<std::string>& parts);
// "3:41", "1:02:03" -> seconds (0: not a duration)
int clockSeconds(const std::string& s);
std::string trim(const std::string& s);

// JSON text of a string (quoted, escaped)
std::string quote(const std::string& s);
// encodeURIComponent
std::string uriComponent(const std::string& s);
std::string uriDecode(const std::string& s);

}  // namespace rondo::direct
