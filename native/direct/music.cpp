// music.h. The comments name the youtubei.js classes and the browse.ts functions each part follows.
#include "music.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>

#include "innertube.h"

namespace rondo::direct::music {
namespace {

const size_t kHomeSections = 6, kSectionItems = 20, kPageItems = 10, kListItems = 100;

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

std::string playlistId(const std::string& id) { return starts(id, "VL") && id.size() > 3 ? id.substr(2) : id; }
bool isMix(const std::string& id) { return starts(id, "RD") && !starts(id, "RDCLAK"); }

int call(const char* endpoint, const std::string& fields, Doc& d) { return innertube(rules().music, endpoint, fields, d); }

// what browse.ts's musicItem reads of a youtubei.js node
struct Node {
  std::string type;  // item_type
  std::string id, fallbackId, title, subtitle, album, year;
  std::vector<std::string> people;  // artists, authors or the author
  int duration = 0;
  std::string thumb;
};

std::string joinNames(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v) {
    if (s.empty()) continue;
    if (!out.empty()) out += ", ";
    out += s;
  }
  return out;
}

// musicItem
Item fromNode(const Node& n) {
  std::string kind = n.type == "non_music_track" ? "song" : n.type;
  static const std::set<std::string> kinds = {"song", "video", "album", "artist", "playlist"};
  std::string raw = !n.id.empty() ? n.id : n.fallbackId;
  if (!kinds.count(kind) || raw.empty()) return {};
  Item i;
  i.kind = kind;
  i.id = kind == "playlist" ? playlistId(raw) : raw;
  std::string by = joinNames(n.people), sub = bullets(n.subtitle);
  if (kind == "song" || kind == "video") i.subtitle = joinDot({by.empty() ? sub : by, n.album});
  else if (kind == "album" && !by.empty()) i.subtitle = joinDot({by, n.year});
  else i.subtitle = joinDot({by.empty() ? sub : by});
  i.title = n.title;
  i.durationSec = n.duration;
  i.thumb = thumbLink(n.thumb);
  return i;
}

bool numeric(const std::string& s) {
  std::string t = trim(s);
  if (t.empty()) return false;
  char* end = nullptr;
  strtod(t.c_str(), &end);
  return end && *end == 0;
}

// MusicTwoRowItem
Node twoRow(J r) {
  Node n;
  Endpoint ep = endpoint(key(r, "navigationEndpoint"));
  n.title = text(key(r, "title"));
  n.id = !ep.browseId.empty() ? ep.browseId : ep.videoId;
  J sub = key(r, "subtitle");
  n.subtitle = text(sub);
  std::vector<Run> rs = runs(sub);
  if (ep.pageType == "MUSIC_PAGE_TYPE_ARTIST") n.type = "artist";
  else if (ep.pageType == "MUSIC_PAGE_TYPE_PLAYLIST") n.type = "playlist";
  else if (ep.pageType == "MUSIC_PAGE_TYPE_ALBUM") n.type = "album";
  else if (ep.name == "watchPlaylistEndpoint") n.type = "endpoint";
  else if (!rs.empty()) n.type = rs[0].text != "Song" ? "video" : "song";
  else n.type = ep.name.empty() ? "unknown" : "endpoint";
  if (n.type == "album" || n.type == "song") {
    for (const Run& x : rs)
      if (starts(x.at.browseId, "UC")) n.people.push_back(x.text);
    if (n.type == "album" && !rs.empty() && numeric(rs.back().text)) n.year = rs.back().text;
  } else if (n.type == "video") {
    for (const Run& x : rs)
      if (starts(x.at.browseId, "UC")) {
        n.people.push_back(x.text);
        break;
      }
  }
  n.thumb = widest(path(r, "thumbnailRenderer", "musicThumbnailRenderer", "thumbnail"));
  return n;
}

bool isDuration(const std::string& s) {  // /^\d+(?::[0-5]\d)+$/
  size_t i = 0;
  while (i < s.size() && isdigit((unsigned char)s[i])) i++;
  if (i == 0 || i == s.size()) return false;
  while (i < s.size()) {
    if (i + 3 > s.size() || s[i] != ':' || s[i + 1] < '0' || s[i + 1] > '5' || !isdigit((unsigned char)s[i + 2])) return false;
    i += 3;
  }
  return true;
}
std::string lastDuration(const std::vector<Run>& rs) {
  for (size_t k = rs.size(); k-- > 0;)
    if (isDuration(rs[k].text)) return rs[k].text;
  return "";
}

// MusicResponsiveListItem
Node responsive(J r) {
  Node n;
  std::vector<J> flex, fixed;
  each(key(r, "flexColumns"), [&](J c) { flex.push_back(path(c, "musicResponsiveListItemFlexColumnRenderer", "text")); });
  each(key(r, "fixedColumns"), [&](J c) { fixed.push_back(path(c, "musicResponsiveListItemFixedColumnRenderer", "text")); });
  auto col = [&](size_t k) -> J { return k < flex.size() ? flex[k] : nullptr; };
  std::string plVideo = str(path(r, "playlistItemData", "videoId"));
  Endpoint ep = endpoint(key(r, "navigationEndpoint"));
  n.fallbackId = !ep.videoId.empty() ? ep.videoId : ep.browseId;
  std::string pageType = ep.pageType;
  if (pageType.empty())
    for (J c : flex)
      if (textEndpoint(c).pageType == "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE") pageType = "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE";
  std::vector<Run> second = runs(col(1));
  auto ucRuns = [&](std::vector<std::string>& out, bool firstOnly) {
    for (const Run& x : second)
      if (starts(x.at.browseId, "UC")) {
        out.push_back(x.text);
        if (firstOnly) break;
      }
  };
  std::string title = text(col(0));
  if (pageType == "MUSIC_PAGE_TYPE_ALBUM") {
    n.type = "album", n.id = ep.browseId, n.title = title;
    ucRuns(n.people, true);
    for (const Run& x : second) {
      const std::string& t = x.text;
      if (t.size() == 4 && (t[0] == '1' || t[0] == '2') && isdigit((unsigned char)t[1]) && isdigit((unsigned char)t[2]) && isdigit((unsigned char)t[3])) {
        n.year = t;
        break;
      }
    }
  } else if (pageType == "MUSIC_PAGE_TYPE_PLAYLIST") {
    n.type = "playlist", n.id = ep.browseId, n.title = title;
    ucRuns(n.people, true);
  } else if (pageType == "MUSIC_PAGE_TYPE_ARTIST" || pageType == "MUSIC_PAGE_TYPE_USER_CHANNEL") {
    n.type = "artist", n.id = ep.browseId, n.title = title;
    n.subtitle = text(col(1));
  } else if (pageType == "MUSIC_PAGE_TYPE_LIBRARY_ARTIST" || pageType == "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE") {
    n.type = "other";
  } else if (pageType == "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE") {
    n.type = "non_music_track", n.id = !plVideo.empty() ? plVideo : ep.videoId, n.title = title;
  } else if (flex.size() > 1) {
    std::vector<Run> first = runs(col(0));
    std::string mvt = first.empty() ? "" : first[0].at.musicVideoType;
    if (mvt == "MUSIC_VIDEO_TYPE_UGC" || mvt == "MUSIC_VIDEO_TYPE_OMV") {
      n.type = "video", n.id = plVideo, n.title = title;
      ucRuns(n.people, false);
      std::string d = lastDuration(second);
      if (d.empty() && !fixed.empty()) d = lastDuration(runs(fixed[0]));
      n.duration = clockSeconds(d);
    } else if (mvt == "MUSIC_VIDEO_TYPE_ATV") {
      n.type = "song", n.id = !plVideo.empty() ? plVideo : ep.videoId, n.title = title;
      std::string d = lastDuration(second);
      if (d.empty() && !fixed.empty()) d = text(fixed[0]);
      n.duration = clockSeconds(d);
      for (size_t c = 1; c <= 2 && n.album.empty(); c++)
        for (const Run& x : runs(col(c)))
          if (starts(x.at.browseId, "MPR")) {
            n.album = x.text;
            break;
          }
      ucRuns(n.people, false);
    } else {
      n.type = ep.name.empty() ? "unknown" : "endpoint";
    }
  } else {
    n.type = ep.name.empty() ? "unknown" : "endpoint";
  }
  n.thumb = widest(path(r, "thumbnail", "musicThumbnailRenderer", "thumbnail"));
  return n;
}

// a card or a row, whichever renderer it is
Item item(J entry) {
  if (J r = key(entry, "musicTwoRowItemRenderer")) return fromNode(twoRow(r));
  if (J r = key(entry, "musicResponsiveListItemRenderer")) return fromNode(responsive(r));
  return {};
}

// panelItem (PlaylistPanelVideo, or the wrapper around one)
Item panel(J entry) {
  J n = path(entry, "playlistPanelVideoWrapperRenderer", "primaryRenderer", "playlistPanelVideoRenderer");
  if (!n) n = key(entry, "playlistPanelVideoRenderer");
  if (!n) return {};
  std::string id = str(key(n, "videoId"));
  if (id.empty()) id = endpoint(key(n, "navigationEndpoint")).videoId;
  if (!isVideoId(id)) return {};
  std::vector<std::string> artists;
  std::string album;
  for (const Run& x : runs(key(n, "longBylineText"))) {
    if (starts(x.at.browseId, "UC")) artists.push_back(x.text);
    else if (album.empty() && starts(x.at.browseId, "MPR")) album = x.text;
  }
  std::string by = joinNames(artists);
  Item i;
  i.kind = "song", i.id = id, i.title = text(key(n, "title"));
  i.subtitle = joinDot({by.empty() ? text(key(n, "shortBylineText")) : by, album});
  i.durationSec = clockSeconds(text(key(n, "lengthText")));
  i.thumb = thumbLink(widest(key(n, "thumbnail")));
  return i;
}

// trackItem: a playable row, always a song to the app
Item track(J entry, const std::string& fallbackThumb = "") {
  bool isPanel = key(entry, "playlistPanelVideoRenderer") || key(entry, "playlistPanelVideoWrapperRenderer");
  Item i = isPanel ? panel(entry) : item(entry);
  if (!i.ok() || (i.kind != "song" && i.kind != "video") || !isVideoId(i.id)) return {};
  i.kind = "song";
  if (i.thumb.empty()) i.thumb = fallbackThumb;
  return i;
}

std::vector<Item> uniqueById(std::vector<Item> v) {
  std::set<std::string> seen;
  std::vector<Item> out;
  for (Item& i : v)
    if (i.ok() && seen.insert(i.id).second) out.push_back(std::move(i));
  return out;
}

// shelfSection: a shelf of any kind with its cards or rows
bool shelf(J entry, Section& out) {
  J s = nullptr;
  for (const char* k : {"musicCarouselShelfRenderer", "musicImmersiveCarouselShelfRenderer", "musicShelfRenderer", "gridRenderer"})
    if ((s = key(entry, k))) break;
  if (!s) return false;
  std::vector<Item> items;
  each(key(s, "contents"), [&](J c) { items.push_back(item(c)); });
  each(key(s, "items"), [&](J c) { items.push_back(item(c)); });
  items = uniqueById(std::move(items));
  if (items.size() > kSectionItems) items.resize(kSectionItems);
  if (items.empty()) return false;
  J h = key(s, "header");
  std::string title;
  for (const char* k : {"musicCarouselShelfBasicHeaderRenderer", "musicImmersiveCarouselShelfBasicHeaderRenderer", "gridHeaderRenderer"})
    if (title.empty()) title = text(path(h, k, "title"));
  if (title.empty()) title = text(key(s, "title"));
  out.title = title;
  out.items = std::move(items);
  return true;
}

std::string pageJson(const Page<Item>& p, const char* lead = "") {
  std::string s = std::string("{") + lead + "\"items\":" + json(p.items);
  if (!p.next.empty()) s += ",\"next\":" + quote(p.next);
  return s + "}";
}

std::function<std::string(const Item&)> byId() {
  return [](const Item& i) { return i.id; };
}

// --- home ---

void homeChunk(J sl, Chunk<Section>& c) {
  each(key(sl, "contents"), [&](J e) {
    Section s;
    if (shelf(e, s)) c.items.push_back(std::move(s));
  });
  std::string token = str(path(sl, "continuations", 0, "nextContinuationData", "continuation"));
  if (token.empty()) return;
  c.more = [token](Chunk<Section>& n) {
    Doc d;
    if (call("browse", "\"continuation\":" + quote(token), d) != 200) return false;
    J sl = path(root(d), "continuationContents", "sectionListContinuation");
    if (!sl) return false;
    homeChunk(sl, n);
    return true;
  };
}

// --- headers (headerInfo) ---

struct Header {
  std::string title, by, subtitle, thumb;
};

Header header(J h) {
  Header out;
  J hh = h;
  if (J e = key(h, "musicEditablePlaylistDetailHeaderRenderer")) {
    J inner = key(e, "header");
    hh = inner;
  }
  // the renderer itself, whichever kind
  J r = nullptr;
  for (const char* k : {"musicResponsiveHeaderRenderer", "musicDetailHeaderRenderer", "musicImmersiveHeaderRenderer", "musicVisualHeaderRenderer"})
    if ((r = key(hh, k))) break;
  if (!r) return out;
  out.title = text(key(r, "title"));
  out.by = text(key(r, "straplineTextOne"));
  if (out.by.empty()) {  // MusicDetailHeader: the author is the subtitle's channel run
    for (const Run& x : runs(key(r, "subtitle")))
      if (starts(x.at.browseId, "UC")) {
        out.by = x.text;
        break;
      }
  }
  std::string count;
  std::string second = text(key(r, "secondSubtitle"));
  size_t a = 0;
  while (count.empty() && a <= second.size()) {
    size_t b = second.find("\xe2\x80\xa2", a);
    std::string part = trim(second.substr(a, b == std::string::npos ? std::string::npos : b - a));
    std::string low = part;
    for (char& ch : low) ch = char(tolower((unsigned char)ch));
    for (const char* w : {" song", " songs", " track", " tracks", " episode", " episodes", " video", " videos"}) {
      size_t at = low.find(w);
      if (at != std::string::npos && at + strlen(w) == low.size()) count = part;
    }
    if (b == std::string::npos) break;
    a = b + 3;
  }
  out.subtitle = joinDot({out.by, bullets(text(key(r, "subtitle"))), count});
  std::string t = widest(path(r, "thumbnail", "musicThumbnailRenderer", "thumbnail"));
  if (t.empty()) t = widest(path(r, "thumbnail", "croppedSquareThumbnailRenderer", "thumbnail"));
  if (t.empty()) t = widest(path(r, "foregroundThumbnail", "musicThumbnailRenderer", "thumbnail"));
  out.thumb = thumbLink(t);
  return out;
}

// a queue row's artists and album (PlaylistPanelVideo's longBylineText)
J panelRenderer(J entry) {
  J n = path(entry, "playlistPanelVideoWrapperRenderer", "primaryRenderer", "playlistPanelVideoRenderer");
  return n ? n : key(entry, "playlistPanelVideoRenderer");
}

// derivedHeader: a page without one (album playlists, mixes) named from its rows
Header derived(const std::vector<J>& rows, const std::vector<Item>& items, const std::string& fallback) {
  Header h;
  std::set<std::string> albums;
  std::vector<std::string> names;
  for (size_t k = 0; k < rows.size(); k++) {
    J row = rows[k];
    if (J r = key(row, "musicResponsiveListItemRenderer")) {
      Node n = responsive(r);
      if (!n.album.empty()) albums.insert(n.album);
      if (k < 10)
        for (auto& p : n.people) names.push_back(p);
    } else if (J p = panelRenderer(row)) {
      for (const Run& x : runs(key(p, "longBylineText"))) {
        if (starts(x.at.browseId, "MPR")) albums.insert(x.text);
        else if (k < 10 && starts(x.at.browseId, "UC")) names.push_back(x.text);
      }
    }
  }
  std::vector<std::string> artists;
  for (auto& n : names)
    if (!n.empty() && std::find(artists.begin(), artists.end(), n) == artists.end() && artists.size() < 3) artists.push_back(n);
  h.title = albums.size() == 1 ? *albums.begin() : fallback;
  h.subtitle = joinDot({joinNames(artists), items.empty() ? "" : std::to_string(items.size()) + " songs"});
  if (!items.empty()) h.thumb = items[0].thumb;
  return h;
}

std::string collectionJson(const Header& h, const Page<Item>& p) {
  std::string lead = "\"title\":" + quote(h.title) + ",\"subtitle\":" + quote(h.subtitle) + ",";
  if (!h.thumb.empty()) lead += "\"thumb\":" + quote(h.thumb) + ",";
  return pageJson(p, lead.c_str());
}

// --- playlists ---

// the rows of a playlist page and the token of the next, in either layout
void playlistRows(J list, std::vector<J>& rows, std::string& token) {
  each(list, [&](J e) {
    if (key(e, "musicResponsiveListItemRenderer")) rows.push_back(e);
    else if (J c = key(e, "continuationItemRenderer")) token = str(path(c, "continuationEndpoint", "continuationCommand", "token"));
  });
}

void playlistChunk(const std::vector<J>& rows, const std::string& token, Chunk<Item>& c) {
  for (J r : rows) c.items.push_back(track(r));
  if (token.empty()) return;
  c.more = [token](Chunk<Item>& n) {
    Doc d;
    if (call("browse", "\"continuation\":" + quote(token), d) != 200) return false;
    std::vector<J> rows;
    std::string next;
    playlistRows(path(root(d), "onResponseReceivedActions", 0, "appendContinuationItemsAction", "continuationItems"), rows, next);
    J old = path(root(d), "continuationContents", "musicPlaylistShelfContinuation");
    if (old) {
      playlistRows(key(old, "contents"), rows, next);
      if (next.empty()) next = str(path(old, "continuations", 0, "nextContinuationData", "continuation"));
    }
    playlistChunk(rows, next, n);
    return true;
  };
}

// a radio's queue (mixChunk)
void mixChunk(const std::string& id, J panelR, Chunk<Item>& c) {
  int rows = 0;
  each(key(panelR, "contents"), [&](J e) {
    c.items.push_back(track(e));
    rows++;
  });
  std::string token = str(path(panelR, "continuations", 0, "nextRadioContinuationData", "continuation"));
  if (token.empty()) token = str(path(panelR, "continuations", 0, "nextContinuationData", "continuation"));
  if (token.empty() || !rows) return;
  c.more = [id, token](Chunk<Item>& n) {
    Doc d;
    if (call("next", "\"playlistId\":" + quote(id) + ",\"continuation\":" + quote(token), d) != 200) return false;
    mixChunk(id, path(root(d), "continuationContents", "playlistPanelContinuation"), n);
    return true;
  };
}

// a search's later page: one musicShelfContinuation
bool searchMore(const std::string& token, Chunk<Item>& c) {
  Doc d;
  if (call("search", "\"continuation\":" + quote(token), d) != 200) return false;
  J s = path(root(d), "continuationContents", "musicShelfContinuation");
  if (!s) return false;
  each(key(s, "contents"), [&](J r) { c.items.push_back(item(r)); });
  std::string t = str(path(s, "continuations", 0, "nextContinuationData", "continuation"));
  if (!t.empty()) c.more = [t](Chunk<Item>& n) { return searchMore(t, n); };
  return true;
}

// the pages after a playlist's first: rows only (browse.ts's playlistPage)
std::string laterRows(const Page<Item>& p) { return pageJson(p, "\"title\":\"\",\"subtitle\":\"\","); }

std::string mix(const std::string& id) {
  Doc d;
  if (call("next", "\"playlistId\":" + quote(id), d) != 200) return "";
  J pr = find(root(d), "playlistPanelRenderer");
  if (!pr) return "";
  std::vector<J> nodes;
  each(key(pr, "contents"), [&](J e) { nodes.push_back(e); });
  std::string title = text(key(pr, "title"));
  if (title.empty()) title = "Mix";
  auto docShared = std::make_shared<Doc>(std::move(d));
  return page<Item>("music/playlist/" + id, [id, pr, docShared](Chunk<Item>& c) { mixChunk(id, pr, c); return true; }, kPageItems, byId(),
                    [nodes, title](const Page<Item>& p) {
                      Header h = derived(nodes, p.items, title);
                      h.title = title;
                      return collectionJson(h, p);
                    },
                    laterRows);
}

}  // namespace

std::string home() {
  return page<Section>(
      "music/home",
      [](Chunk<Section>& c) {
        Doc d;
        if (call("browse", "\"browseId\":\"FEmusic_home\"", d) != 200) return false;
        J sl = path(root(d), "contents", "singleColumnBrowseResultsRenderer", "tabs", 0, "tabRenderer", "content", "sectionListRenderer");
        if (!sl) return false;
        homeChunk(sl, c);
        return true;
      },
      kHomeSections, nullptr,
      [](const Page<Section>& p) {
        std::string s = "{\"sections\":[";
        for (size_t k = 0; k < p.items.size(); k++) s += (k ? "," : "") + json(p.items[k]);
        s += "]";
        if (!p.next.empty()) s += ",\"next\":" + quote(p.next);
        return s + "}";
      });
}

namespace {
// the library's grid: every MusicTwoRowItem that is a playlist (the New playlist button isn't one)
void libraryItems(J o, std::vector<Item>& out, std::set<std::string>& seen, int depth = 0) {
  if (!o || depth > 40) return;
  if (yyjson_is_obj(o)) {
    if (J r = key(o, "musicTwoRowItemRenderer")) {
      Item i = fromNode(twoRow(r));
      if (i.ok() && i.kind == "playlist" && seen.insert(i.id).second) out.push_back(i);
      return;
    }
    size_t k, n;
    J kk, v;
    yyjson_obj_foreach(o, k, n, kk, v) libraryItems(v, out, seen, depth + 1);
  } else if (yyjson_is_arr(o)) {
    each(o, [&](J v) { libraryItems(v, out, seen, depth + 1); });
  }
}
}  // namespace

std::string library() {
  Doc d;
  if (call("browse", "\"browseId\":\"FEmusic_liked_playlists\"", d) != 200 || !root(d)) return "";
  std::vector<Item> items;
  std::set<std::string> seen;
  libraryItems(root(d), items, seen);
  return "{\"playlists\":" + json(items) + "}";
}

std::string search(const std::string& q, const std::string& type) {
  static const std::map<std::string, std::string> kParams = {
      {"song", "EgWKAQIIAQ%3D%3D"}, {"video", "EgWKAQIQAQ%3D%3D"}, {"album", "EgWKAQIYAQ%3D%3D"},
      {"artist", "EgWKAQIgAQ%3D%3D"}, {"playlist", "EgWKAQIoAQ%3D%3D"}};
  auto it = kParams.find(type);
  if (it == kParams.end()) return "";
  std::string fields = "\"query\":" + quote(q) + ",\"params\":" + quote(it->second);
  return page<Item>(
      "music/search",
      [fields](Chunk<Item>& c) {
        Doc d;
        if (call("search", fields, d) != 200) return false;
        J list = path(root(d), "contents", "tabbedSearchResultsRenderer", "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents");
        if (!list) return false;
        std::string token;
        each(list, [&](J e) {
          J s = key(e, "musicShelfRenderer");
          if (!s) return;
          each(key(s, "contents"), [&](J r) { c.items.push_back(item(r)); });
          std::string t = str(path(s, "continuations", 0, "nextContinuationData", "continuation"));
          if (!t.empty()) token = t;
        });
        if (!token.empty()) c.more = [token](Chunk<Item>& n) { return searchMore(token, n); };
        return true;
      },
      kPageItems, byId(), [](const Page<Item>& p) { return pageJson(p); });
}

std::string playlist(const std::string& rawId) {
  std::string id = playlistId(rawId);
  if (isMix(id)) return mix(id);
  Doc d;
  if (call("browse", "\"browseId\":" + quote("VL" + id), d) != 200) return "";
  J r = root(d);
  // the header and the rows: two columns (2025-26), or the older single column
  J tabs = path(r, "contents", "twoColumnBrowseResultsRenderer");
  J head = tabs ? path(tabs, "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents", 0) : key(r, "header");
  J shelfR = tabs ? path(tabs, "secondaryContents", "sectionListRenderer", "contents", 0, "musicPlaylistShelfRenderer")
                  : find(key(r, "contents"), "musicPlaylistShelfRenderer");
  if (!shelfR && !head) return "";
  std::vector<J> rows;
  std::string token;
  playlistRows(key(shelfR, "contents"), rows, token);
  if (token.empty()) token = str(path(shelfR, "continuations", 0, "nextContinuationData", "continuation"));
  Header h = header(head);
  auto keep = std::make_shared<Doc>(std::move(d));
  return page<Item>("music/playlist/" + id, [rows, token, keep](Chunk<Item>& c) { playlistChunk(rows, token, c); return true; }, kPageItems,
                    byId(),
                    [h, rows](const Page<Item>& p) {
                      Header info = h.title.empty() ? derived(rows, p.items, "Playlist") : h;
                      return collectionJson(info, p);
                    },
                    laterRows);
}

std::string album(const std::string& id) {
  Doc d;
  if (call("browse", "\"browseId\":" + quote(id), d) != 200) return "";
  J r = root(d);
  J tabs = path(r, "contents", "twoColumnBrowseResultsRenderer");
  J head = tabs ? path(tabs, "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents", 0) : key(r, "header");
  J shelfR = tabs ? path(tabs, "secondaryContents", "sectionListRenderer", "contents", 0, "musicShelfRenderer")
                  : find(key(r, "contents"), "musicShelfRenderer");
  if (!shelfR) return "";
  Header h = header(head);
  std::vector<Item> rows;
  each(key(shelfR, "contents"), [&](J e) { rows.push_back(track(e, h.thumb)); });
  rows = uniqueById(std::move(rows));
  if (rows.size() > kListItems) rows.resize(kListItems);
  std::string fill = joinDot({h.by, h.title});  // fillAlbumRows
  for (Item& i : rows)
    if (i.subtitle.empty()) i.subtitle = fill;
  Page<Item> p;
  p.items = std::move(rows);
  return collectionJson(h, p);
}

std::string artist(const std::string& id) {
  Doc d;
  if (call("browse", "\"browseId\":" + quote(id), d) != 200) return "";
  J r = root(d);
  Header h = header(key(r, "header"));
  std::string s = "{\"title\":" + quote(h.title);
  if (!h.thumb.empty()) s += ",\"thumb\":" + quote(h.thumb);
  s += ",\"sections\":[";
  int n = 0;
  each(path(r, "contents", "singleColumnBrowseResultsRenderer", "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents"), [&](J e) {
    Section sec;
    if (shelf(e, sec)) s += (n++ ? "," : "") + json(sec);
  });
  return s + "]}";
}

std::string next(const std::string& videoId) {
  Doc d;
  if (call("next", "\"videoId\":" + quote(videoId) + ",\"isAudioOnly\":true", d) != 200) return "";
  J pr = find(root(d), "playlistPanelRenderer");
  if (!pr) return "";
  Doc mixDoc;
  if (!key(pr, "playlistId")) {  // no queue of its own: the automix it previews (getUpNext with automix)
    J auto_ = find(pr, "automixPlaylistVideoRenderer");
    Endpoint ep = endpoint(key(auto_, "navigationEndpoint"));
    if (!ep.playlistId.empty()) {
      std::string f = "\"videoId\":" + quote(videoId) + ",\"playlistId\":" + quote(ep.playlistId);
      if (!ep.params.empty()) f += ",\"params\":" + quote(ep.params);
      if (call("next", f, mixDoc) == 200)
        if (J m = find(root(mixDoc), "playlistPanelRenderer")) pr = m;
    }
  }
  std::vector<Item> items;
  each(key(pr, "contents"), [&](J e) { items.push_back(track(e)); });
  items = uniqueById(std::move(items));
  items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.id == videoId; }), items.end());
  if (items.size() > kListItems) items.resize(kListItems);
  return "{\"items\":" + json(items) + "}";
}

std::string song(const std::string& videoId) {
  // the song as the queue shows it: its artists, its album (and the album's id) and often a year
  Doc d;
  if (call("next", "\"videoId\":" + quote(videoId) + ",\"isAudioOnly\":true", d) != 200) return "";
  J pr = find(root(d), "playlistPanelRenderer");
  J n = nullptr;
  each(key(pr, "contents"), [&](J e) {
    J p = panelRenderer(e);
    if (!n && p) {
      std::string id = str(key(p, "videoId"));
      if (id.empty()) id = endpoint(key(p, "navigationEndpoint")).videoId;
      if (id == videoId) n = p;
    }
  });
  std::string title, album, albumId, year, cover;
  std::vector<std::string> artists;
  int seconds = 0;
  if (n) {
    title = text(key(n, "title"));
    for (const Run& x : runs(key(n, "longBylineText"))) {
      if (starts(x.at.browseId, "UC")) artists.push_back(x.text);
      else if (album.empty() && starts(x.at.browseId, "MPR")) album = x.text, albumId = x.at.browseId;
      else if (x.text.size() == 4 && numeric(x.text)) year = x.text;
    }
    if (artists.empty()) artists.push_back(text(key(n, "shortBylineText")));
    seconds = clockSeconds(text(key(n, "lengthText")));
    cover = widest(key(n, "thumbnail"));
  }
  if (title.empty()) return "";
  // its album: the year, the album's artist, its cover, and where the song is in it (the rows' order)
  std::string albumArtist;
  int number = 0, tracks = 0;
  Doc ad;
  if (!albumId.empty() && call("browse", "\"browseId\":" + quote(albumId), ad) == 200) {
    J r = root(ad);
    J tabs = path(r, "contents", "twoColumnBrowseResultsRenderer");
    J head = tabs ? path(tabs, "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents", 0) : key(r, "header");
    J shelfR = tabs ? path(tabs, "secondaryContents", "sectionListRenderer", "contents", 0, "musicShelfRenderer")
                    : find(key(r, "contents"), "musicShelfRenderer");
    Header h = header(head);
    albumArtist = h.by;
    if (!h.title.empty()) album = h.title;
    for (const std::string& part : std::vector<std::string>{h.subtitle}) {  // "Album • 2019" in its subtitle
      size_t i = 0;
      while ((i = part.find_first_of("0123456789", i)) != std::string::npos) {
        size_t j = i;
        while (j < part.size() && isdigit((unsigned char)part[j])) j++;
        if (j - i == 4 && (i == 0 || !isalnum((unsigned char)part[i - 1]))) year = part.substr(i, 4);
        i = j;
      }
    }
    // the song by its id, else by its title (a song found as its video has another id than its album's
    // track), else the album's only one
    auto plain = [](const std::string& t) {
      std::string o;
      int depth = 0;
      for (char c : t) {
        c = char(tolower((unsigned char)c));
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth) depth--;
        else if (!depth && isalnum((unsigned char)c)) o += c;
        else if (!depth && (unsigned char)c >= 0x80) o += c;
      }
      return o;
    };
    int k = 0, byTitle = 0;
    each(key(shelfR, "contents"), [&](J e) {
      Item t = track(e);
      if (!t.ok()) return;
      k++;
      if (t.id == videoId && !number) number = k;
      if (!byTitle && plain(t.title) == plain(title)) byTitle = k;
    });
    tracks = k;
    if (!number) number = byTitle ? byTitle : tracks == 1 ? 1 : 0;
    // the album's cover: square and the largest there is (a song's own can be a video's frame)
    std::string big = widest(path(r, "background", "musicThumbnailRenderer", "thumbnail"));
    if (big.empty() && head) {
      for (const char* kk : {"musicResponsiveHeaderRenderer", "musicDetailHeaderRenderer"})
        if (J hr = key(head, kk)) big = widest(path(hr, "thumbnail", "musicThumbnailRenderer", "thumbnail"));
    }
    if (!big.empty()) cover = big;
  }
  return "{\"id\":" + quote(videoId) + ",\"title\":" + quote(title) + ",\"artist\":" + quote(joinNames(artists)) +
         ",\"albumArtist\":" + quote(albumArtist) + ",\"album\":" + quote(album) + ",\"albumId\":" + quote(albumId) +
         ",\"year\":" + quote(year) + ",\"track\":" + std::to_string(number) + ",\"tracks\":" + std::to_string(tracks) +
         ",\"cover\":" + quote(cover.empty() ? "" : thumbLink(cover)) + ",\"durationSec\":" + std::to_string(seconds) + "}";
}

bool lyrics(const std::string& videoId, Lyrics& out) {
  Doc d;
  if (call("next", "\"videoId\":" + quote(videoId), d) != 200) return false;
  std::vector<J> tabs;
  findAll(root(d), "tabRenderer", tabs);
  std::string browseId;
  for (J t : tabs) {
    Endpoint ep = endpoint(key(t, "endpoint"));
    if (ep.pageType == "MUSIC_PAGE_TYPE_TRACK_LYRICS") browseId = ep.browseId;
  }
  if (browseId.empty()) return false;  // not a song, or no lyrics tab
  // the Android app's page has the lines with their times (timedLyrics)
  Doc a;
  int asked = innertube(rules().androidMusic, "browse", "\"browseId\":" + quote(browseId), a);
  J model = asked == 200 ? find(root(a), "timedLyricsModel", 64) : nullptr;
  if (asked != 200) fprintf(stderr, "rondo-direct: timed lyrics for %s: %d\n", videoId.c_str(), asked);
  else if (!model) fprintf(stderr, "rondo-direct: timed lyrics for %s: no timedLyricsModel\n", videoId.c_str());
  if (asked == 200) {
    J rows = path(model, "lyricsData", "timedLyricsData");
    struct L {
      std::string text;
      double start, end;
    };
    std::vector<L> lines;
    bool timed = count(rows) > 0;
    each(rows, [&](J r) {
      std::string words = str(key(r, "lyricLine"));
      std::string squeezed;
      for (char ch : words) {
        if (isspace((unsigned char)ch)) {
          if (!squeezed.empty() && squeezed.back() != ' ') squeezed += ' ';
        } else {
          squeezed += ch;
        }
      }
      squeezed = trim(squeezed);
      bool notes = true;
      for (size_t k = 0; k < squeezed.size();) {
        if (squeezed.compare(k, 3, "\xe2\x99\xaa") == 0 || squeezed.compare(k, 3, "\xe2\x99\xab") == 0) k += 3;
        else if (squeezed[k] == ' ') k++;
        else {
          notes = false;
          break;
        }
      }
      J cue = key(r, "cueRange");
      auto ms = [&](const char* k) -> double {
        J v = key(cue, k);
        if (!v) return -1;
        std::string s = isStr(v) ? str(v) : std::to_string(num(v));
        return s.empty() ? -1 : atof(s.c_str());
      };
      double st = ms("startTimeMilliseconds"), en = ms("endTimeMilliseconds");
      if (st < 0) timed = false;
      lines.push_back({notes ? "" : squeezed, st, en});
    });
    std::string plain;
    for (size_t k = 0; k < lines.size(); k++) plain += (k ? "\n" : "") + lines[k].text;
    while (plain.find("\n\n\n") != std::string::npos) plain.replace(plain.find("\n\n\n"), 3, "\n\n");
    plain = trim(plain);
    if (!plain.empty()) {
      out.plain = plain;
      if (timed) {
        for (size_t k = 0; k < lines.size(); k++) {
          out.synced.push_back({int(lines[k].start + 0.5), lines[k].text});
          bool pause = !lines[k].text.empty() && lines[k].end > lines[k].start &&
                       (k + 1 == lines.size() || lines[k + 1].start - lines[k].end >= 1000);
          if (pause) out.synced.push_back({int(lines[k].end + 0.5), ""});
        }
        std::stable_sort(out.synced.begin(), out.synced.end(), [](auto& x, auto& y) { return x.first < y.first; });
      }
      return true;
    }
  } else {
    out.partial = true;
  }
  // the web page's plain text
  Doc w;
  if (call("browse", "\"browseId\":" + quote(browseId), w) != 200) return false;
  J shelfR = find(root(w), "musicDescriptionShelfRenderer");
  std::string plain = trim(text(key(shelfR, "description")));
  if (plain.empty()) return false;
  out.plain = plain;
  out.synced.clear();
  return true;
}

}  // namespace rondo::direct::music
