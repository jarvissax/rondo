// FmRadio (fm.h): every chip call on one worker thread, as 1.0.x's FmRadio did; the sound recorded on a
// thread of its own.
#include "../core/clock.h"
#include "fm.h"

#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "../platform/aosp_begin.h"
#include <media/AudioRecord.h>
#include <media/AudioSystem.h>
#include <utils/String8.h>
#include "../platform/aosp_end.h"

// libfmjni.so's FMR_* functions (C++ names, plain arguments; units from its JNI wrappers, read with REA)
int FMR_init();
int FMR_open_dev(int idx);
int FMR_close_dev(int idx);
int FMR_pwr_up(int idx, int freq10);
int FMR_pwr_down(int idx, int type);
int FMR_tune(int idx, int freq10);
int FMR_seek(int idx, int freq100, int dir, int* found100);
int FMR_scan(int idx, uint16_t* list, int* count);
int FMR_Pre_Search(int idx);
int FMR_Restore_Search(int idx);
int FMR_get_rssi(int idx, int* rssi);
int FMR_get_stereomono(int idx, int* stereo);
int FMR_set_mute(int idx, int mute);
int FMR_is_rdsrx_support(int idx, int* supported);
int FMR_turn_on_off_rds(int idx, int on);
int FMR_read_rds_data(int idx, uint16_t* status);
int FMR_get_ps(int idx, uint8_t** ps, int* len);
int FMR_get_rt(int idx, uint8_t** rt, int* len);

namespace rondo {
namespace {
const bool kFmLog = getenv("RONDO_FM_LOG") != nullptr;  // development: the radio's steps
}  // namespace
namespace {

// RDS text is ISO-8859-1, padded with NULs and spaces: UTF-8, single-spaced, trimmed
std::string rdsText(const uint8_t* p, int len) {
  std::string out;
  bool space = false;
  for (int i = 0; i < len; i++) {
    uint8_t c = p[i];
    if (c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ', space = false;
    if (c < 0x80) out += char(c);
    else out += char(0xc0 | (c >> 6)), out += char(0x80 | (c & 0x3f));
  }
  return out;
}

}  // namespace

bool FmRadio::headphonesIn() {
  if (getenv("RONDO_FM_NO_ANTENNA")) return true;  // development: the tuner without headphones in
  char b[8] = {0};
  int fd = open("/sys/class/switch/h2w/state", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return true;  // nothing to tell: let the chip try
  ssize_t n = read(fd, b, sizeof(b) - 1);
  close(fd);
  return n > 0 && atoi(b) != 0;
}

FmStatus FmRadio::status() const {
  pthread_mutex_lock(&mu_);
  FmStatus s = st_;
  pthread_mutex_unlock(&mu_);
  return s;
}

void FmRadio::post(std::function<void()> job) {
  pthread_mutex_lock(&mu_);
  if (!started_) {
    started_ = true;
    pthread_t t;
    pthread_create(&t, nullptr, run, this);
    pthread_detach(t);
  }
  jobs_.push_back(std::move(job));
  pthread_cond_signal(&cv_);
  pthread_mutex_unlock(&mu_);
}

void* FmRadio::run(void* self) {
  prctl(PR_SET_NAME, "rondo-fm");
  setpriority(PRIO_PROCESS, 0, 0);
  static_cast<FmRadio*>(self)->loop();
  return nullptr;
}

// the jobs in order; once a second while on, the signal, stereo and RDS
void FmRadio::loop() {
  for (;;) {
    pthread_mutex_lock(&mu_);
    while (jobs_.empty()) {
      if (waitUntil(&cv_, &mu_, now_ns() + kSec) != 0) break;
    }
    std::function<void()> job;
    if (!jobs_.empty()) job = std::move(jobs_.front()), jobs_.pop_front();
    pthread_mutex_unlock(&mu_);
    if (job) job();
    else readStatus();
  }
}

void FmRadio::start(int frequency) {
  if (active_) return;
  if (!headphonesIn()) return update([](FmStatus& s) { s.error = 1; });
  active_ = true;
  update([&](FmStatus& s) {
    s.starting = true, s.error = 0, s.frequency = frequency;
  });
  post([this, frequency] {
    if (idx_ < 0) idx_ = FMR_init();
    bool ok = FMR_open_dev(idx_) == 0 && FMR_pwr_up(idx_, frequency) == 0;
    if (!ok) {
      FMR_close_dev(idx_);
      active_ = false;
      fprintf(stderr, "rondo-player: the FM chip didn't start\n");
      return update([](FmStatus& s) { s.starting = false, s.error = 2; });
    }
    // as the stock player does: muted while the sound path starts (no pop), then unmuted
    FMR_set_mute(idx_, 1);
    int rds = 0;
    rds_ = FMR_is_rdsrx_support(idx_, &rds) == 0 && rds == 1;
    if (rds_) FMR_turn_on_off_rds(idx_, 1);
    startSound();
    FMR_set_mute(idx_, 0);
    update([](FmStatus& s) {
      s.on = true, s.starting = false, s.muted = false;
      s.name.clear(), s.text.clear();
    });
  });
}

void FmRadio::stop() {
  if (!active_) return;
  post([this] {
    stopSound();
    FMR_set_mute(idx_, 1);
    FMR_pwr_down(idx_, 0);
    FMR_close_dev(idx_);
    active_ = false;
    update([](FmStatus& s) {
      s.on = s.starting = s.seeking = s.scanning = s.stereo = false;
      s.name.clear(), s.text.clear();
      s.signal = 0;
    });
  });
}

void FmRadio::tune(int frequency) {
  update([&](FmStatus& s) {
    s.frequency = frequency;
    s.name.clear(), s.text.clear();
  });
  post([this, frequency] {
    if (status().on) FMR_tune(idx_, frequency);
  });
}

void FmRadio::seek(bool up, int min, int max) {
  FmStatus now = status();
  if (!now.on || now.seeking) return;
  update([](FmStatus& s) { s.seeking = true; });
  post([this, up, min, max] {
    int found = 0;
    int from = status().frequency;
    int r = FMR_seek(idx_, from * 10, up ? 1 : 0, &found);
    if (kFmLog) fprintf(stderr, "fm: seek %s from %d -> %d, found %d\n", up ? "up" : "down", from, r, found);
    int f = int(lroundf(found / 10.f));
    if (f >= min && f <= max) {
      FMR_tune(idx_, f);
      update([&](FmStatus& s) {
        s.frequency = f, s.seeking = false, s.seekSerial++;
        s.name.clear(), s.text.clear();
      });
    } else {
      update([](FmStatus& s) { s.seeking = false, s.seekSerial++; });
    }
  });
}

// every station in the band (about 10-20 s); the chip stops on the last it found: back to ours
void FmRadio::scan(int min, int max) {
  FmStatus now = status();
  if (!now.on || now.scanning) return;
  update([](FmStatus& s) { s.scanning = true; });
  post([this, min, max] {
    uint16_t list[200];
    int count = 200;
    FMR_Pre_Search(idx_);
    int r = FMR_scan(idx_, list, &count);
    if (r >= 0) FMR_Restore_Search(idx_);
    std::vector<int> found;
    for (int i = 0; r >= 0 && i < count && i < 200; i++)
      if (list[i] >= min && list[i] <= max) found.push_back(list[i]);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    fprintf(stderr, "rondo-player: the FM scan found %zu stations\n", found.size());
    FMR_tune(idx_, status().frequency);
    update([&](FmStatus& s) {
      s.scanning = false;
      s.found = found;
      s.scanSerial++;
    });
  });
}

void FmRadio::setMuted(bool muted) {
  if (!status().on) return;
  update([&](FmStatus& s) { s.muted = muted; });
  post([this, muted] { FMR_set_mute(idx_, muted); });
}

namespace {
int64_t nowMsFm() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}
}  // namespace

void FmRadio::readStatus() {
  FmStatus now = status();
  if (!now.on) return;
  // the antenna is the headphone cable: unplugged, the radio stops
  if (!headphonesIn()) return stop();
  int rssi = -120, stereo = 0;
  FMR_get_rssi(idx_, &rssi);
  FMR_get_stereomono(idx_, &stereo);
  if (kFmLog) fprintf(stderr, "fm: %d rssi %d stereo %d\n", now.frequency, rssi, stereo);
  int signal = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : rssi >= -90 ? 1 : 0;
  // the sound watchdog (FmRadio.kt): MediaTek's FM sound switch is one for the whole system, and
  // the record stream can stall; switched off, or no sound for 2 s, the path starts again, muted
  if (!now.seeking && !now.scanning && rec_) {
    int64_t quiet = nowMsFm() - lastSound_.load();
    android::String8 v = android::AudioSystem::getParameters(0, android::String8("GetFmEnable"));
    bool off = strstr(v.string(), "=false") != nullptr;
    if (off || quiet > 2000) {
      fprintf(stderr, "rondo-player: the FM sound stopped (%s), starting it again\n", off ? "switched off" : "no sound");
      FMR_set_mute(idx_, 1);
      stopSound();
      startSound();
      FMR_set_mute(idx_, now.muted);
    }
  }
  std::string name = now.name, text = now.text;
  if (rds_) {
    uint16_t st = 0;
    FMR_read_rds_data(idx_, &st);
    uint8_t* p = nullptr;
    int len = 0;
    if (FMR_get_ps(idx_, &p, &len) == 0 && p && len > 0) {
      std::string n = rdsText(p, std::min(len, 64));
      if (!n.empty()) name = n;
    }
    p = nullptr, len = 0;
    if (FMR_get_rt(idx_, &p, &len) == 0 && p && len > 0) {
      std::string t = rdsText(p, std::min(len, 128));
      if (!t.empty()) text = t;
    }
  }
  if (signal != now.signal || (stereo != 0) != now.stereo || name != now.name || text != now.text)
    update([&](FmStatus& s) { s.signal = signal, s.stereo = stereo != 0, s.name = name, s.text = text; });
}

// The digital FM path on, and its stream recorded (source 99, as MediaTek's FMAudioPlayer records it)
void FmRadio::startSound() {
  android::AudioSystem::setParameters(0, android::String8("AudioSetFmDigitalEnable=1"));
  auto* rec = new (::operator new(sizeof(android::AudioRecord) + 256))
      android::AudioRecord((audio_source_t)99, 44100, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_IN_STEREO, 0);
  rec->incStrong(this);
  if (rec->initCheck() != 0 || rec->start() != 0) {
    fprintf(stderr, "rondo-player: the FM sound can't be recorded (%d)\n", rec->initCheck());
    rec->decStrong(this);
    return;
  }
  rec_ = rec;
  lastSound_ = nowMsFm();  // the watchdog's count starts now
  recording_ = true;
  sounding_ = true;
  pthread_create(&recThread_, nullptr, recordThread, this);
  changed_();
}

void FmRadio::stopSound() {
  if (!rec_) return;
  recording_ = false;
  auto* rec = static_cast<android::AudioRecord*>(rec_);
  rec->stop();  // first: a read waiting for sound returns
  pthread_join(recThread_, nullptr);
  rec->decStrong(this);
  rec_ = nullptr;
  sounding_ = false;
  android::AudioSystem::setParameters(0, android::String8("AudioSetFmDigitalEnable=0"));
  changed_();
}

void* FmRadio::recordThread(void* self) {
  prctl(PR_SET_NAME, "rondo-fmrec");
  setpriority(PRIO_PROCESS, 0, -8);
  static_cast<FmRadio*>(self)->record();
  return nullptr;
}

void FmRadio::record() {
  auto* rec = static_cast<android::AudioRecord*>(rec_);
  static int16_t buf[2048 * 2];
  while (recording_) {
    ssize_t n = rec->read(buf, sizeof(buf));
    if (n <= 0) {
      usleep(5000);
      continue;
    }
    lastSound_ = nowMsFm();
    sound_(buf, size_t(n) / 4);
  }
}

}  // namespace rondo
