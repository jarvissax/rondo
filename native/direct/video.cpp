// video.h. The comments name the tv.ts and browse.ts functions each part follows.
#include "video.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <thread>

#include "innertube.h"
#include "model.h"
#include "streams.h"

namespace rondo::direct::video {
namespace {

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
bool ends(const std::string& s, const char* p) {
  size_t l = strlen(p);
  return s.size() >= l && s.compare(s.size() - l, l, p) == 0;
}

std::string pageJson(const Page<Item>& p) {
  std::string s = "{\"items\":" + json(p.items);
  if (!p.next.empty()) s += ",\"next\":" + quote(p.next);
  return s + "}";
}

std::function<std::string(const Item&)> byId() {
  return [](const Item& i) { return i.id; };
}

// --- the TV app's pages (tv.ts) ---

// tileLines: the channel line and the views · age line, bullets dropped
std::vector<std::string> tileLines(J meta) {
  std::vector<std::string> out;
  each(key(meta, "lines"), [&](J l) {
    std::vector<std::string> parts;
    each(path(l, "lineRenderer", "items"), [&](J i) {
      std::string t = trim(text(path(i, "lineItemRenderer", "text")));
      if (!t.empty() && t != "\xe2\x80\xa2") parts.push_back(t);
    });
    std::string line = joinDot(parts);
    if (!line.empty()) out.push_back(line);
  });
  return out;
}

// fromTile
Item fromTile(J t) {
  std::string type = str(key(t, "contentType"));
  J header = path(t, "header", "tileHeaderRenderer");
  J meta = path(t, "metadata", "tileMetadataRenderer");
  J menu = path(t, "onLongPressCommand", "showMenuCommand");
  std::string title = trim(text(key(meta, "title")));
  if (title.empty()) title = trim(text(key(menu, "title")));
  std::vector<std::string> lines;
  if (meta) lines = tileLines(meta);
  else {
    std::string sub = text(key(menu, "subtitle"));
    size_t b = sub.find("\xe2\x80\xa2");
    std::string first = trim(sub.substr(0, b));
    if (!first.empty()) lines.push_back(first);
  }
  std::string thumb = thumbLink(widest(path(header, "thumbnail")));
  Item i;
  if (ends(type, "_PLAYLIST")) {
    std::string raw = str(path(t, "onSelectCommand", "browseEndpoint", "browseId"));
    if (raw.empty()) raw = str(key(t, "contentId"));
    if (raw.empty()) return {};
    i.kind = "playlist", i.id = starts(raw, "VL") && raw.size() > 3 ? raw.substr(2) : raw, i.title = title, i.subtitle = joinDot(lines), i.thumb = thumb;
    return i;
  }
  if (!ends(type, "_VIDEO") || path(t, "onSelectCommand", "reelWatchEndpoint")) return {};  // shorts, channels
  std::string id = str(path(t, "onSelectCommand", "watchEndpoint", "videoId"));
  if (id.empty()) id = str(key(t, "contentId"));
  if (!isVideoId(id)) return {};
  int seconds = 0;
  each(key(header, "thumbnailOverlays"), [&](J o) {
    if (!seconds) seconds = clockSeconds(text(path(o, "thumbnailOverlayTimeStatusRenderer", "text")));
  });
  if (!seconds) return {};  // live streams, premieres and promos carry no duration
  if (lines.size() > 2) lines.resize(2);
  i.kind = "video", i.id = id, i.title = title, i.subtitle = joinDot(lines), i.durationSec = seconds, i.thumb = thumb;
  return i;
}

// badgeTexts: a lockup's thumbnail badges ("2:44", "LIVE", "Mix")
void badges(J o, std::vector<std::string>& out, int depth = 0) {
  if (!o || depth > 20) return;
  if (yyjson_is_obj(o)) {
    if (J b = key(o, "thumbnailBadgeViewModel"))
      if (isStr(key(b, "text"))) out.push_back(str(key(b, "text")));
    size_t i, n;
    J k, v;
    yyjson_obj_foreach(o, i, n, k, v) badges(v, out, depth + 1);
  } else if (yyjson_is_arr(o)) {
    each(o, [&](J v) { badges(v, out, depth + 1); });
  }
}

bool mentions(J o, const char* name, int depth = 0) {
  if (!o || depth > 24) return false;
  if (yyjson_is_obj(o)) {
    if (key(o, name)) return true;
    size_t i, n;
    J k, v;
    yyjson_obj_foreach(o, i, n, k, v) if (mentions(v, name, depth + 1)) return true;
  } else if (yyjson_is_arr(o)) {
    size_t i, n;
    J v;
    yyjson_arr_foreach(o, i, n, v) if (mentions(v, name, depth + 1)) return true;
  }
  return false;
}

// fromLockup (also browse.ts's lockupItem: the same view model on the web)
Item fromLockup(J l) {
  std::string type = str(key(l, "contentType"));
  if (mentions(l, "reelWatchEndpoint")) return {};  // shorts
  J meta = path(l, "metadata", "lockupMetadataViewModel");
  std::string title = trim(text(key(meta, "title")));
  std::vector<std::string> parts;
  each(path(meta, "metadata", "contentMetadataViewModel", "metadataRows"), [&](J row) {
    each(key(row, "metadataParts"), [&](J p) {
      std::string t = trim(text(key(p, "text")));
      if (!t.empty()) parts.push_back(t);
    });
  });
  J img = path(l, "contentImage", "thumbnailViewModel", "image");
  if (!img) img = path(l, "contentImage", "collectionThumbnailViewModel", "primaryThumbnail", "thumbnailViewModel", "image");
  std::string thumb = thumbLink(widest(key(img, "sources")));
  std::string id = str(key(l, "contentId"));
  Item i;
  if (ends(type, "_PLAYLIST")) {
    if (id.empty()) return {};
    i.kind = "playlist", i.id = id, i.title = title, i.subtitle = joinDot(parts), i.thumb = thumb;
    return i;
  }
  if (!(ends(type, "_VIDEO") || ends(type, "_MUSIC")) || !isVideoId(id)) return {};
  std::vector<std::string> b;
  badges(l, b);
  int seconds = 0;
  for (auto& t : b)
    if (!seconds) seconds = clockSeconds(t);
  if (!seconds) return {};
  if (parts.size() > 2) parts.resize(2);
  // related videos give a bare "1.6B" for the views
  if (parts.size() == 2) {
    const std::string& v = parts[1];
    bool bare = !v.empty() && isdigit((unsigned char)v[0]) && v.find(' ') == std::string::npos;
    if (bare) parts[1] = v + " views";
  }
  i.kind = "video", i.id = id, i.title = title, i.subtitle = joinDot(parts), i.durationSec = seconds, i.thumb = thumb;
  return i;
}

// tvItems: every video and playlist a TV response shows, in page order, once each
void tvItems(J o, std::vector<Item>& out, std::set<std::string>& seen, int depth = 0) {
  if (!o || depth > 40) return;
  if (yyjson_is_obj(o)) {
    J t = key(o, "tileRenderer"), l = key(o, "lockupViewModel");
    if (t || l) {
      Item i = t ? fromTile(t) : fromLockup(l);
      if (i.ok() && seen.insert(i.id).second) out.push_back(i);
      return;  // nothing playable nests inside an item
    }
    size_t k, n;
    J kk, v;
    yyjson_obj_foreach(o, k, n, kk, v) tvItems(v, out, seen, depth + 1);
  } else if (yyjson_is_arr(o)) {
    each(o, [&](J v) { tvItems(v, out, seen, depth + 1); });
  }
}

// --- the web's videos (browse.ts's videoItem, for search) ---

Item videoRenderer(J v) {
  Item i;
  i.kind = "video";
  i.id = str(key(v, "videoId"));
  if (!isVideoId(i.id)) return {};
  i.title = text(key(v, "title"));
  std::string author = text(key(v, "ownerText"));
  if (author.empty()) author = text(key(v, "longBylineText"));
  std::string views = text(key(v, "shortViewCountText"));
  if (views.empty()) views = text(key(v, "viewCountText"));
  i.subtitle = joinDot({author, views});
  i.durationSec = clockSeconds(text(key(v, "lengthText")));
  i.thumb = thumbLink(widest(key(v, "thumbnail")));
  return i;
}

void searchItems(J list, Chunk<Item>& c, std::string& token) {
  each(list, [&](J e) {
    if (J s = key(e, "itemSectionRenderer")) {
      each(key(s, "contents"), [&](J x) {
        if (J v = key(x, "videoRenderer")) c.items.push_back(videoRenderer(v));
        else if (J l = key(x, "lockupViewModel")) {
          Item i = fromLockup(l);
          if (i.kind == "video") c.items.push_back(i);
        }
      });
    } else if (J ci = key(e, "continuationItemRenderer")) {
      token = str(path(ci, "continuationEndpoint", "continuationCommand", "token"));
    }
  });
}

bool searchMore(const std::string& token, Chunk<Item>& c) {
  Doc d;
  if (innertube(rules().web, "search", "\"continuation\":" + quote(token), d) != 200) return false;
  std::string next;
  each(key(root(d), "onResponseReceivedCommands"), [&](J cmd) {
    searchItems(path(cmd, "appendContinuationItemsAction", "continuationItems"), c, next);
  });
  if (!next.empty()) c.more = [next](Chunk<Item>& n) { return searchMore(next, n); };
  return true;
}

// --- the account's feeds (signed in) ---

// every video of a web browse answer, in page order, and its continuation; the Shorts and news shelves
// between the rows stay out (the grid's own videos only)
void webVideos(J o, Chunk<Item>& c, std::set<std::string>& seen, std::string& token, int depth = 0) {
  if (!o || depth > 40) return;
  if (yyjson_is_obj(o)) {
    if (J v = key(o, "videoRenderer")) {
      Item i = videoRenderer(v);
      if (i.ok() && seen.insert(i.id).second) c.items.push_back(i);
      return;
    }
    if (J l = key(o, "lockupViewModel")) {
      Item i = fromLockup(l);
      if (i.ok() && i.kind == "video" && seen.insert(i.id).second) c.items.push_back(i);
      return;
    }
    if (J ci = key(o, "continuationItemRenderer")) {
      std::string t = str(path(ci, "continuationEndpoint", "continuationCommand", "token"));
      if (!t.empty()) token = t;
      return;
    }
    if (key(o, "reelShelfRenderer") || key(o, "richSectionRenderer")) return;
    size_t k, n;
    J kk, v;
    yyjson_obj_foreach(o, k, n, kk, v) webVideos(v, c, seen, token, depth + 1);
  } else if (yyjson_is_arr(o)) {
    each(o, [&](J v) { webVideos(v, c, seen, token, depth + 1); });
  }
}

bool feedChunk(const std::string& fields, Chunk<Item>& c) {
  Doc d;
  if (innertube(rules().web, "browse", fields, d) != 200 || !root(d)) return false;
  std::set<std::string> seen;
  std::string token;
  webVideos(root(d), c, seen, token);
  if (!token.empty()) c.more = [token](Chunk<Item>& n) { return feedChunk("\"continuation\":" + quote(token), n); };
  return true;
}

// --- Shorts ---

// shortItem: a ShortsLockupView (search), or a "watch next" entry, which names only the video
Item shortLockup(J s) {
  Item i;
  i.kind = "video";
  i.id = str(path(s, "onTap", "innertubeCommand", "reelWatchEndpoint", "videoId"));
  if (!isVideoId(i.id)) return {};
  i.title = text(path(s, "overlayMetadata", "primaryText"));
  i.subtitle = text(path(s, "overlayMetadata", "secondaryText"));
  i.thumb = thumbLink(widest(key(s, "thumbnail")));
  if (i.thumb.empty()) i.thumb = thumbLink("https://i.ytimg.com/vi/" + i.id + "/frame0.jpg");
  return i;
}

void findShorts(J o, std::vector<Item>& out, int depth = 0) {
  if (!o || depth > 14) return;
  if (yyjson_is_obj(o)) {
    if (J s = key(o, "shortsLockupViewModel")) {
      Item i = shortLockup(s);
      if (i.ok()) out.push_back(i);
      return;
    }
    size_t k, n;
    J kk, v;
    yyjson_obj_foreach(o, k, n, kk, v) findShorts(v, out, depth + 1);
  } else if (yyjson_is_arr(o)) {
    each(o, [&](J v) { findShorts(v, out, depth + 1); });
  }
}

std::string base64(const std::string& in) {
  static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
    out += abc[v >> 18], out += abc[(v >> 12) & 63], out += abc[(v >> 6) & 63], out += abc[v & 63];
  }
  if (i + 1 == in.size()) {
    unsigned v = (unsigned char)in[i] << 16;
    out += abc[v >> 18], out += abc[(v >> 12) & 63], out += "==";
  } else if (i + 2 == in.size()) {
    unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
    out += abc[v >> 18], out += abc[(v >> 12) & 63], out += abc[(v >> 6) & 63], out += '=';
  }
  return out;
}

// the chain's entries (sequenceChunk)
void sequenceChunk(J r, Chunk<Item>& c) {
  each(key(r, "entries"), [&](J e) {
    J ep = path(e, "command", "reelWatchEndpoint");
    Item i;
    i.kind = "video", i.id = str(key(ep, "videoId"));
    if (!isVideoId(i.id)) return;
    i.thumb = thumbLink(widest(key(ep, "thumbnail")));
    c.items.push_back(i);
  });
  std::string token = str(path(r, "continuationEndpoint", "continuationCommand", "token"));
  if (token.empty()) return;
  c.more = [token](Chunk<Item>& n) {
    Doc d;
    if (innertube(rules().web, "reel/reel_watch_sequence", "\"continuation\":" + quote(token), d) != 200) return false;
    sequenceChunk(root(d), n);
    return true;
  };
}

// a short's "watch next" chain: ReelSequence {shortId: 1, params: 5 {number: 3 = 5}, feature2: 10 = 25}
bool chain(const std::string& id, Chunk<Item>& c) {
  std::string msg = std::string("\x0a\x0b", 2) + id + std::string("\x2a\x02\x18\x05\x50\x19", 6);
  Doc d;
  if (innertube(rules().web, "reel/reel_watch_sequence", "\"sequenceParams\":" + quote(uriComponent(base64(msg))), d) != 200) return false;
  sequenceChunk(root(d), c);
  return true;
}

std::string compact(int64_t n) {
  char b[32];
  auto one = [&](double v, const char* unit) {
    snprintf(b, sizeof(b), "%.1f", v);
    std::string s = b;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s + unit;
  };
  if (n >= 1000000000) return one(n / 1e9, "B");
  if (n >= 1000000) return one(n / 1e6, "M");
  if (n >= 1000) return std::to_string(int64_t(n / 1000.0 + 0.5)) + "K";
  return std::to_string(n);
}

}  // namespace

std::string home() {
  // YouTube's home is empty signed out and Trending is gone: the TV app's topic pages, interleaved
  static const char* kTopics[] = {"FEtopics_music", "FEtopics_gaming", "FEtopics_news", "FEtopics_sports"};
  std::vector<std::vector<Item>> lists(4);
  std::vector<std::thread> ts;
  for (int k = 0; k < 4; k++)
    ts.emplace_back([&, k] {
      Doc d;
      if (innertube(rules().tv, "browse", std::string("\"browseId\":\"") + kTopics[k] + "\"", d) != 200) return;
      std::vector<Item> all;
      std::set<std::string> seen;
      tvItems(root(d), all, seen);
      for (Item& i : all)
        if (i.kind == "video") lists[k].push_back(i);
    });
  for (auto& t : ts) t.join();
  std::vector<Item> out;
  std::set<std::string> seen;
  for (size_t i = 0;; i++) {
    bool any = false;
    for (auto& l : lists)
      if (i < l.size()) {
        any = true;
        if (seen.insert(l[i].id).second) out.push_back(l[i]);
      }
    if (!any) break;
  }
  if (out.empty()) return "";
  return "{\"items\":" + json(out) + "}";
}

std::string feed(const std::string& scope, const std::string& browseId) {
  std::string fields = "\"browseId\":" + quote(browseId);
  return page<Item>(
      scope, [fields](Chunk<Item>& c) { return feedChunk(fields, c); }, 20, byId(), pageJson);
}

std::string search(const std::string& q) {
  std::string fields = "\"query\":" + quote(q) + ",\"params\":\"EgIQAQ%3D%3D\"";
  return page<Item>(
      "video/search",
      [fields](Chunk<Item>& c) {
        Doc d;
        if (innertube(rules().web, "search", fields, d) != 200) return false;
        J list = path(root(d), "contents", "twoColumnSearchResultsRenderer", "primaryContents", "sectionListRenderer", "contents");
        if (!list) return false;
        std::string token;
        searchItems(list, c, token);
        if (!token.empty()) c.more = [token](Chunk<Item>& n) { return searchMore(token, n); };
        return true;
      },
      10, byId(), pageJson);
}

std::string shorts() {
  return page<Item>(
      "video/shorts",
      [](Chunk<Item>& c) {
        Doc d;
        if (innertube(rules().web, "search", "\"query\":\"shorts\",\"params\":\"EgIQCQ%3D%3D\"", d) != 200) return false;
        std::vector<Item> seeds;
        findShorts(root(d), seeds);
        std::set<std::string> seen;
        for (Item& s : seeds)
          if (seen.insert(s.id).second) c.items.push_back(s);
        if (c.items.empty()) return false;
        // the first shorts start resolving now: their streams are asked for next (prepare)
        for (size_t k = 0; k < c.items.size() && k < 4; k++) prepare(c.items[k].id);
        // one of the first five's chain, on and on
        std::string seed = c.items[size_t(rand()) % std::min<size_t>(5, c.items.size())].id;
        c.more = [seed](Chunk<Item>& n) { return chain(seed, n); };
        return true;
      },
      10, byId(), pageJson);
}

std::string shortInfo(const std::string& id) {
  VideoDetails v;
  if (!details(id, v)) return "";
  std::string views = v.views >= 0 ? compact(v.views) + " views" : "";
  std::string s = "{\"id\":" + quote(id) + ",\"title\":" + quote(v.title) + ",\"channel\":" + quote(v.author) +
                  ",\"subtitle\":" + quote(joinDot({v.author, views}));
  if (v.seconds > 0) s += ",\"durationSec\":" + std::to_string(v.seconds);
  return s + "}";
}

}  // namespace rondo::direct::video
