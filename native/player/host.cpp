// The UI's side of player.h.
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "player.h"
#include "protocol.h"

namespace rondo {

bool PlayerHost::start() {
  void* p = mmap(nullptr, sizeof(PlayerState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return false;
  shm_ = static_cast<PlayerState*>(p);
  memset(shm_, 0, sizeof(PlayerState));  // every page touched now, never in a frame
  shm_->index = shm_->pos = -1;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return false;
  child_ = fork();
  if (child_ < 0) return false;
  if (child_ == 0) {
    close(sv[0]);
    runPlayer(sv[1], shm_);
  }
  close(sv[1]);
  sock_ = sv[0];
  return true;
}

PlayerState PlayerHost::state(bool withOrder) const {
  PlayerState s;
  if (!shm_) {
    memset(&s, 0, sizeof(s));
    s.index = s.nextIndex = -1;
    return s;
  }
  size_t n = withOrder ? sizeof(s) : offsetof(PlayerState, order);
  for (int tries = 0; tries < 100; tries++) {  // the sequence lock: an even, unchanged count
    uint32_t a = __atomic_load_n(&shm_->seq, __ATOMIC_ACQUIRE);
    if (a & 1) continue;
    memcpy(&s, shm_, n);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&shm_->seq, __ATOMIC_ACQUIRE) == a) return s;
  }
  memcpy(&s, shm_, n);
  return s;
}

void PlayerHost::send(uint32_t type, const void* payload, uint32_t len) {
  uint32_t head[2] = {4 + len, type};
  writeFully(sock_, head, sizeof(head));
  if (len) writeFully(sock_, payload, len);
}

void PlayerHost::playQueue(const std::vector<std::string>& paths, int start) {
  queue_ = paths;
  std::string b(4, '\0');
  uint32_t s = uint32_t(start);
  memcpy(&b[0], &s, 4);
  for (const auto& p : paths) b += p, b += '\0';
  send(kPlayQueue, b.data(), uint32_t(b.size()));
}

void PlayerHost::togglePlay() { send(kToggle); }
void PlayerHost::play() { send(kPlay); }
void PlayerHost::pause() { send(kPause); }
void PlayerHost::next() { send(kNext); }
void PlayerHost::previous() { send(kPrevious); }
void PlayerHost::seekTo(int64_t ms) { send(kSeekTo, &ms, 8); }
void PlayerHost::seekBy(int64_t ms) { send(kSeekBy, &ms, 8); }
void PlayerHost::jumpTo(int index) {
  int64_t i = index;
  send(kJumpTo, &i, 8);
}
void PlayerHost::insertNext(const std::string& path) {
  queue_.push_back(path);
  send(kInsertNext, path.data(), uint32_t(path.size()));
}
void PlayerHost::append(const std::string& path) {
  queue_.push_back(path);
  send(kAppend, path.data(), uint32_t(path.size()));
}
void PlayerHost::release() { send(kRelease); }
void PlayerHost::configure(const std::string& baseUrl, const std::string& token) {
  std::string b = baseUrl + '\0' + token;
  send(kConfigure, b.data(), uint32_t(b.size()));
}
void PlayerHost::setShuffle(bool on) {
  uint8_t v = on;
  send(kShuffle, &v, 1);
}
void PlayerHost::setRepeat(int mode) {
  uint8_t v = uint8_t(mode);
  send(kRepeat, &v, 1);
}
void PlayerHost::changeVolume(int delta) {
  int64_t d = delta;
  send(kVolume, &d, 8);
}
void PlayerHost::playVideo(const std::string& soundPath, const std::vector<std::string>& picturePaths) {
  queue_ = {soundPath};
  std::string b = soundPath;
  for (const auto& p : picturePaths) b += '\0', b += p;
  send(kPlayVideo, b.data(), uint32_t(b.size()));
}
void PlayerHost::setVideoQuality(const std::vector<std::string>& picturePaths) {
  std::string b;
  for (const auto& p : picturePaths) b += p, b += '\0';
  send(kVideoQuality, b.data(), uint32_t(b.size()));
}
void PlayerHost::fmStart(int frequency, int volume, bool speaker) {
  uint8_t b[9];
  int32_t f = frequency, v = volume;
  memcpy(b, &f, 4);
  memcpy(b + 4, &v, 4);
  b[8] = speaker;
  send(kFmStart, b, 9);
}
void PlayerHost::fmStop() { send(kFmStop); }
void PlayerHost::fmTune(int frequency) {
  int32_t f = frequency;
  send(kFmTune, &f, 4);
}
void PlayerHost::fmSeek(bool up, int min, int max) {
  uint8_t b[9];
  b[0] = up;
  int32_t a = min, z = max;
  memcpy(b + 1, &a, 4);
  memcpy(b + 5, &z, 4);
  send(kFmSeek, b, 9);
}
void PlayerHost::fmScan(int min, int max) {
  int32_t b[2] = {min, max};
  send(kFmScan, b, 8);
}
void PlayerHost::fmMute(bool muted) {
  uint8_t v = muted;
  send(kFmMute, &v, 1);
}
void PlayerHost::fmSpeaker(bool on) {
  uint8_t v = on;
  send(kFmSpeaker, &v, 1);
}
void PlayerHost::click() { send(kClick); }
void PlayerHost::stopVideo() {
  queue_.clear();
  send(kStopVideo);
}
bool PlayerHost::active() const { return !queue_.empty(); }

}  // namespace rondo
