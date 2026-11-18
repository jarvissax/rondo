// mictest: the microphone recorded as WhatsApp's voice messages record it (player/recorder.cpp), to test
// the Y1's microphones (development):
//
//   mictest <seconds> <out.wav> [source=N]
//
// source: Android's audio source (1 MIC, 5 CAMCORDER, 6 VOICE_RECOGNITION...); without it, the
// recorder's own choice (the built-in mic, whatever is in the jack). With a headset in, source=1 records
// the headset input, which on the Y1 gives only a steady 973 Hz pulse train (recorder.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../player/recorder.h"
#include "../platform/aosp_begin.h"
#include <binder/ProcessState.h>
#include "../platform/aosp_end.h"

namespace {

// 0: nothing in the jack, 1: a headset with a microphone, 2: headphones without one
int jack() {
  int state = -1;
  if (FILE* f = fopen("/sys/class/switch/h2w/state", "r")) {
    if (fscanf(f, "%d", &state) != 1) state = -1;
    fclose(f);
  }
  return state;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: mictest <seconds> <out.wav> [source=N]\n");
    return 2;
  }
  int seconds = atoi(argv[1]);
  int source = -1;  // the recorder's choice
  for (int i = 3; i < argc; i++)
    if (!strncmp(argv[i], "source=", 7)) source = atoi(argv[i] + 7);
  android::ProcessState::self()->startThreadPool();
  printf("jack: %d, source %d\n", jack(), source);
  rondo::Recorder rec([] {});
  if (!rec.start(argv[2], source)) {
    fprintf(stderr, "mictest: the microphone can't be recorded (%d)\n", rec.error());
    return 1;
  }
  sleep(unsigned(seconds));
  int64_t ms = rec.ms();
  rec.stop(true);
  printf("recorded %lld ms to %s\n", (long long)ms, argv[2]);
  return 0;
}
