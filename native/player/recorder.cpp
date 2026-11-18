// Recorder (recorder.h): AudioRecord from the built-in microphone into a WAV file, as FM radio records
// its stream (fm.cpp), but from AUDIO_SOURCE_MIC.
#include "recorder.h"

#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>

#include "../platform/aosp_begin.h"
#include <media/AudioRecord.h>
#include "../platform/aosp_end.h"

namespace rondo {
namespace {

void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24); }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }

// RIFF/WAVE, PCM 16-bit mono at the recorder's rate, for `bytes` of samples
void header(uint8_t h[44], uint32_t bytes) {
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);  // PCM
  put16(h + 22, 1);  // mono
  put32(h + 24, Recorder::kRate);
  put32(h + 28, Recorder::kRate * 2);
  put16(h + 32, 2);
  put16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  put32(h + 40, bytes);
}

// 1 in h2w: a headset with a microphone (2: headphones without one, 0: nothing)
bool headsetMicIn() {
  int state = 0;
  if (FILE* f = fopen("/sys/class/switch/h2w/state", "r")) {
    if (fscanf(f, "%d", &state) != 1) state = 0;
    fclose(f);
  }
  return state == 1;
}

}  // namespace

bool Recorder::start(const std::string& path, int source) {
  stop(false);
  error_ = 0;
  frames_ = 0;
  level_ = 0;
  file_ = fopen(path.c_str(), "wb");
  if (!file_) {
    error_ = -1;
    changed_();
    return false;
  }
  uint8_t h[44];
  header(h, 0);
  fwrite(h, 1, sizeof(h), file_);
  path_ = path;
  if (source < 0) source = headsetMicIn() ? 5 /* AUDIO_SOURCE_CAMCORDER */ : 1 /* AUDIO_SOURCE_MIC */;
  auto* rec = new (::operator new(sizeof(android::AudioRecord) + 256))
      android::AudioRecord(audio_source_t(source), kRate, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_IN_MONO, 0);
  rec->incStrong(this);
  int check = rec->initCheck();
  if (check != 0 || rec->start() != 0) {
    fprintf(stderr, "rondo-player: the microphone can't be recorded (%d)\n", check);
    rec->decStrong(this);
    fclose(file_);
    file_ = nullptr;
    unlink(path.c_str());
    error_ = check ? check : -2;
    changed_();
    return false;
  }
  rec_ = rec;
  recording_ = true;
  pthread_create(&thread_, nullptr, run, this);
  changed_();
  return true;
}

void Recorder::stop(bool keep) {
  if (!rec_) return;
  recording_ = false;
  auto* rec = static_cast<android::AudioRecord*>(rec_);
  rec->stop();  // first: a read waiting for sound returns
  pthread_join(thread_, nullptr);
  rec->decStrong(this);
  rec_ = nullptr;
  if (file_) {
    uint32_t bytes = uint32_t(frames_.load() * 2);
    uint8_t h[44];
    header(h, bytes);
    fseek(file_, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), file_);
    fclose(file_);
    file_ = nullptr;
    if (!keep) unlink(path_.c_str());
  }
  level_ = 0;
  changed_();
}

void* Recorder::run(void* self) {
  prctl(PR_SET_NAME, "rondo-mic");
  setpriority(PRIO_PROCESS, 0, -8);
  static_cast<Recorder*>(self)->loop();
  return nullptr;
}

void Recorder::loop() {
  auto* rec = static_cast<android::AudioRecord*>(rec_);
  static int16_t buf[kRate / 10];  // a tenth of a second
  size_t have = 0;
  int peak = 0;
  while (recording_) {
    ssize_t n = rec->read(buf + have, (sizeof(buf) / 2 - have) * 2);
    if (n <= 0) {
      usleep(5000);
      continue;
    }
    size_t got = size_t(n) / 2;
    for (size_t i = have; i < have + got; i++) peak = std::max(peak, abs(int(buf[i])));
    have += got;
    if (have < sizeof(buf) / 2) continue;
    if (file_) fwrite(buf, 2, have, file_);
    frames_ += int64_t(have);
    level_ = std::min(100, peak * 100 / 16384);
    have = 0;
    peak = 0;
    changed_();
    if (ms() >= kMaxMs) recording_ = false;  // the UI sees it stopped and sends what there is
  }
}

}  // namespace rondo
