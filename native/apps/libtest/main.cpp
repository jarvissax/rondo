// libtest: N2's card-library checks on the Y1 (runs with Android up or down).
//
//   libtest <db> [--import local-index.json] [--compare local-index.json] [--search "words"...]
//
// Scans the card, reads every tag, saves the index, scans again (an unchanged card must take <= 1 s),
// and compares the result with 1.0.x's index: the same files with the same titles, artists, albums,
// track numbers and durations.
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "../../core/clock.h"
#include "../../core/config.h"
#include "../../core/library.h"

using namespace rondo;

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: libtest <db> [--import json] [--compare json] [--search words]... [--config path]\n");
    return 2;
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Library lib;
  int64_t t0 = now_ns();
  if (!lib.open(argv[1])) return 1;
  printf("open: %zu tracks in the index, %.0f ms\n", lib.tracks().size(), (now_ns() - t0) / 1e6);
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--import")) printf("import: %d tracks from 1.0.x\n", lib.importIndexJson(argv[i + 1]));
  }
  t0 = now_ns();
  lib.scan();
  printf("scan: %zu files, %.0f ms\n", lib.tracks().size(), (now_ns() - t0) / 1e6);
  t0 = now_ns();
  int n = lib.readTags();
  printf("tags: %d files read, %.0f ms\n", n, (now_ns() - t0) / 1e6);
  t0 = now_ns();
  lib.save();
  printf("save: %.0f ms\n", (now_ns() - t0) / 1e6);
  t0 = now_ns();
  lib.scan();
  int again = lib.readTags();
  printf("rescan of the unchanged card: %.0f ms (%d files to read again)\n", (now_ns() - t0) / 1e6, again);
  printf("songs %zu, videos %zu, artists %zu, albums %zu\n", lib.songs().size(), lib.videos().size(),
         lib.artists().size(), lib.albums().size());
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--compare")) {
      Library old;
      old.importIndexJson(argv[i + 1]);
      std::map<std::string, const Track*> mine;
      for (const Track& t : lib.tracks()) mine[t.path] = &t;
      int same = 0, missing = 0, title = 0, artist = 0, album = 0, track = 0, dur = 0, durOff1 = 0;
      for (const Track& o : old.tracks()) {
        auto it = mine.find(o.path);
        if (it == mine.end()) {
          missing++;
          continue;
        }
        const Track& m = *it->second;
        bool ok = true;
        if (m.title != o.title) title++, ok = false;
        if (m.artist != o.artist) artist++, ok = false;
        if (m.album != o.album) album++, ok = false;
        if (m.track != o.track) track++, ok = false;
        if (m.durationSec != o.durationSec) {
          if (abs(m.durationSec - o.durationSec) == 1) durOff1++;
          else dur++;
          ok = false;
        }
        if (ok) same++;
        else if (title + artist + album + track + dur < 6 && (m.title != o.title || m.artist != o.artist || m.album != o.album || m.track != o.track || abs(m.durationSec - o.durationSec) > 1))
          printf("  differs: title [%s|%s] artist [%s|%s] album [%s|%s] track %d|%d dur %d|%d\n", m.title.c_str(),
                 o.title.c_str(), m.artist.c_str(), o.artist.c_str(), m.album.c_str(), o.album.c_str(), m.track,
                 o.track, m.durationSec, o.durationSec);
      }
      printf("compare with 1.0.x: %zu there, %zu here; identical %d, missing %d; differing: title %d, artist %d, "
             "album %d, track %d, duration %d (and %d off by one second)\n",
             old.tracks().size(), lib.tracks().size(), same, missing, title, artist, album, track, dur, durOff1);
    } else if (!strcmp(argv[i], "--search")) {
      t0 = now_ns();
      Library::Found f = lib.search(argv[i + 1]);
      printf("search \"%s\": %zu artists, %zu albums, %zu songs, %zu videos (%.1f ms)\n", argv[i + 1], f.artists.size(),
             f.albums.size(), f.songs.size(), f.videos.size(), (now_ns() - t0) / 1e6);
      for (size_t k = 0; k < f.songs.size() && k < 3; k++) printf("  %s - %s\n", f.songs[k]->artist.c_str(), f.songs[k]->title.c_str());
    } else if (!strcmp(argv[i], "--config")) {
      Config c = loadConfig(argv[i + 1]);
      printf("config: accent %s, %s, text %.2f, menu", c.theme.accent.c_str(), c.theme.appearance.c_str(), c.theme.textScale);
      for (auto& m : c.theme.menu) printf(" %s%s", m.id.c_str(), m.visible ? "" : "(hidden)");
      printf("; wheel %s; server %s, token %s; radio %d presets\n", c.behavior.wheelAcceleration.c_str(),
             c.server.url.empty() ? "none" : "set", c.server.token.empty() ? "none" : "set", int(c.radio.presets.size()));
      bool saved = saveConfig("/data/local/tmp/rondo/config-roundtrip.json", c);
      Config back = loadConfig("/data/local/tmp/rondo/config-roundtrip.json");
      printf("config round trip: %s\n", saved && back.theme.accent == c.theme.accent && back.server.url == c.server.url &&
                                                back.theme.menu.size() == c.theme.menu.size() && back.radio.frequency == c.radio.frequency
                                            ? "same"
                                            : "DIFFERENT");
    }
  }
  return 0;
}
