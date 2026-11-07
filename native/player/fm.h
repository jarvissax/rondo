// FM radio (FmRadio.kt in 1.0.x), in rondo-player: MediaTek's tuner through libfmjni.so's plain FMR_*
// calls (S4: power-up and tune take tenths of MHz, seek hundredths), and its sound as this Y1 makes it,
// digitally: "AudioSetFmDigitalEnable=1", then the FM stream recorded from audio source 99 (44.1 kHz
// stereo) and handed to the engine, which plays it through its own output like music. Wired
// headphones are the antenna: without them it won't start, and unplugging them stops it.
#pragma once
#include <pthread.h>
#include <stdint.h>

#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace rondo {

struct FmStatus {
  bool on = false, starting = false, muted = false, seeking = false, scanning = false, stereo = false;
  int frequency = 0;  // tenths of MHz
  int signal = 0;     // 0..4
  int error = 0;      // 1: no wired headphones, 2: the chip didn't start
  std::string name, text;  // RDS: the programme service name and the radio text
  std::vector<int> found;  // the last scan's stations
  uint32_t scanSerial = 0;
  uint32_t seekSerial = 0;  // moves on when a seek has finished (its station in frequency)
};

class FmRadio {
 public:
  using Changed = std::function<void()>;                               // the status changed
  using Sound = std::function<void(const int16_t* pcm, size_t frames)>;  // 44.1 kHz stereo
  FmRadio(Changed changed, Sound sound) : changed_(std::move(changed)), sound_(std::move(sound)) {}

  void start(int frequency);
  void stop();
  void tune(int frequency);
  void seek(bool up, int min, int max);
  void scan(int min, int max);
  void setMuted(bool muted);

  FmStatus status() const;
  bool active() const { return active_.load(); }      // on or starting
  bool sounding() const { return sounding_.load(); }  // its sound is being recorded
  static bool headphonesIn();                         // the wired jack (h2w), the antenna

 private:
  void post(std::function<void()> job);
  static void* run(void* self);
  void loop();
  void readStatus();
  void startSound();
  void stopSound();
  static void* recordThread(void* self);
  void record();
  template <typename F>
  void update(F change) {
    pthread_mutex_lock(&mu_);
    change(st_);
    pthread_mutex_unlock(&mu_);
    changed_();
  }

  Changed changed_;
  Sound sound_;
  mutable pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;  // st_ and jobs_
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  FmStatus st_;
  std::deque<std::function<void()>> jobs_;
  bool started_ = false;
  std::atomic<bool> active_{false}, sounding_{false};

  // the worker's
  int idx_ = -1;
  bool rds_ = false;
  void* rec_ = nullptr;  // android::AudioRecord
  pthread_t recThread_;
  std::atomic<bool> recording_{false};
};

}  // namespace rondo
