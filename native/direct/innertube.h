// Direct mode's line to YouTube: InnerTube requests (POST /youtubei/v1/<endpoint>) as each client
// app makes them, and plain GETs to YouTube's other hosts (images, googlevideo, LRCLIB).
//
// Which clients, and their names, versions and user agents, are the "rules": built in, and replaced by
// <data>/youtube-rules.json when it exists, so that a YouTube change can be met without a new Rondo
// (private/native-plan.md, N7). YouTube changes these every month or two.
#pragma once
#include <stdint.h>
#include <yyjson.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rondo::direct {

struct Client {
  std::string name, version, userAgent, host;  // host: www.youtube.com or music.youtube.com
  int id = 0;                                  // X-YouTube-Client-Name
  std::vector<std::pair<std::string, std::string>> extra;  // more of context.client (deviceMake, osName…)
};

struct Rules {
  Client music;         // WEB_REMIX: YouTube Music's pages, search, up next
  Client web;           // WEB: YouTube's search, related, shorts
  Client tv;            // TVHTML5: the topic pages of the signed-out YouTube home
  Client player;        // VISIONOS: stream links without JavaScript or a PO token (Nov 9, 2026)
  Client androidMusic;  // ANDROID_MUSIC: timed lyrics
};

// Where direct mode keeps its state (the visitor id, the rules); "" keeps nothing.
void setDataDir(const std::string& dir);

// The YouTube account direct mode signs in with, as the Rondo server does: a cookies.txt exported from a
// browser signed in to YouTube, uploaded on the dashboard to <data>/youtube-cookies.txt (0600, never
// leaves the Y1). It is read again when it changes. The web clients (WEB, WEB_REMIX) send it, signed
// (SAPISIDHASH); the TV, visionOS and Android clients stay signed out.
bool hasCookies();
int accountGeneration();  // changes with the file: answers cached before belong to the other account
// {"signedIn": bool, "name", "handle"}: YouTube's account menu, asked at most every 10 minutes
std::string accountJson();
bool signedIn();  // the last answer (true until one says otherwise, while there are cookies)
void reloadAccount();  // the file read again now (the dashboard just changed it)
// a Netscape cookies.txt with at least one youtube.com cookie (admin.ts's isCookiesFile)
bool isCookiesText(const std::string& text);
const Rules& rules();

struct DocFree {
  void operator()(yyjson_doc* d) const { yyjson_doc_free(d); }
};
using Doc = std::unique_ptr<yyjson_doc, DocFree>;
inline yyjson_val* root(const Doc& d) { return d ? yyjson_doc_get_root(d.get()) : nullptr; }

// POST https://<host>/youtubei/v1/<endpoint> with {"context": …, <fields>} (fields: JSON members, no
// braces). The HTTP status (0: no connection) and the answer parsed (null when it isn't JSON).
int innertube(const Client& c, const char* endpoint, const std::string& fields, Doc& out);

// A GET to a full URL: the status, the body (up to maxBytes), the Content-Type
struct Fetched {
  int status = 0;
  std::string body, type;
  int64_t total = -1;  // from Content-Range's /total
};
Fetched get(const std::string& url, const std::vector<std::string>& headers = {}, size_t maxBytes = 8 << 20);
// A GET whose body goes to `sink` as it comes (false from it stops); the status and headers first,
// to `head` (false stops before any body)
int getStreamed(const std::string& url, const std::vector<std::string>& headers,
                const std::function<bool(int status, const std::string& type, int64_t length, const std::string& contentRange)>& head,
                const std::function<bool(const char*, size_t)>& sink);

}  // namespace rondo::direct
