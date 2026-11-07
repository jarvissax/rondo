// Songs the server streams, as the player's queue holds them: "/s/<id>/140" paths (1.0.x's
// audioMediaItem: AAC from YouTube Music through the server's range proxy), with what the screens show
// about each (title, subtitle, artwork), kept here since the library knows nothing of them.
#include <map>

#include "../core/api.h"
#include "app.h"
#include "screens.h"

namespace rondo::shell {
namespace {
std::map<std::string, Item>& registry() {
  static std::map<std::string, Item> r;
  return r;
}
}  // namespace

std::string streamPath(const Item& it) { return "/s/" + it.id + "/140"; }

bool isStream(const std::string& path) { return path.compare(0, 3, "/s/") == 0; }

const Item* streamItem(const std::string& path) {
  auto it = registry().find(path);
  return it == registry().end() ? nullptr : &it->second;
}

void playStreams(const std::vector<Item>& items, int start) {
  std::vector<std::string> paths;
  for (const Item& it : items) {
    std::string p = streamPath(it);
    registry()[p] = it;
    paths.push_back(p);
  }
  g_player.playQueue(paths, start);
}

// largeThumb(): YouTube Music thumbnails are URLs like ...=w120-h120-l90-rj: a bigger one for the large
// art views (the server still resizes it to the width asked)
std::string largeThumb(const std::string& thumb, int size) {
  static const std::string prefix = "/thumb?u=";
  if (thumb.compare(0, prefix.size(), prefix) != 0) return thumb;
  std::string enc = thumb.substr(prefix.size());
  // the URL is percent-encoded: "%3Dw120-h120" is "=w120-h120"
  size_t at = enc.find("%3Dw");
  if (at == std::string::npos) return thumb;
  size_t h = enc.find("-h", at);
  if (h == std::string::npos) return thumb;
  size_t end = h + 2;
  while (end < enc.size() && isdigit((unsigned char)enc[end])) end++;
  std::string s = std::to_string(size);
  return prefix + enc.substr(0, at) + "%3Dw" + s + "-h" + s + enc.substr(end);
}

std::string artOf(const std::string& path, int size) {
  if (!isStream(path)) return path;
  const Item* it = streamItem(path);
  return it ? (size > 0 ? largeThumb(it->thumb, size) : it->thumb) : "";
}

// the search comes with the search screen's port
std::unique_ptr<Screen> makeMusicSearch() { return nullptr; }

}  // namespace rondo::shell
