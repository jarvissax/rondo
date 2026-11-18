// The Y1's microphone (inside, beside the jack), recorded into a WAV file: WhatsApp's voice messages.
// Android's AudioRecord from the built-in mic, 16 kHz mono 16-bit, on a thread of its own; how long it
// has run and how loud it is are read by the engine for the UI.
//
// A headset's microphone never reaches the Y1: with one in the jack (h2w state 1), Android's MIC source
// records the headset input, and that gives only a steady 973 Hz pulse train and its harmonics, nothing
// of the voice (Nov 18: with the headset-detection call state active too, and whatever the backlight
// did). So with a headset in, the recording takes the camcorder source, which stays on the built-in
// mic (the policy's getDeviceForInputSource gives 0x80000004 for it, headset or not).
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <functional>
#include <string>

namespace rondo {

class Recorder {
 public:
  using Changed = std::function<void()>;
  explicit Recorder(Changed changed) : changed_(std::move(changed)) {}
  ~Recorder() { stop(); }
  // false when the microphone can't be opened (error() says so)
  // source: Android's audio source; -1: the built-in mic, whatever is in the jack (MIC, or the
  // camcorder source with a headset in)
  bool start(const std::string& path, int source = -1);
  // the file's header finished (keep: true) or the file removed
  void stop(bool keep = true);
  bool recording() const { return recording_.load(); }
  int64_t ms() const { return frames_.load() * 1000 / kRate; }
  int level() const { return level_.load(); }  // the last tenth of a second's peak, 0-100
  int error() const { return error_.load(); }  // 0, or AudioRecord's status when it wouldn't start

  static constexpr int kRate = 16000;
  static constexpr int64_t kMaxMs = 15 * 60 * 1000;  // WhatsApp's limit is longer; the card's room isn't

 private:
  static void* run(void* self);
  void loop();
  Changed changed_;
  void* rec_ = nullptr;  // android::AudioRecord
  FILE* file_ = nullptr;
  std::string path_;
  pthread_t thread_{};
  std::atomic<bool> recording_{false};
  std::atomic<int64_t> frames_{0};
  std::atomic<int> level_{0}, error_{0};
};

}  // namespace rondo
