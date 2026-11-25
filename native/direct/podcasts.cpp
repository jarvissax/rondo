// podcasts.h. The comments name the server/src/podcasts.ts functions each part follows.
#include "podcasts.h"

#include <ctype.h>
#include <string.h>

#include <memory>
#include <set>

#include "innertube.h"

namespace rondo::direct::podcasts {
namespace {

const char* kPodcastPage = "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE";
const char* kEpisodePage = "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE";
const char* kEpisodeType = "MUSIC_VIDEO_TYPE_PODCAST_EPISODE";
const size_t kPageItems = 10;

int call(const char* endpoint, const std::string& fields, Doc& d) { return innertube(rules().music, endpoint, fields, d); }

bool starts(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

// an episode as a Rondo lists it (podcasts.ts's Episode)
struct Episode {
  std::string id, title, subtitle, thumb, show, showId, date, length;
  int durationSec = 0;
  int progress = -1;  // -1: YouTube didn't say (signed out)
  bool played = false;
  bool ok() const { return isVideoId(id) && !title.empty(); }
};

std::string json(const Episode& e) {
  std::string s = "{\"kind\":\"episode\",\"id\":" + quote(e.id) + ",\"title\":" + quote(e.title) + ",\"subtitle\":" + quote(e.subtitle);
  if (!e.thumb.empty()) s += ",\"thumb\":" + quote(e.thumb);
  if (e.durationSec > 0) s += ",\"durationSec\":" + std::to_string(e.durationSec);
  if (!e.show.empty()) s += ",\"show\":" + quote(e.show);
  if (!e.showId.empty()) s += ",\"showId\":" + quote(e.showId);
  if (!e.date.empty()) s += ",\"date\":" + quote(e.date);
  if (!e.length.empty()) s += ",\"length\":" + quote(e.length);
  if (e.progress >= 0) s += ",\"progress\":" + std::to_string(e.progress) + ",\"played\":" + (e.played ? "true" : "false");
  return s + "}";
}

std::string json(const std::vector<Episode>& v) {
  std::string s = "[";
  for (size_t k = 0; k < v.size(); k++) s += (k ? "," : "") + json(v[k]);
  return s + "]";
}

bool separator(const std::string& t) {
  std::string x = trim(t);
  return x.empty() || x == "\xe2\x80\xa2" || x == "\xc2\xb7";
}

// a text's parts: its runs that aren't separators
std::vector<Run> parts(J t) {
  std::vector<Run> out;
  for (Run& r : runs(t))
    if (!separator(r.text)) out.push_back(r);
  return out;
}

// the run naming a show (showRun)
bool showRun(J t, Run& out) {
  for (const Run& r : runs(t))
    if (r.at.pageType == kPodcastPage) return out = r, true;
  return false;
}

// the date: the part right before the show's name, else the last part (dateOf)
std::string dateOf(J t, const Run* show) {
  std::vector<Run> ps = parts(t);
  if (show) {
    for (size_t k = 0; k < ps.size(); k++)
      if (ps[k].text == show->text && ps[k].at.browseId == show->at.browseId) return k ? trim(ps[k - 1].text) : "";
  }
  return ps.empty() ? "" : trim(ps.back().text);
}

void progressOf(J p, Episode& e) {
  J r = key(p, "musicPlaybackProgressRenderer");
  if (!r) return;
  J pct = key(r, "playbackProgressPercentage");
  if (pct) {
    int64_t v = num(pct, 0);
    e.progress = int(v < 0 ? 0 : v > 100 ? 100 : v);
    e.played = e.progress >= 100;
  }
  std::vector<Run> ps = parts(key(r, "durationText"));
  if (!ps.empty()) e.length = trim(ps.back().text);
}

std::string thumbOf(J t) {
  std::string u = widest(path(t, "musicThumbnailRenderer", "thumbnail"));
  if (u.empty()) u = widest(t);
  return thumbLink(u);
}

struct OnShow {
  std::string title, id;
};

// episodeItem: a show's or New Episodes' row (multi-row), a search's or Episodes for Later's
// (responsive), a home shelf's card (two-row)
Episode episodeItem(J entry, const OnShow* on = nullptr) {
  Episode e;
  if (J r = key(entry, "musicMultiRowListItemRenderer")) {
    J watch = key(r, "onTap");
    if (!watch) watch = path(r, "overlay", "musicItemThumbnailOverlayRenderer", "content", "musicPlayButtonRenderer", "playNavigationEndpoint");
    Endpoint w = endpoint(watch);
    std::vector<Run> t = runs(key(r, "title"));
    bool titled = !t.empty() && t[0].at.pageType == kEpisodePage;
    if (w.musicVideoType != kEpisodeType && !titled) return {};
    e.id = w.videoId;
    e.title = text(key(r, "title"));
    e.thumb = thumbOf(key(r, "thumbnail"));
    Run s;
    bool named = showRun(key(r, "secondTitle"), s);
    e.show = named ? s.text : on ? on->title : "";
    e.showId = named ? s.at.browseId : on ? on->id : "";
    e.date = dateOf(key(r, "subtitle"), nullptr);
    progressOf(key(r, "playbackProgress"), e);
    e.subtitle = joinDot({on ? "" : e.show, e.date, e.length});
    return e;
  }
  if (J r = key(entry, "musicResponsiveListItemRenderer")) {
    std::vector<J> cols;
    each(key(r, "flexColumns"), [&](J c) { cols.push_back(path(c, "musicResponsiveListItemFlexColumnRenderer", "text")); });
    auto col = [&](size_t k) -> J { return k < cols.size() ? cols[k] : nullptr; };
    Endpoint play = endpoint(path(r, "overlay", "musicItemThumbnailOverlayRenderer", "content", "musicPlayButtonRenderer", "playNavigationEndpoint"));
    std::vector<Run> t = runs(col(0));
    bool titled = !t.empty() && t[0].at.pageType == kEpisodePage;
    if (play.musicVideoType != kEpisodeType && !titled) return {};
    e.id = str(path(r, "playlistItemData", "videoId"));
    if (e.id.empty()) e.id = play.videoId;
    e.title = text(col(0));
    e.thumb = thumbOf(key(r, "thumbnail"));
    Run s;
    bool named = showRun(col(1), s) || showRun(col(2), s);
    if (named) e.show = s.text, e.showId = s.at.browseId;
    e.durationSec = clockSeconds(trim(text(path(r, "fixedColumns", 0, "musicResponsiveListItemFixedColumnRenderer", "text"))));
    e.date = dateOf(col(1), named ? &s : nullptr);
    e.subtitle = joinDot({e.show, e.date});
    return e;
  }
  if (J r = key(entry, "musicTwoRowItemRenderer")) {
    Endpoint w = endpoint(key(r, "navigationEndpoint"));
    if (w.musicVideoType != kEpisodeType) return {};
    e.id = w.videoId;
    e.title = text(key(r, "title"));
    e.thumb = thumbOf(key(r, "thumbnailRenderer"));
    Run s;
    bool named = showRun(key(r, "subtitle"), s);
    if (named) e.show = s.text, e.showId = s.at.browseId;
    e.date = dateOf(key(r, "subtitle"), named ? &s : nullptr);
    e.subtitle = joinDot({e.show, e.date});
    return e;
  }
  return {};
}

// showItem: a show's card or row
Item showItem(J entry) {
  J r = key(entry, "musicTwoRowItemRenderer");
  bool two = r != nullptr;
  if (!r) r = key(entry, "musicResponsiveListItemRenderer");
  if (!r) return {};
  Endpoint ep = endpoint(key(r, "navigationEndpoint"));
  if (ep.pageType != kPodcastPage || !isShowId(ep.browseId)) return {};
  J title = two ? key(r, "title") : path(r, "flexColumns", 0, "musicResponsiveListItemFlexColumnRenderer", "text");
  J sub = two ? key(r, "subtitle") : path(r, "flexColumns", 1, "musicResponsiveListItemFlexColumnRenderer", "text");
  Item i;
  i.kind = "podcast", i.id = ep.browseId, i.title = text(title);
  std::vector<Run> ps = parts(sub);
  i.subtitle = ps.empty() ? "" : trim(ps.back().text);
  i.thumb = thumbOf(two ? key(r, "thumbnailRenderer") : key(r, "thumbnail"));
  if (i.title.empty()) return {};
  return i;
}

std::string nextToken(J shelf) { return str(path(shelf, "continuations", 0, "nextContinuationData", "continuation")); }

// rowsAndToken
void rowsAndToken(J contents, const OnShow* on, std::vector<Episode>& out, std::string& token) {
  each(contents, [&](J e) {
    std::string t = str(path(e, "continuationItemRenderer", "continuationEndpoint", "continuationCommand", "token"));
    if (!t.empty()) {
      token = t;
      return;
    }
    Episode x = episodeItem(e, on);
    if (x.ok()) out.push_back(std::move(x));
  });
}

std::function<std::string(const Episode&)> byId() {
  return [](const Episode& e) { return e.id; };
}

std::string itemsJson(const Page<Episode>& p) {
  std::string s = "{\"items\":" + json(p.items);
  if (!p.next.empty()) s += ",\"next\":" + quote(p.next);
  return s + "}";
}

// a later page of a show's episodes or a list's (showMore, episodeListMore), from its token
void moreChunk(const std::string& token, std::shared_ptr<OnShow> on, Chunk<Episode>& c);
bool loadMore(const std::string& token, std::shared_ptr<OnShow> on, Chunk<Episode>& n) {
  Doc d;
  if (call("browse", "\"continuation\":" + quote(token), d) != 200) return false;
  J r = root(d);
  std::string next;
  J c = path(r, "continuationContents", "musicShelfContinuation");
  if (!c) c = path(r, "continuationContents", "musicPlaylistShelfContinuation");
  if (c) {
    rowsAndToken(key(c, "contents"), on.get(), n.items, next);
    std::string t = nextToken(c);
    if (!t.empty()) next = t;
  } else {
    each(key(r, "onResponseReceivedActions"), [&](J a) { rowsAndToken(path(a, "appendContinuationItemsAction", "continuationItems"), on.get(), n.items, next); });
  }
  if (!next.empty()) moreChunk(next, on, n);
  return true;
}
void moreChunk(const std::string& token, std::shared_ptr<OnShow> on, Chunk<Episode>& c) {
  c.more = [token, on](Chunk<Episode>& n) { return loadMore(token, on, n); };
}

J responsiveHeader(J r) { return path(r, "contents", "twoColumnBrowseResultsRenderer", "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents", 0, "musicResponsiveHeaderRenderer"); }

// the walk for the library's grid of cards
void cardsIn(J o, std::vector<J>& out, int depth = 0) {
  if (!o || depth > 30) return;
  if (yyjson_is_obj(o)) {
    if (key(o, "musicTwoRowItemRenderer")) return out.push_back(o);
    size_t k, n;
    J kk, v;
    yyjson_obj_foreach(o, k, n, kk, v) cardsIn(v, out, depth + 1);
  } else if (yyjson_is_arr(o)) {
    each(o, [&](J v) { cardsIn(v, out, depth + 1); });
  }
}

// the library page's grid token (libraryToken)
std::string gridToken(J o) {
  J g = find(o, "gridRenderer");
  if (!g) g = find(o, "gridContinuation");
  return nextToken(g);
}

// a search's page: shows or episodes (podcastSearch); token: its next page
void searchRows(J list, bool shows, std::vector<Item>& items, std::vector<Episode>& eps, std::string& token) {
  each(list, [&](J e) {
    J s = key(e, "musicShelfRenderer");
    if (!s) return;
    each(key(s, "contents"), [&](J r) {
      if (shows) {
        Item i = showItem(r);
        if (i.ok()) items.push_back(i);
      } else {
        Episode x = episodeItem(r);
        if (x.ok()) eps.push_back(x);
      }
    });
    std::string t = nextToken(s);
    if (!t.empty()) token = t;
  });
}

// the Mark as played toggle and Episodes for Later's, from an episode's page (episodeActions)
struct Actions {
  std::string playedToken, unplayedToken;
  int isPlayed = -1, later = -1;
};
Actions actions(J r) {
  Actions a;
  J h = responsiveHeader(r);
  std::string token = str(path(h, "progress", "musicPlaybackProgressRenderer", "videoPlaybackPositionFeedbackToken"));
  each(key(h, "buttons"), [&](J b) {
    J t = key(b, "toggleButtonRenderer");
    if (t && str(path(t, "toggledServiceEndpoint", "playlistEditEndpoint", "playlistId")) == "SE" && !path(t, "defaultServiceEndpoint", "modalEndpoint"))
      a.later = yyjson_is_true(key(t, "isToggled")) ? 1 : 0;
    each(path(b, "menuRenderer", "items"), [&](J i) {
      J m = key(i, "toggleMenuServiceItemRenderer");
      std::string on = str(path(m, "defaultServiceEndpoint", "feedbackEndpoint", "feedbackToken"));
      std::string off = str(path(m, "toggledServiceEndpoint", "feedbackEndpoint", "feedbackToken"));
      if (!on.empty() && on == token && !off.empty()) {
        a.playedToken = on, a.unplayedToken = off;
        a.isPlayed = yyjson_is_true(key(m, "isToggled")) ? 1 : 0;
      }
    });
  });
  return a;
}

}  // namespace

bool isShowId(const std::string& id) {
  if (!starts(id, "MPSP") || id.size() < 8 || id.size() > 84) return false;
  for (char c : id)
    if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

Item homeItem(J entry) {
  Item s = showItem(entry);
  if (s.ok()) return s;
  Episode e = episodeItem(entry);
  if (!e.ok()) return {};
  Item i;
  i.kind = "episode", i.id = e.id, i.title = e.title, i.subtitle = e.subtitle, i.thumb = e.thumb;
  return i;
}

std::string library() {
  Doc d;
  if (call("browse", "\"browseId\":\"FEmusic_library_non_music_audio_list\"", d) != 200 || !root(d)) return "";
  std::vector<Item> shows;
  std::set<std::string> seen;
  std::string later, latest;
  bool laterSeen = false, latestSeen = false;
  auto take = [&](J r) {
    std::vector<J> cards;
    cardsIn(key(r, "contents") ? key(r, "contents") : key(r, "continuationContents"), cards);
    for (J c : cards) {
      std::string id = endpoint(path(c, "musicTwoRowItemRenderer", "navigationEndpoint")).browseId;
      if (id == "VLSE") later = trim(text(path(c, "musicTwoRowItemRenderer", "subtitle"))), laterSeen = true;
      else if (id == "VLRDPN") latest = trim(text(path(c, "musicTwoRowItemRenderer", "subtitle"))), latestSeen = true;
      Item s = showItem(c);
      if (s.ok() && seen.insert(s.id).second) shows.push_back(s);
    }
  };
  take(root(d));
  std::string token = gridToken(root(d));
  std::vector<std::unique_ptr<Doc>> keep;
  for (int i = 0; i < 5 && !token.empty(); i++) {
    auto more = std::make_unique<Doc>();
    if (call("browse", "\"continuation\":" + quote(token), *more) != 200) break;
    take(root(*more));
    token = gridToken(root(*more));
    keep.push_back(std::move(more));
  }
  std::string s = "{\"items\":" + json(shows);
  if (laterSeen) s += ",\"later\":" + quote(later);
  if (latestSeen) s += ",\"latest\":" + quote(latest);
  return s + "}";
}

std::string list(const std::string& which) {
  return page<Episode>(
      "music/podcasts/" + which,
      [which](Chunk<Episode>& c) {
        auto d = std::make_shared<Doc>();
        if (call("browse", "\"browseId\":" + quote("VL" + which), *d) != 200) return false;
        J first = path(root(*d), "contents", "twoColumnBrowseResultsRenderer", "secondaryContents", "sectionListRenderer", "contents", 0);
        J shelf = key(first, "musicPlaylistShelfRenderer");
        if (!shelf) shelf = key(first, "musicShelfRenderer");
        std::string token;
        rowsAndToken(key(shelf, "contents"), nullptr, c.items, token);
        std::string t = nextToken(shelf);
        if (!t.empty()) token = t;
        if (!token.empty()) moreChunk(token, nullptr, c);
        return true;
      },
      kPageItems, byId(), itemsJson);
}

std::string show(const std::string& id) {
  auto d = std::make_shared<Doc>();
  if (call("browse", "\"browseId\":" + quote(id), *d) != 200 || !root(*d)) return "";
  J r = root(*d);
  J h = responsiveHeader(r);
  if (!h) return "";
  auto on = std::make_shared<OnShow>();
  on->title = text(key(h, "title"));
  on->id = id;
  std::string by = text(key(h, "straplineTextOne"));
  std::string description = trim(text(path(h, "description", "musicDescriptionShelfRenderer", "description")));
  std::string thumb = thumbOf(key(h, "thumbnail"));
  int saved = -1;  // the Save to library toggle (signed in: a like of the show's playlist)
  each(key(h, "buttons"), [&](J b) {
    J t = key(b, "toggleButtonRenderer");
    if (t && path(t, "defaultServiceEndpoint", "likeEndpoint")) saved = yyjson_is_true(key(t, "isToggled")) ? 1 : 0;
  });
  J shelf = path(r, "contents", "twoColumnBrowseResultsRenderer", "secondaryContents", "sectionListRenderer", "contents", 0, "musicShelfRenderer");
  std::string head = "\"title\":" + quote(on->title) + ",\"subtitle\":" + quote(by) + (thumb.empty() ? "" : ",\"thumb\":" + quote(thumb)) +
                     ",\"description\":" + quote(description) + (saved >= 0 ? std::string(",\"saved\":") + (saved ? "true" : "false") : "") + ",";
  return page<Episode>(
      "music/podcast/" + id,
      [d, shelf, on](Chunk<Episode>& c) {
        std::string token;
        rowsAndToken(key(shelf, "contents"), on.get(), c.items, token);
        std::string t = nextToken(shelf);
        if (!t.empty()) token = t;
        if (!token.empty()) moreChunk(token, on, c);
        return true;
      },
      kPageItems, byId(),
      [head](const Page<Episode>& p) {
        std::string s = "{" + head + "\"items\":" + json(p.items);
        if (!p.next.empty()) s += ",\"next\":" + quote(p.next);
        return s + "}";
      },
      itemsJson);
}

std::string episode(const std::string& id) {
  Doc page, next, web;
  if (call("browse", "\"browseId\":" + quote("MPED" + id), page) != 200 || !root(page)) return "";
  J h = responsiveHeader(root(page));
  std::string title = text(key(h, "title"));
  if (title.empty()) return "";
  Run s;
  bool named = showRun(key(h, "straplineTextOne"), s);
  if (!named) {
    std::vector<Run> rs = runs(key(h, "straplineTextOne"));
    if (!rs.empty()) s = rs[0], named = true;
  }
  J two = path(root(page), "contents", "twoColumnBrowseResultsRenderer");
  std::string description = trim(text(path(two, "secondaryContents", "sectionListRenderer", "contents", 0, "musicDescriptionShelfRenderer", "description")));
  Episode p;
  progressOf(key(h, "progress"), p);
  Actions a = actions(root(page));
  if (a.isPlayed >= 0) p.played = a.isPlayed == 1;
  std::string thumb = thumbOf(key(h, "thumbnail"));
  std::string date = dateOf(key(h, "subtitle"), nullptr);
  // the length: the episode's own row in the queue YouTube Music would play it in (lengthFromNext)
  int seconds = 0;
  if (call("next", "\"videoId\":" + quote(id), next) == 200) {
    std::vector<J> rows;
    findAll(root(next), "playlistPanelVideoRenderer", rows, 40);
    for (J r : rows)
      if (str(key(r, "videoId")) == id && !seconds) seconds = clockSeconds(trim(text(key(r, "lengthText"))));
  }
  // the chapters: YouTube's markers on WEB's watch page (chaptersOf)
  std::string chapters = "[";
  if (innertube(rules().web, "next", "\"videoId\":" + quote(id), web) == 200) {
    J maps = path(find(root(web), "multiMarkersPlayerBarRenderer", 40), "markersMap");
    J list = nullptr;
    for (const char* k : {"DESCRIPTION_CHAPTERS", "AUTO_CHAPTERS"})
      each(maps, [&](J m) {
        if (!list && str(key(m, "key")) == k) list = path(m, "value", "chapters");
      });
    int64_t last = -1;
    int n = 0;
    std::string body;
    each(list, [&](J c) {
      J r = key(c, "chapterRenderer");
      J ms = key(r, "timeRangeStartMillis");
      if (!ms) return;
      int64_t at = num(ms, -1);
      if (at < 0 || at <= last) return;
      last = at;
      body += std::string(n++ ? "," : "") + "{\"ms\":" + std::to_string(at) + ",\"title\":" + quote(trim(text(key(r, "title")))) + "}";
    });
    if (n >= 2) chapters += body;
  }
  chapters += "]";
  std::string out = "{\"id\":" + quote(id) + ",\"title\":" + quote(title) + ",\"show\":" + quote(named ? s.text : "") +
                    ",\"showId\":" + quote(named ? s.at.browseId : "") + (thumb.empty() ? "" : ",\"thumb\":" + quote(thumb)) +
                    ",\"description\":" + quote(description) + (seconds > 0 ? ",\"durationSec\":" + std::to_string(seconds) : "") +
                    (date.empty() ? "" : ",\"date\":" + quote(date)) + ",\"chapters\":" + chapters;
  if (p.progress >= 0) out += ",\"progress\":" + std::to_string(p.progress) + ",\"played\":" + (p.played ? "true" : "false");
  if (a.later >= 0) out += std::string(",\"later\":") + (a.later ? "true" : "false");
  return out + "}";
}

std::string search(const std::string& q, const std::string& type) {
  bool shows = type == "podcast";
  std::string fields = "\"query\":" + quote(q) + ",\"params\":" + quote(shows ? "EgWKAQJQAQ%3D%3D" : "EgWKAQJIAQ%3D%3D");
  if (shows) {
    // shows: Items (paged as music's search is)
    struct Load {
      static bool page(const std::string& fields, bool first, Chunk<Item>& c) {
        Doc d;
        if (call("search", fields, d) != 200) return false;
        J list = first ? path(root(d), "contents", "tabbedSearchResultsRenderer", "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents") : nullptr;
        std::vector<Episode> none;
        std::string token;
        if (first) searchRows(list, true, c.items, none, token);
        else if (J s = path(root(d), "continuationContents", "musicShelfContinuation")) {
          each(key(s, "contents"), [&](J r) {
            Item i = showItem(r);
            if (i.ok()) c.items.push_back(i);
          });
          token = nextToken(s);
        }
        if (!token.empty()) {
          std::string f = "\"continuation\":" + quote(token);
          c.more = [f](Chunk<Item>& n) { return page(f, false, n); };
        }
        return true;
      }
    };
    return page<Item>("music/search", [fields](Chunk<Item>& c) { return Load::page(fields, true, c); }, kPageItems,
                      [](const Item& i) { return i.id; },
                      [](const Page<Item>& p) {
                        std::string s = "{\"items\":" + json(p.items);
                        if (!p.next.empty()) s += ",\"next\":" + quote(p.next);
                        return s + "}";
                      });
  }
  struct Load {
    static bool page(const std::string& fields, bool first, Chunk<Episode>& c) {
      Doc d;
      if (call("search", fields, d) != 200) return false;
      std::vector<Item> none;
      std::string token;
      if (first)
        searchRows(path(root(d), "contents", "tabbedSearchResultsRenderer", "tabs", 0, "tabRenderer", "content", "sectionListRenderer", "contents"), false, none,
                   c.items, token);
      else if (J s = path(root(d), "continuationContents", "musicShelfContinuation")) {
        each(key(s, "contents"), [&](J r) {
          Episode e = episodeItem(r);
          if (e.ok()) c.items.push_back(e);
        });
        token = nextToken(s);
      }
      if (!token.empty()) {
        std::string f = "\"continuation\":" + quote(token);
        c.more = [f](Chunk<Episode>& n) { return page(f, false, n); };
      }
      return true;
    }
  };
  return page<Episode>("music/search", [fields](Chunk<Episode>& c) { return Load::page(fields, true, c); }, kPageItems, byId(), itemsJson);
}

bool save(const std::string& id, bool saved) {
  Doc d;
  std::string playlist = starts(id, "MPSP") ? id.substr(4) : id;
  return call(saved ? "like/like" : "like/removelike", "\"target\":{\"playlistId\":" + quote(playlist) + "}", d) == 200;
}

bool later(const std::string& id, bool on) {
  Doc d;
  std::string action = on ? "{\"action\":\"ACTION_ADD_VIDEO\",\"addedVideoId\":" + quote(id) + ",\"dedupeOption\":\"DEDUPE_OPTION_CHECK\"}"
                          : "{\"action\":\"ACTION_REMOVE_VIDEO_BY_VIDEO_ID\",\"removedVideoId\":" + quote(id) + "}";
  return call("browse/edit_playlist", "\"playlistId\":\"SE\",\"actions\":[" + action + "]", d) == 200;
}

bool played(const std::string& id, bool on) {
  // the toggle's tokens from the episode's page as it is now (sent even when it says so already:
  // YouTube shows a change some seconds late)
  Doc page;
  if (call("browse", "\"browseId\":" + quote("MPED" + id), page) != 200) return false;
  Actions a = actions(root(page));
  const std::string& token = on ? a.playedToken : a.unplayedToken;
  if (token.empty()) return false;
  Doc d;
  return call("feedback", "\"feedbackTokens\":[" + quote(token) + "]", d) == 200;
}

}  // namespace rondo::direct::podcasts
