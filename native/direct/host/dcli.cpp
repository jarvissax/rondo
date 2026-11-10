// direct mode on the Mac (tools/native/direct-host.sh): one route answered and printed,
//   dcli /music/search?q=tarkan&type=song
// or, with --serve <port>, the whole local server, for curl and a desktop browser.
#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../routes.h"
#include "../innertube.h"
#include "../server.h"

int main(int argc, char** argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  signal(SIGPIPE, SIG_IGN);
  rondo::direct::setDataDir(getenv("DCLI_DATA") ? getenv("DCLI_DATA") : "");
  if (argc >= 3 && !strcmp(argv[1], "--serve")) {
    if (!rondo::direct::serve(atoi(argv[2]))) return 1;
    fprintf(stderr, "dcli: serving on 127.0.0.1:%s\n", argv[2]);
    for (;;) pause();
  }
  if (argc < 2) {
    fprintf(stderr, "usage: dcli <path?query> | --serve <port>\n");
    return 2;
  }
  rondo::direct::Request r = rondo::direct::parseTarget("GET", argv[1]);
  rondo::direct::Response out;
  rondo::direct::route(r, out);
  fprintf(stderr, "%d %s\n", out.status, out.type.c_str());
  if (out.stream) {
    size_t n = 0;
    out.stream([&](const char*, size_t len) { n += len; return true; });
    fprintf(stderr, "(streamed %zu bytes)\n", n);
  } else {
    fwrite(out.body.data(), 1, out.body.size(), stdout);
    printf("\n");
  }
  return 0;
}
