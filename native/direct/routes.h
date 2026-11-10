// Direct mode's answers, route by route: the Rondo server's API (docs/dev/server-api.md,
// server/src/*.ts) for the paths the Y1 asks, answered from YouTube itself. The local HTTP server
// (direct/server.h) and the Mac harness (direct/host) call route().
#pragma once
#include <stdint.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rondo::direct {

struct Request {
  std::string method = "GET", path;
  std::map<std::string, std::string> query;
  std::string range;  // the Range header
};
Request parseTarget(const std::string& method, const std::string& target);

using Write = std::function<bool(const char*, size_t)>;

struct Response {
  int status = 200;
  std::string type = "application/json";
  std::string body;
  std::vector<std::string> headers;  // "Name: value"
  // a body that comes as it is fetched (the range proxy): length bytes, written through `write`
  std::function<bool(const Write& write)> stream;
  int64_t length = -1;
  void error(int code, const char* reason) {
    status = code;
    type = "application/json";
    body = std::string("{\"error\":\"") + reason + "\"}";
  }
};

void route(const Request& r, Response& out);

}  // namespace rondo::direct
