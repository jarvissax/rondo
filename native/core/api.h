// The Rondo server's API (today's net/Api.kt): JSON over HTTPS with the device's token, through one
// libcurl handle (OpenSSL, HTTP/2 to the server, the connection kept open). The roots trusted are the
// ones 1.0.x bundles (ISRG Root X1 and YR). Runs in rondo-core, never in the drawing process.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

typedef void CURL;

namespace rondo {

struct Item {  // one row of any list the server returns
  std::string kind, id, title, subtitle, thumb;
  int durationSec = -1;
};
struct Section {
  std::string title;
  std::vector<Item> items;
};
struct SectionsPage {
  std::vector<Section> sections;
  std::string next;  // empty: the last page
};
struct ItemsPage {
  std::vector<Item> items;
  std::string next;
};
struct Health {
  bool ok = false, cookiesOk = false, music = true, videos = true;
  int lastResolveOk = -1;  // the server's last playback check: -1 not tried yet, 0 failing, 1 working
  std::string ytdlp;      // empty: missing
};

class Api {
 public:
  Api();
  ~Api();
  void configure(const std::string& baseUrl, const std::string& token);
  bool configured() const { return !base_.empty(); }

  // GET base + path: the HTTP status (0: no connection) and the body. Connection refusals are retried
  // 20 times 250 ms apart (the hotspot this Y1 uses refuses most new connections; ConnectRetry.kt).
  int get(const std::string& path, std::string& body);
  // As get(), answered from memory for ttlMs (screens reopened within two minutes open at once).
  int cached(const std::string& path, std::string& body, int64_t ttlMs = 120000);
  // POST a JSON body to a full URL without the token (pairing: 1.0.x's bare client)
  int post(const std::string& url, const std::string& json, std::string& body);

  int health(Health& out);
  int musicHome(SectionsPage& out);
  int musicSearch(const std::string& query, ItemsPage& out, const std::string& type = "song");
  int videoSearch(const std::string& query, ItemsPage& out);
  // The next page of a list: the same path with ?next=<token>. 410: the token expired (30 min); reload.
  int more(const std::string& path, const std::string& next, ItemsPage& out);

  static std::string escape(const std::string& s);
  // The time the last request took, ms (for the logs).
  int lastMs() const { return lastMs_; }

 private:
  CURL* curl_;
  std::string base_, auth_;
  int lastMs_ = 0;
  struct Entry {
    std::string path, body;
    int64_t at;
  };
  std::vector<Entry> cache_;
};

bool parseSections(const std::string& json, SectionsPage& out);
bool parseHealth(const std::string& json, Health& out);
// /video/short/<id>: a short's details (the Shorts chain names only the video and its first frame)
struct ShortInfo {
  std::string id, title, channel, subtitle;
  int durationSec = -1;
};
bool parseShortInfo(const std::string& json, ShortInfo& out);
bool parseItems(const std::string& json, ItemsPage& out);

}  // namespace rondo
