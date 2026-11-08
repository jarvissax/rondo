// The Y1's controls, read straight from the kernel's input devices (docs/device/README.md): the wheel
// and Menu/Play/Next/Prev on event2 (mtk-tpd), Center and Power on event0 (mtk-kpd), and a wired
// headset's buttons on event1 (ACCDET: play/pause, next, previous, volume). A thread of its own blocks
// on them, stamps each event on arrival and queues it for the UI.
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#include <atomic>

#include "../../core/clock.h"
#include "../platform.h"

namespace rondo {
namespace {

Key keyFor(int code) {
  switch (code) {
    case KEY_DOWN: return Key::WheelCw;
    case KEY_UP: return Key::WheelCcw;
    case KEY_REPLY: return Key::Center;
    case KEY_ENTER: return Key::Center;
    case KEY_BACK: return Key::Menu;
    case KEY_PLAYPAUSE: return Key::Play;
    case KEY_RIGHT: return Key::Next;
    case KEY_LEFT: return Key::Prev;
    case KEY_POWER: return Key::Power;
    default: return Key::None;
  }
}

// ACCDET's keys (getevent -p: 0x72 0x73 0xa3-0xa6); KEY_STOPCD is left alone, as 1.0.x did
Key headsetKeyFor(int code) {
  switch (code) {
    case KEY_PLAYPAUSE: return Key::HeadsetPlay;
    case KEY_NEXTSONG: return Key::HeadsetNext;
    case KEY_PREVIOUSSONG: return Key::HeadsetPrev;
    case KEY_VOLUMEUP: return Key::HeadsetVolumeUp;
    case KEY_VOLUMEDOWN: return Key::HeadsetVolumeDown;
    default: return Key::None;
  }
}

class EvdevInput : public Input {
 public:
  bool open() {
    const char* paths[] = {"/dev/input/event0", "/dev/input/event2", "/dev/input/event1"};
    for (int i = 0; i < 3; i++) {
      fds_[i] = ::open(paths[i], O_RDONLY | O_NONBLOCK);
      if (fds_[i] < 0) {
        perror(paths[i]);
        if (i == 2) continue;  // the headset's: Rondo works without it
        return false;
      }
    }
    if (pipe(wake_) != 0) return false;
    fcntl(wake_[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_[1], F_SETFL, O_NONBLOCK);
    pthread_create(&thread_, nullptr, &EvdevInput::run, this);
    return true;
  }

  bool poll(InputEvent& out) override {
    uint32_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    out = ring_[r % kRing];
    read_.store(r + 1, std::memory_order_release);
    return true;
  }
  int waitFd() const override { return wake_[0]; }
  void acknowledge() override {
    char buf[64];
    while (read(wake_[0], buf, sizeof(buf)) > 0) {
    }
  }

 private:
  static constexpr uint32_t kRing = 256;

  static void* run(void* self) {
    static_cast<EvdevInput*>(self)->loop();
    return nullptr;
  }

  void loop() {
    pollfd p[3] = {{fds_[0], POLLIN, 0}, {fds_[1], POLLIN, 0}, {fds_[2], POLLIN, 0}};
    input_event ev[32];
    for (;;) {
      if (::poll(p, 3, -1) <= 0) continue;
      int64_t t = now_ns();
      for (int i = 0; i < 3; i++) {
        if (!(p[i].revents & POLLIN)) continue;
        ssize_t n = read(fds_[i], ev, sizeof(ev));
        for (ssize_t k = 0; k < n / ssize_t(sizeof(input_event)); k++) {
          if (ev[k].type != EV_KEY || ev[k].value == 2) continue;  // 2 = autorepeat
          Key key = i == 2 ? headsetKeyFor(ev[k].code) : keyFor(ev[k].code);
          if (key == Key::None) continue;
          bool wheel = key == Key::WheelCw || key == Key::WheelCcw;
          if (wheel && ev[k].value == 0) continue;  // a detent's release carries nothing
          push({key, ev[k].value != 0, t});
        }
      }
    }
  }

  void push(const InputEvent& e) {
    uint32_t w = write_.load(std::memory_order_relaxed);
    if (w - read_.load(std::memory_order_acquire) >= kRing) return;  // full: drop
    ring_[w % kRing] = e;
    write_.store(w + 1, std::memory_order_release);
    char c = 1;
    write(wake_[1], &c, 1);
  }

  int fds_[3] = {-1, -1, -1};
  int wake_[2] = {-1, -1};
  pthread_t thread_;
  InputEvent ring_[kRing];
  std::atomic<uint32_t> write_{0}, read_{0};
};

}  // namespace

std::unique_ptr<Input> openInput() {
  std::unique_ptr<EvdevInput> in(new EvdevInput);
  if (!in->open()) return nullptr;
  return in;
}

}  // namespace rondo
