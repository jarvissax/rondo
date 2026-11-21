// Api's requests keep their own method (core/api.cpp): one curl handle serves a worker's GETs, POSTs
// and uploads, and a GET after a POST went out as a POST in 4.0.1 and 4.0.2 (the server answered 404:
// "Unreachable", WhatsApp's "server too old"). A local HTTP server records each request's method.
// (the -D: clock.h's Bionic-only wait, which api.cpp doesn't use)
//   clang -c native/third_party/src/yyjson-0.12.0/src/yyjson.c -o /tmp/yyjson.o &&
//   clang++ -std=c++17 -Dpthread_cond_timedwait_monotonic_np=pthread_cond_timedwait -I native/core \
//     -I native/third_party/src/yyjson-0.12.0/src native/core/api.cpp native/core/net.cpp \
//     native/tests/host/api_method_test.cpp /tmp/yyjson.o -lcurl -o /tmp/amt && /tmp/amt
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "api.h"

namespace rondo {
extern const char kRootCerts[] = "";  // the generated roots.cpp's, unused over plain HTTP
}
using namespace rondo;

static int failed = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), failed++; \
  } while (0)

namespace {
int g_listen = -1;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
std::vector<std::string> g_lines;  // "METHOD /path" of each request, in order

// one connection: requests read and answered 200 until the client closes it (curl keeps it open)
void serve(int fd) {
  std::string in;
  char buf[4096];
  for (;;) {
    size_t head = in.find("\r\n\r\n");
    while (head == std::string::npos) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) return;
      in.append(buf, size_t(n));
      head = in.find("\r\n\r\n");
    }
    std::string headers = in.substr(0, head);
    size_t length = 0;
    for (const char* k : {"Content-Length: ", "content-length: "}) {
      size_t at = headers.find(k);
      if (at != std::string::npos) length = size_t(atol(headers.c_str() + at + strlen(k)));
    }
    while (in.size() < head + 4 + length) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) return;
      in.append(buf, size_t(n));
    }
    std::string line = headers.substr(0, headers.find(" HTTP/"));
    pthread_mutex_lock(&g_mu);
    g_lines.push_back(line);
    pthread_mutex_unlock(&g_mu);
    in.erase(0, head + 4 + length);
    const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    if (write(fd, ok, strlen(ok)) < 0) return;
  }
}

void* acceptLoop(void*) {
  for (;;) {
    int fd = accept(g_listen, nullptr, nullptr);
    if (fd < 0) return nullptr;
    serve(fd);
    close(fd);
  }
}

std::string last() {
  pthread_mutex_lock(&g_mu);
  std::string l = g_lines.empty() ? "" : g_lines.back();
  pthread_mutex_unlock(&g_mu);
  return l;
}
}  // namespace

int main() {
  g_listen = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0 || listen(g_listen, 4) != 0 || getsockname(g_listen, (sockaddr*)&a, &len) != 0) {
    perror("listen");
    return 1;
  }
  pthread_t t;
  pthread_create(&t, nullptr, acceptLoop, nullptr);
  std::string base = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port));

  char file[] = "/tmp/api_method_test_XXXXXX";
  int fd = mkstemp(file);
  if (fd < 0 || write(fd, "voice", 5) != 5) return 1;
  close(fd);

  Api api;
  api.configure(base, "token");
  std::string body;
  CHECK(api.get("/healthz", body) == 200);
  CHECK(last() == "GET /healthz");
  // a chat marked read, then the server's health: the GET must stay a GET
  CHECK(api.post("/wa/chats/1/read", "{}", body) == 200);
  CHECK(last() == "POST /wa/chats/1/read");
  CHECK(api.get("/healthz", body) == 200);
  CHECK(last() == "GET /healthz");
  CHECK(api.get("/wa/status", body) == 200);
  CHECK(last() == "GET /wa/status");
  // a voice message, then a list
  CHECK(api.upload("/wa/chats/1/voice", file, "audio/ogg", body) == 200);
  CHECK(last() == "POST /wa/chats/1/voice");
  CHECK(api.get("/wa/chats", body) == 200);
  CHECK(last() == "GET /wa/chats");
  // a post, an upload right after it (it once read through the post's JSON), then a GET
  CHECK(api.post("/wa/chats/1/text", "{\"text\":\"hi\"}", body) == 200);
  CHECK(api.upload("/wa/chats/1/voice", file, "audio/ogg", body) == 200);
  CHECK(last() == "POST /wa/chats/1/voice");
  CHECK(api.get("/video/home", body) == 200);
  CHECK(last() == "GET /video/home");
  // a full-URL post (pairing), then a GET
  CHECK(api.post(base + "/device/pair", "{}", body) == 200);
  CHECK(last() == "POST /device/pair");
  CHECK(api.get("/music/home", body) == 200);
  CHECK(last() == "GET /music/home");

  unlink(file);
  if (failed) printf("%d failed\n", failed);
  else printf("all passed\n");
  return failed ? 1 : 0;
}
