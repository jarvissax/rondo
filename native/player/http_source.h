// A song streamed from the server (/s/<id>/140: YouTube's AAC through the server's range proxy) as
// FFmpeg's input: 256 KB Range requests over one libcurl handle kept open between songs, with the
// device's token. Nothing is written to disk; the player's ring holds a few seconds. A request that
// fails (the hotspot refusing a connection, a Wi-Fi drop, the server re-resolving an expired URL) is
// tried again for up to 20 s before the song gives up; a restart (seek, skip) stops it at once.
#pragma once
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

struct AVIOContext;
typedef void CURL;
struct curl_slist;

namespace rondo {

class HttpSource {
 public:
  HttpSource(CURL* curl, std::string url, const std::string& token, std::function<bool()> stopped);
  ~HttpSource();
  AVIOContext* avio() { return avio_; }

 private:
  static int readPacket(void* self, uint8_t* buf, int size);
  static int64_t seekTo(void* self, int64_t offset, int whence);
  bool fill(int64_t at);  // the chunk starting at `at`

  CURL* curl_;
  std::string url_;
  curl_slist* headers_ = nullptr;
  std::function<bool()> stopped_;
  AVIOContext* avio_ = nullptr;
  int64_t pos_ = 0, size_ = -1;
  std::vector<uint8_t> chunk_;
  int64_t chunkAt_ = -1;
  bool wideRoots_ = false;  // the bundled roots didn't verify the server: the system's too
};

}  // namespace rondo
