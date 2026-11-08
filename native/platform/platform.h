// The platform layer: what the portable code needs from the device. The Y1 implements it on Android's
// native layer with no Java (platform/y1/); the Mac simulator on SDL (platform/sim/, later).
#pragma once
#include <stdint.h>

#include <memory>

namespace rondo {

// The screen: a GLES2 context drawing straight into it, paced by its vertical sync.
class Display {
 public:
  virtual ~Display() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  // Nanoseconds between refreshes (the Y1's panel: 23.73 ms, 42.14 Hz).
  virtual int64_t period() const = 0;
  // Blocks until the next refresh starts and returns its time (CLOCK_MONOTONIC). A frame drawn now
  // appears at the refresh after it.
  virtual int64_t waitVsync() = 0;
  // Hands the frame drawn into the current GL context to the screen.
  virtual void present() = 0;
};

// Creates the display and makes its GL context current on the calling thread; null on failure.
std::unique_ptr<Display> openDisplay();

// The Y1's controls. The wheel sends one pulse per detent, cw or ccw.
enum class Key : uint8_t {
  None, WheelCw, WheelCcw, Center, Menu, Play, Next, Prev, Power,
  // a wired headset's buttons (event1, ACCDET): media keys, not the Y1's own
  HeadsetPlay, HeadsetNext, HeadsetPrev, HeadsetVolumeUp, HeadsetVolumeDown
};

struct InputEvent {
  Key key = Key::None;
  bool down = false;  // a wheel pulse is always down, with no release
  int64_t time = 0;   // when it reached Rondo (CLOCK_MONOTONIC ns)
};

// Reads the controls on a thread of its own, so an event is timed the moment it arrives.
class Input {
 public:
  virtual ~Input() = default;
  // The next event, or false when none is waiting.
  virtual bool poll(InputEvent& out) = 0;
  // A descriptor that turns readable when an event is waiting (for poll()/epoll while idle).
  virtual int waitFd() const = 0;
  // Clears waitFd's readable state; call before draining with poll().
  virtual void acknowledge() = 0;
};

std::unique_ptr<Input> openInput();

}  // namespace rondo
