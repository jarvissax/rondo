// N0 test S4: FM radio without Java.
//
// Run with the Java layer stopped (`adb shell stop zygote`) and wired headphones plugged in (they are
// the antenna). libfmjni.so is MediaTek's FM library: its JNI functions only wrap plain FMR_* calls,
// which this calls directly (units read from the wrappers with REA: power-up and tune take MHz x 10,
// seek MHz x 100; 0 means success). FM sound is switched by the audio parameter
// "AudioSetFmEnable=1" (what MediaTek's FM player in libmtkplayer.so sets) through AudioSystem.
//
// The tuner stays muted the whole time, on purpose: this checks control and the audio path switch,
// not loudness.
//
//   s4 [start MHz] [noaudio]      default 87.5

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "../../platform/aosp_begin.h"
#include <binder/ProcessState.h>
#include "../../platform/permission_service.h"
#include <media/AudioRecord.h>
#include <media/AudioSystem.h>
#include <utils/String8.h>
#include "../../platform/aosp_end.h"

// libfmjni.so's FMR_* functions (C++ names, plain arguments)
int FMR_init();
int FMR_open_dev(int idx);
int FMR_close_dev(int idx);
int FMR_pwr_up(int idx, int freq10);
int FMR_pwr_down(int idx, int type);
int FMR_tune(int idx, int freq10);
int FMR_seek(int idx, int freq100, int dir, int* found100);
int FMR_get_rssi(int idx, int* rssi);
int FMR_get_stereomono(int idx, int* stereo);
int FMR_set_mute(int idx, int mute);
int FMR_turn_on_off_rds(int idx, int on);
int FMR_read_rds_data(int idx, uint16_t* status);
int FMR_get_ps(int idx, uint8_t** ps, int* len);

using android::AudioSystem;
using android::String8;

static int64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  android::ProcessState::self()->startThreadPool();
  // without system_server, AudioFlinger's permission check for setParameters waits forever
  if (android::defaultServiceManager()->checkService(android::String16("permission")) == nullptr)
    printf("s4: published our permission service: %s\n", rondo::PermissionService::publish() ? "yes" : "no");
  // "digital": the path this Y1 uses (libaudiosetting.so: fm_use_analog_input = 0). MediaTek's FM player
  // enables "AudioSetFmDigitalEnable=1", records the FM stream from audio source 99 at 44.1 kHz stereo
  // and plays it through an AudioTrack. This records one second and measures its level; nothing plays.
  if (argc > 2 && !strcmp(argv[2], "digital")) {
    int idx = FMR_init();
    FMR_open_dev(idx);
    int freq10 = int(atof(argv[1]) * 10 + 0.5f);
    int64_t a = now_ms();
    int rr = FMR_pwr_up(idx, freq10);
    FMR_set_mute(idx, 0);  // the digital path only reaches the recording, never a speaker by itself
    int rssi = 0;
    FMR_get_rssi(idx, &rssi);
    printf("s4: tuner at %.1f MHz -> %d (%lld ms), rssi %d\n", freq10 / 10.0, rr, (long long)(now_ms() - a), rssi);
    a = now_ms();
    rr = AudioSystem::setParameters(0, String8("AudioSetFmDigitalEnable=1"));
    printf("s4: AudioSetFmDigitalEnable=1 -> %d in %lld ms\n", rr, (long long)(now_ms() - a));
    android::AudioRecord* rec = new (::operator new(sizeof(android::AudioRecord) + 256))
        android::AudioRecord((audio_source_t)99, 44100, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_IN_STEREO, 0);
    printf("s4: AudioRecord(FM source 99) initCheck %d\n", rec->initCheck());
    if (rec->initCheck() == 0) {
      rec->start();
      static int16_t pcm[44100 * 2];
      size_t got = 0;
      int64_t until = now_ms() + 3000;
      while (got < sizeof(pcm) && now_ms() < until) {
        ssize_t n = rec->read((char*)pcm + got, sizeof(pcm) - got);
        if (n <= 0) break;
        got += size_t(n);
      }
      rec->stop();
      double sum = 0;
      int peak = 0;
      size_t samples = got / 2;
      for (size_t i = 0; i < samples; i++) {
        sum += double(pcm[i]) * pcm[i];
        if (abs(pcm[i]) > peak) peak = abs(pcm[i]);
      }
      double rms = samples ? sqrt(sum / samples) : 0;
      printf("s4: recorded %zu bytes of FM audio: RMS %.0f (%.1f dBFS), peak %d\n", got, rms,
             rms > 0 ? 20 * log10(rms / 32768) : -999.0, peak);
    }
    AudioSystem::setParameters(0, String8("AudioSetFmDigitalEnable=0"));
    FMR_set_mute(idx, 1);
    FMR_pwr_down(idx, 0);
    FMR_close_dev(idx);
    printf("s4: FM off\n");
    return 0;
  }
  // "audioonly": only the audio path switch, with the tuner off
  if (argc > 1 && !strcmp(argv[1], "audioonly")) {
    int64_t a = now_ms();
    int rr = AudioSystem::setParameters(0, String8("AudioSetFmEnable=1"));
    printf("s4: AudioSetFmEnable=1 -> %d in %lld ms; GetFmEnable \"%s\"\n", rr, (long long)(now_ms() - a),
           AudioSystem::getParameters(0, String8("GetFmEnable")).string());
    rr = AudioSystem::setParameters(0, String8("AudioSetFmEnable=0"));
    printf("s4: AudioSetFmEnable=0 -> %d; GetFmEnable \"%s\"\n", rr,
           AudioSystem::getParameters(0, String8("GetFmEnable")).string());
    return 0;
  }
  // "noaudio" skips the audio path switch (it can block in mediaserver; see the N0 log)
  const bool audio = !(argc > 2 && !strcmp(argv[2], "noaudio"));
  float startMhz = argc > 1 ? atof(argv[1]) : 87.5f;
  int64_t t0 = now_ms();
  int idx = FMR_init();
  int r = FMR_open_dev(idx);
  printf("s4: FMR_init -> %d, open_dev -> %d (%lld ms)\n", idx, r, (long long)(now_ms() - t0));
  if (r != 0) return 1;

  t0 = now_ms();
  r = FMR_pwr_up(idx, int(startMhz * 10 + 0.5f));
  FMR_set_mute(idx, 1);
  printf("s4: power up at %.1f MHz -> %d (%lld ms), muted\n", startMhz, r, (long long)(now_ms() - t0));
  if (r != 0) {
    FMR_close_dev(idx);
    return 1;
  }
  int rssi = 0, stereo = 0;
  FMR_get_rssi(idx, &rssi);
  FMR_get_stereomono(idx, &stereo);
  printf("s4: %.1f MHz: rssi %d, %s\n", startMhz, rssi, stereo ? "stereo" : "mono");

  // two seeks upward: each finds the next station with enough signal
  int at100 = int(startMhz * 100 + 0.5f);
  for (int i = 0; i < 2; i++) {
    int found = 0;
    t0 = now_ms();
    r = FMR_seek(idx, at100, 1, &found);
    FMR_set_mute(idx, 1);
    if (r != 0 || found <= 0) {
      printf("s4: seek -> %d, nothing found\n", r);
      break;
    }
    at100 = found;
    FMR_get_rssi(idx, &rssi);
    FMR_get_stereomono(idx, &stereo);
    printf("s4: seek found %.2f MHz in %lld ms: rssi %d, %s\n", found / 100.0, (long long)(now_ms() - t0), rssi,
           stereo ? "stereo" : "mono");
  }

  // RDS: the station's name (PS), if it sends one
  FMR_turn_on_off_rds(idx, 1);
  int64_t until = now_ms() + 6000;
  char ps[16] = {0};
  while (now_ms() < until && !ps[0]) {
    uint16_t status = 0;
    if (FMR_read_rds_data(idx, &status) == 0) {
      uint8_t* p = nullptr;
      int len = 0;
      if (FMR_get_ps(idx, &p, &len) == 0 && p && len > 0) {
        memcpy(ps, p, len < 15 ? len : 15);
      }
    }
    usleep(100000);
  }
  printf("s4: RDS name: %s\n", ps[0] ? ps : "(none within 6 s)");
  FMR_turn_on_off_rds(idx, 0);

  // the FM audio path, switched the way MediaTek's FM player does it; the tuner stays muted
  if (audio) {
  r = AudioSystem::setParameters(0, String8("AudioSetFmEnable=1"));
  String8 state = AudioSystem::getParameters(0, String8("GetFmEnable"));
  printf("s4: AudioSetFmEnable=1 -> %d; GetFmEnable says \"%s\"\n", r, state.string());
  usleep(500000);
  r = AudioSystem::setParameters(0, String8("AudioSetFmEnable=0"));
  state = AudioSystem::getParameters(0, String8("GetFmEnable"));
  printf("s4: AudioSetFmEnable=0 -> %d; GetFmEnable says \"%s\"\n", r, state.string());
  }

  t0 = now_ms();
  FMR_pwr_down(idx, 0);
  FMR_close_dev(idx);
  printf("s4: powered down (%lld ms)\n", (long long)(now_ms() - t0));
  return 0;
}
