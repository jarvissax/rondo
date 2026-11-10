// server.h.
#include "server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <thread>

#include "routes.h"

namespace rondo::direct {
namespace {

std::atomic<int> g_open{0};

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool sendAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    ssize_t k = send(fd, p, n, kSendFlags);
    if (k <= 0) return false;
    p += k, n -= size_t(k);
  }
  return true;
}

const char* reason(int s) {
  switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 410: return "Gone";
    case 416: return "Range Not Satisfiable";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return s >= 500 ? "Server Error" : s >= 400 ? "Client Error" : "OK";
  }
}

void connection(int fd) {
  timeval tv{60, 0};  // an idle connection goes after a minute
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  std::string buf;
  for (;;) {
    size_t end;
    while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
      if (buf.size() > 32768) return;
      char tmp[4096];
      ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) return;
      buf.append(tmp, size_t(n));
    }
    std::string head = buf.substr(0, end);
    buf.erase(0, end + 4);
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return;
    std::string method = line.substr(0, s1), target = line.substr(s1 + 1, s2 - s1 - 1), version = line.substr(s2 + 1);
    bool keep = version != "HTTP/1.0";
    std::string range;
    int64_t bodyLen = 0;
    size_t at = eol == std::string::npos ? head.size() : eol + 2;
    while (at < head.size()) {
      size_t e = head.find("\r\n", at);
      std::string h = head.substr(at, e == std::string::npos ? std::string::npos : e - at);
      at = e == std::string::npos ? head.size() : e + 2;
      size_t colon = h.find(':');
      if (colon == std::string::npos) continue;
      std::string name = h.substr(0, colon), value = h.substr(colon + 1);
      while (!value.empty() && value[0] == ' ') value.erase(0, 1);
      if (!strcasecmp(name.c_str(), "range")) range = value;
      else if (!strcasecmp(name.c_str(), "connection") && !strcasecmp(value.c_str(), "close")) keep = false;
      else if (!strcasecmp(name.c_str(), "content-length")) bodyLen = atoll(value.c_str());
    }
    // a body (none of the routes reads one): read past it
    while (bodyLen > 0) {
      if (!buf.empty()) {
        size_t take = std::min<size_t>(buf.size(), size_t(bodyLen));
        buf.erase(0, take);
        bodyLen -= int64_t(take);
        continue;
      }
      char tmp[4096];
      ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0) return;
      buf.append(tmp, size_t(n));
    }
    Request req = parseTarget(method, target);
    req.range = range;
    Response res;
    timeval t0, t1;
    gettimeofday(&t0, nullptr);
    if (method != "GET" && method != "HEAD") res.error(404, "not_found");
    else route(req, res);
    static const bool log = getenv("RONDO_DIRECT_LOG") != nullptr;  // development: every request, its answer and time
    if (log) {
      gettimeofday(&t1, nullptr);
      long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
      fprintf(stderr, "rondo-direct: %s %.120s%s -> %d %s %zu B, %ld ms\n", method.c_str(), target.c_str(), range.empty() ? "" : (" " + range).c_str(),
              res.status, res.type.c_str(), res.stream ? size_t(res.length) : res.body.size(), ms);
    }
    int64_t length = res.stream ? res.length : int64_t(res.body.size());
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\nContent-Type: " + res.type + "\r\n";
    if (length >= 0) out += "Content-Length: " + std::to_string(length) + "\r\n";
    else keep = false;  // a stream of unknown length ends with the connection
    for (const std::string& h : res.headers) out += h + "\r\n";
    out += keep ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    if (!sendAll(fd, out.data(), out.size())) return;
    if (method == "HEAD") {
      if (!keep) return;
      continue;
    }
    if (res.stream) {
      int64_t sent = 0;
      bool ok = res.stream([&](const char* p, size_t n) {
        sent += int64_t(n);
        return sendAll(fd, p, n);
      });
      if (!ok || (length >= 0 && sent != length)) return;  // a short body can't be told apart otherwise
    } else if (!sendAll(fd, res.body.data(), res.body.size())) {
      return;
    }
    if (!keep) return;
  }
}

}  // namespace

bool serve(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return false;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 32) != 0) {
    perror("rondo-direct: listen");
    close(s);
    return false;
  }
  std::thread([s] {
    for (;;) {
      int fd = accept(s, nullptr, nullptr);
      if (fd < 0) {
        usleep(100000);
        continue;
      }
      if (g_open >= 32) {  // more connections than the Y1's parts make: someone is misbehaving
        close(fd);
        continue;
      }
      g_open++;
      std::thread([fd] {
        connection(fd);
        close(fd);
        g_open--;
      }).detach();
    }
  }).detach();
  return true;
}

}  // namespace rondo::direct
