// uploadtest: Api::upload after Api::post on the same handle, against a server on 127.0.0.1 that
// records the bytes it receives (development; runs on the Y1 or any build of rondo_net):
//
//   uploadtest [bytes]
//
// post() used to leave its JSON's pointer on the curl handle, and curl takes POSTFIELDS before the
// read function: the upload after it sent the bytes found where that JSON had been instead of the
// file, or ran off the stack (rondo-core's crashes with WhatsApp's voice messages, Nov 15-18). The
// JSON here is short (kept inside the std::string, on the stack) and gone before the upload, as in
// rondo-core's workers. Exit 0 when the server got the file's bytes exactly.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <string>

#include "../core/api.h"

namespace {

std::atomic<uint64_t> g_lastHash{0};
std::atomic<long> g_lastSize{-1};

uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

// One connection: requests with Content-Length bodies, answered 200 "{}", kept alive.
void* serveConnection(void* p) {
  int fd = int(intptr_t(p));
  std::string buf;
  char tmp[8192];
  for (;;) {
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
      ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) return close(fd), nullptr;
      buf.append(tmp, size_t(n));
    }
    std::string head = buf.substr(0, end);
    buf.erase(0, end + 4);
    long length = 0;
    for (size_t at = 0; at < head.size();) {
      size_t e = head.find("\r\n", at);
      std::string line = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
      at = e == std::string::npos ? head.size() : e + 2;
      if (strncasecmp(line.c_str(), "content-length:", 15) == 0) length = atol(line.c_str() + 15);
    }
    while (long(buf.size()) < length) {
      ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) return close(fd), nullptr;
      buf.append(tmp, size_t(n));
    }
    std::string body = buf.substr(0, size_t(length));
    buf.erase(0, size_t(length));
    g_lastSize = length;
    g_lastHash = fnv(body);
    const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
    if (send(fd, ok, strlen(ok), 0) < 0) return close(fd), nullptr;
  }
}

void* acceptLoop(void* p) {
  int s = int(intptr_t(p));
  for (;;) {
    int fd = accept(s, nullptr, nullptr);
    if (fd < 0) continue;
    pthread_t t;
    pthread_create(&t, nullptr, serveConnection, (void*)intptr_t(fd));
    pthread_detach(t);
  }
}

// a short JSON body in this frame only: its pointer is stale once this returns
int postShort(rondo::Api& api) {
  std::string json = "{\"id\":1}";
  std::string reply;
  return api.post("/wa/test/post", json, reply);
}

}  // namespace

int main(int argc, char** argv) {
  long bytes = argc > 1 ? atol(argv[1]) : 65233;
  int s = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 8) != 0) return perror("uploadtest: listen"), 2;
  socklen_t len = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  pthread_t t;
  pthread_create(&t, nullptr, acceptLoop, (void*)intptr_t(s));

  std::string file = "/data/local/tmp/uploadtest.bin";
  std::string data(size_t(bytes), '\0');
  for (long i = 0; i < bytes; i++) data[size_t(i)] = char((i * 2654435761u) >> 24);
  if (FILE* f = fopen(file.c_str(), "wb")) {
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
  } else {
    return perror("uploadtest: file"), 2;
  }

  rondo::Api api;
  api.configure("http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)), "test-token-not-real");
  int failures = 0;
  for (int round = 1; round <= 3; round++) {
    int p = postShort(api);
    std::string reply;
    int u = api.upload("/wa/test/voice", file, "audio/wav", reply);
    bool same = g_lastSize.load() == bytes && g_lastHash.load() == fnv(data);
    printf("round %d: post %d, upload %d, the server got %ld of %ld bytes, %s\n", round, p, u, g_lastSize.load(), bytes,
           same ? "the file's bytes" : "NOT the file's bytes");
    if (!same) failures++;
  }
  unlink(file.c_str());
  printf("%s\n", failures ? "FAIL" : "OK");
  return failures ? 1 : 0;
}
