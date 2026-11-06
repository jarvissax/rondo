// apitest: N2's server client on the Y1 (Android up, for Wi-Fi). Uses the server and token from
// 1.0.x's config.json and prints status codes, sizes and times, never the token.
//
//   apitest [config.json] [search words]
#include <stdio.h>

#include "../../core/api.h"
#include "../../core/clock.h"
#include "../../core/config.h"

using namespace rondo;

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Config c = loadConfig(argc > 1 ? argv[1] : "/data/data/com.jarvissax.rondo/files/config.json");
  if (c.server.url.empty()) {
    printf("apitest: no server in the config\n");
    return 1;
  }
  Api api;
  api.configure(c.server.url, c.server.token);
  Health h;
  int s = api.health(h);
  printf("/healthz: %d in %d ms (first request: connect and TLS): ok %d, cookies %d, music %d, videos %d\n", s,
         api.lastMs(), h.ok, h.cookiesOk, h.music, h.videos);
  s = api.health(h);
  printf("/healthz again on the open connection: %d in %d ms\n", s, api.lastMs());
  SectionsPage home;
  int64_t t0 = now_ns();
  s = api.musicHome(home);
  size_t rows = 0;
  for (auto& sec : home.sections) rows += sec.items.size();
  printf("/music/home: %d in %d ms (with parsing %.0f ms): %zu sections, %zu items%s\n", s, api.lastMs(),
         (now_ns() - t0) / 1e6, home.sections.size(), rows, home.next.empty() ? "" : ", more pages");
  for (size_t i = 0; i < home.sections.size() && i < 3; i++)
    printf("  %s (%zu)%s%s\n", home.sections[i].title.c_str(), home.sections[i].items.size(),
           home.sections[i].items.empty() ? "" : ": ",
           home.sections[i].items.empty() ? "" : home.sections[i].items[0].title.c_str());
  t0 = now_ns();
  s = api.musicHome(home);
  printf("/music/home again: %d from the 2-minute cache in %.1f ms\n", s, (now_ns() - t0) / 1e6);
  const char* q = argc > 2 ? argv[2] : "barış manço";
  ItemsPage found;
  s = api.musicSearch(q, found);
  printf("/music/search \"%s\": %d in %d ms: %zu items%s\n", q, s, api.lastMs(), found.items.size(),
         found.next.empty() ? "" : ", more pages");
  for (size_t i = 0; i < found.items.size() && i < 3; i++)
    printf("  %s \xe2\x80\x94 %s (%d s)\n", found.items[i].title.c_str(), found.items[i].subtitle.c_str(), found.items[i].durationSec);
  if (!found.next.empty()) {
    ItemsPage page2;
    s = api.more("/music/search?type=song&q=x", found.next, page2);
    printf("next page: %d in %d ms: %zu items\n", s, api.lastMs(), page2.items.size());
  }
  return 0;
}
