// The wheel's click (Settings › Click sound): what 1.0.x got from AudioManager.playSoundEffect(
// FX_KEY_CLICK), AudioService's SoundPool playing /system/media/audio/ui/Effect_Tick.ogg on the system
// stream at config_soundEffectVolumeDb (-6 dB). Here an OpenSL player of its own on that stream, the
// tick decoded once.
#pragma once
#include <pthread.h>
#include <stdint.h>

#include <vector>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

namespace rondo {

class Click {
 public:
  // false without the sound file or an output; play() then does nothing
  bool open(SLEngineItf engine, SLObjectItf mix);
  void play();  // from the start, cutting one still sounding

 private:
  static void* idle(void* self);  // pauses the player once the last click has sounded
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  int64_t pauseAt_ = 0;  // CLOCK_MONOTONIC ns; 0: paused
  int64_t durationNs_ = 0;
  std::vector<int16_t> pcm_;  // 44.1 kHz stereo
  SLObjectItf obj_ = nullptr;
  SLPlayItf play_ = nullptr;
  SLAndroidSimpleBufferQueueItf queue_ = nullptr;
};

}  // namespace rondo
