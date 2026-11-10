// What direct mode answers with: the server's shapes (server/src/browse.ts), so the screens can't tell
// the two modes apart. Items, sections and pages of them, and their JSON.
#pragma once
#include <stdint.h>

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "json.h"

namespace rondo::direct {

struct Item {
  std::string kind;  // song, video, album, artist, playlist
  std::string id, title, subtitle, thumb;
  int durationSec = 0;
  bool ok() const { return !kind.empty() && !id.empty(); }
};

struct Section {
  std::string title;
  std::vector<Item> items;
};

std::string json(const Item& i);
std::string json(const std::vector<Item>& items);
std::string json(const Section& s);

// "/thumb?u=<url>" for Google's image hosts only (thumbLink): "" otherwise
std::string thumbLink(const std::string& url);
bool isVideoId(const std::string& id);

// One YouTube response reduced: its items (those that didn't reduce are left out) and, when YouTube
// has more, how to fetch the next response (false: it failed).
template <class T>
struct Chunk {
  std::vector<T> items;
  std::function<bool(Chunk<T>&)> more;
};

template <class T>
struct Page {
  std::vector<T> items;
  std::string next;  // "" at the end
};

// paging.ts's Pager: a page is gathered from chunks until it has `min` items (at most three more
// requests); its `next` token resumes after it, for 30 minutes (500 tokens at most)
class Tokens {
 public:
  std::string put(const std::string& scope, std::function<std::string()> load);
  // the page a token stands for, as the route's JSON; "" when unknown, expired or another list's
  bool resume(const std::string& scope, const std::string& token, std::function<std::string()>& load);
  void clear();

 private:
  struct Entry {
    std::string scope;
    std::function<std::string()> load;
    int64_t expires;
  };
  std::mutex mu_;
  std::map<std::string, Entry> entries_;
  std::vector<std::string> order_;
};
Tokens& tokens();

int64_t nowMs();

// A page of T from a first chunk; key: the dedupe key (ids), "" keeps every item. shape: the page as
// the route's JSON; later: the pages after it (a playlist's have no header), shape when empty. The
// `next` token re-runs this from the chunk after. "" when the first request failed.
template <class T>
std::string page(const std::string& scope, std::function<bool(Chunk<T>&)> first, size_t min, std::function<std::string(const T&)> key,
                 std::function<std::string(const Page<T>&)> shape, std::function<std::string(const Page<T>&)> later = nullptr,
                 std::set<std::string> seen = {}) {
  Page<T> p;
  Chunk<T> chunk;
  if (!first(chunk)) return "";
  std::function<bool(Chunk<T>&)> more;
  for (int hop = 0;; hop++) {
    for (T& item : chunk.items) {
      std::string k = key ? key(item) : "";
      if (!k.empty() && seen.count(k)) continue;
      if (!k.empty()) seen.insert(k);
      p.items.push_back(std::move(item));
    }
    more = chunk.more;
    if (!more || p.items.size() >= min || hop >= 3) break;
    Chunk<T> next;
    if (!more(next)) {
      if (p.items.empty()) return "";
      break;  // what there is makes a page; `next` tries the request that failed again
    }
    chunk = std::move(next);
  }
  if (more) {
    auto resume = more;
    auto rest = later ? later : shape;
    p.next = tokens().put(scope, [scope, resume, min, key, rest, seen] { return page<T>(scope, resume, min, key, rest, rest, seen); });
  }
  return shape(p);
}

}  // namespace rondo::direct
