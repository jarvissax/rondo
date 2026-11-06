// N0 test S3, part 2: A2DP without Java.
//
// Run with the Java layer stopped and the headset already paired (s3 ... "<name>"). It does what
// MediaTek's BluetoothA2dpService JNI (libandroid_runtime.so, read with REA) does with the C library
// libmtkbtextadpa2dp.so, then routes music to the headset and plays a quiet tone:
//
//   1. bind the abstract datagram socket "bt.ext.adp.a2dp" (mtkbt's replies), pass its address with
//      btmtk_a2dp_setSockAddress, open an auto-bound API socket connected to /dev/socket/bt.int.adp
//   2. btmtk_a2dp_send_activate_req(reply socket, api socket); a thread feeds every reply to
//      btmtk_a2dp_handle_message
//   3. btmtk_a2dp_send_stream_open_req(0, &address, 1): connect
//   4. publish our "permission" service, then AudioSystem::setDeviceConnectionState(A2DP, available)
//   5. a quiet 440 Hz tone for 3 s through OpenSL ES (about -26 dBFS, player volume -30 dB)
//   6. AudioSystem::setDeviceConnectionState(A2DP, unavailable), btmtk_a2dp_close_device
//
//   s3a2dp <address AA:BB:CC:DD:EE:FF> [seconds of tone] [seconds to log the headset's AVRCP buttons]

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "../../platform/aosp_begin.h"
#include <binder/ProcessState.h>
#include <media/AudioSystem.h>
#include "../../platform/permission_service.h"
#include "../../platform/aosp_end.h"

// libmtkbtextadpa2dp.so, as BluetoothA2dpService.cpp calls it
extern "C" {
void btmtk_a2dp_setSockAddress(void* addr, int len);
void btmtk_a2dp_send_activate_req(int replySock, int apiSock);
void btmtk_a2dp_send_stream_open_req(int index, void* addr, int local);
int btmtk_a2dp_handle_message(void* ilm);
int btmtk_a2dp_get_ap_state();
void btmtk_a2dp_close_device(void* addr);
}

namespace {

int64_t now_ms() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return int64_t(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

int g_reply = -1;
volatile bool g_stop = false;
volatile int g_state = 0;  // btmtk_a2dp_handle_message: 1 disconnected, 2 connecting, 3 connected, 4 playing
int64_t g_t0 = 0;

// AVRCP, as libextavrcp_jni.so (MtkBt) and libextavrcp.so do it: replies on the abstract
// "bt.ext.adp.avrcp", requests from a socket connected to mtkbt, in the same ILM format (payload from
// +0x20). activate 500 (index, 0, version 14, SDP features 1), answered by 0x1f5; a headset connecting
// arrives as 0x1fa (channel +0x20) and is accepted with 0x1fb (channel, 1); each button arrives as
// 0x207 (channel +0x20, sequence +0x21, frame length +0x26, profile +0x28, frame from +0x2a, the
// AVRCP operation at +0x2b with bit 7 set on release) and is acknowledged with 0x208 ACCEPTED.
int g_avrcpReply = -1, g_avrcpApi = -1;

void avrcp_send(uint32_t id, uint8_t* m, uint16_t len) {
  memcpy(m + 4, &id, 4);
  uint32_t two = 2, zero = 0;
  memcpy(m + 0x0c, &two, 4);
  memcpy(m + 0x10, &zero, 4);
  m[0x14] = 0;
  uint16_t one = 1;
  memcpy(m + 0x1c, &one, 2);
  memcpy(m + 0x1e, &len, 2);
  send(g_avrcpApi, m, len + 0x1c, 0);
}

const char* avrcp_key(int op) {
  switch (op) {
    case 0x41: return "volume up";
    case 0x42: return "volume down";
    case 0x44: return "play";
    case 0x45: return "stop";
    case 0x46: return "pause";
    case 0x4b: return "next";
    case 0x4c: return "previous";
    case 0x49: return "fast forward";
    case 0x48: return "rewind";
    default: return "?";
  }
}

void avrcp_handle(uint8_t* m, ssize_t n) {
  static uint8_t r[0x81c];
  uint32_t id;
  memcpy(&id, m + 4, 4);
  long long at = (long long)(now_ms() - g_t0);
  if (id == 0x1f5) {
    printf("s3a2dp:   +%5lld ms  AVRCP active (result %u)\n", at, m[0x20]);
  } else if (id == 0x1fa) {
    printf("s3a2dp:   +%5lld ms  AVRCP connection from the headset: accepting\n", at);
    memset(r, 0, sizeof(r));
    r[0x20] = m[0x20];
    r[0x21] = 1;
    avrcp_send(0x1fb, r, 8);
  } else if (id == 0x207) {
    uint16_t len;
    memcpy(&len, m + 0x26, 2);
    if (len == 3) {
      int op = m[0x2b] & 0x7f;
      bool released = m[0x2b] & 0x80;
      printf("s3a2dp:   +%5lld ms  AVRCP button 0x%02x (%s) %s\n", at, op, avrcp_key(op), released ? "released" : "pressed");
    } else {
      printf("s3a2dp:   +%5lld ms  AVRCP command frame, %u bytes\n", at, len);
    }
    memset(r, 0, sizeof(r));
    r[0x20] = m[0x20];  // channel
    r[0x21] = m[0x21];  // sequence
    r[0x22] = 9;        // ACCEPTED
    r[0x23] = 9;        // panel subunit
    r[0x24] = 0;
    memcpy(r + 0x26, &len, 2);
    memcpy(r + 0x28, m + 0x28, 2);
    if (len && len <= 200) memcpy(r + 0x2a, m + 0x2a, len);
    avrcp_send(0x208, r, 0xd6);
  } else {
    printf("s3a2dp:   +%5lld ms  AVRCP message 0x%x (%zd bytes)\n", at, id, n);
  }
}

bool avrcp_start() {
  g_avrcpReply = socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un me = {};
  me.sun_family = AF_UNIX;
  const char* name = "bt.ext.adp.avrcp";
  memcpy(me.sun_path + 1, name, strlen(name));
  if (bind(g_avrcpReply, (sockaddr*)&me, socklen_t(offsetof(sockaddr_un, sun_path) + 1 + strlen(name))) != 0) return false;
  g_avrcpApi = socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un autobind = {};
  autobind.sun_family = AF_UNIX;
  bind(g_avrcpApi, (sockaddr*)&autobind, sizeof(sa_family_t));
  sockaddr_un mtkbt = {};
  mtkbt.sun_family = AF_UNIX;
  strcpy(mtkbt.sun_path, "/dev/socket/bt.int.adp");
  if (connect(g_avrcpApi, (sockaddr*)&mtkbt, sizeof(mtkbt)) != 0) return false;
  static uint8_t m[0x81c];
  memset(m, 0, sizeof(m));
  m[0x20] = 0;   // index
  m[0x21] = 0;
  m[0x22] = 14;  // AVRCP 1.4, as MtkBt activates it
  m[0x23] = 1;   // SDP features
  avrcp_send(500, m, 8);
  return true;
}

void* event_loop(void*) {
  static uint8_t m[0x81c];
  while (!g_stop) {
    pollfd p[2] = {{g_reply, POLLIN, 0}, {g_avrcpReply, POLLIN, 0}};
    if (poll(p, g_avrcpReply >= 0 ? 2 : 1, 200) <= 0) continue;
    if (g_avrcpReply >= 0 && (p[1].revents & POLLIN)) {
      ssize_t an = recv(g_avrcpReply, m, sizeof(m), 0);
      if (an >= 8) avrcp_handle(m, an);
      continue;
    }
    if (!(p[0].revents & POLLIN)) continue;
    ssize_t n = recv(g_reply, m, sizeof(m), 0);
    if (n < 8) continue;
    uint32_t id;
    memcpy(&id, m + 4, 4);
    int r = btmtk_a2dp_handle_message(m);
    if (r >= 1 && r <= 4) g_state = r;
    printf("s3a2dp:   +%5lld ms  A2DP message 0x%x (%zd bytes) -> %d, state %d\n", (long long)(now_ms() - g_t0), id, n,
           r, btmtk_a2dp_get_ap_state());
  }
  return nullptr;
}

// "AA:BB:CC:DD:EE:FF" to MediaTek's bt_addr_struct (LAP u32, UAP, pad, NAP u16)
bool parse_addr(const char* s, uint8_t out[8]) {
  unsigned b[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
  memset(out, 0, 8);
  uint32_t lap = (b[3] << 16) | (b[4] << 8) | b[5];
  memcpy(out, &lap, 4);
  out[4] = uint8_t(b[2]);
  uint16_t nap = uint16_t((b[0] << 8) | b[1]);
  memcpy(out + 6, &nap, 2);
  return true;
}

void tone(int seconds) {
  SLObjectItf engineObj, mixObj, playerObj;
  SLEngineItf engine;
  slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr);
  (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
  (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);
  (*engine)->CreateOutputMix(engine, &mixObj, 0, nullptr, nullptr);
  (*mixObj)->Realize(mixObj, SL_BOOLEAN_FALSE);
  SLDataLocator_AndroidSimpleBufferQueue locQ = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                          SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                          SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource src = {&locQ, &pcm};
  SLDataLocator_OutputMix locMix = {SL_DATALOCATOR_OUTPUTMIX, mixObj};
  SLDataSink sink = {&locMix, nullptr};
  const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
  const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  (*engine)->CreateAudioPlayer(engine, &playerObj, &src, &sink, 2, ids, req);
  (*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE);
  SLPlayItf play;
  SLAndroidSimpleBufferQueueItf queue;
  SLVolumeItf volume;
  (*playerObj)->GetInterface(playerObj, SL_IID_PLAY, &play);
  (*playerObj)->GetInterface(playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue);
  (*playerObj)->GetInterface(playerObj, SL_IID_VOLUME, &volume);
  (*volume)->SetVolumeLevel(volume, -3000);
  // the whole tone in one buffer: seconds of 440 Hz with a short fade in and out
  size_t frames = size_t(44100) * seconds;
  int16_t* buf = (int16_t*)calloc(frames * 2, sizeof(int16_t));
  for (size_t i = 0; i < frames; i++) {
    double env = std::min(1.0, double(std::min(i, frames - i)) / 4410.0);
    int16_t s = int16_t(sin(2 * M_PI * 440 * i / 44100.0) * 1600 * env);
    buf[2 * i] = buf[2 * i + 1] = s;
  }
  (*queue)->Enqueue(queue, buf, frames * 4);
  (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
  usleep(useconds_t(seconds) * 1000000 + 300000);
  SLmillisecond pos = 0;
  (*play)->GetPosition(play, &pos);
  printf("s3a2dp: tone played, position %u ms\n", (unsigned)pos);
  (*playerObj)->Destroy(playerObj);
  (*mixObj)->Destroy(mixObj);
  (*engineObj)->Destroy(engineObj);
  free(buf);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    fprintf(stderr, "usage: s3a2dp <AA:BB:CC:DD:EE:FF> [seconds]\n");
    return 2;
  }
  int seconds = argc > 2 ? atoi(argv[2]) : 3;
  int buttonSeconds = argc > 3 ? atoi(argv[3]) : 0;
  uint8_t addr[8];
  if (!parse_addr(argv[1], addr)) return 2;
  android::ProcessState::self()->startThreadPool();
  g_t0 = now_ms();

  // 1. mtkbt's replies on the abstract "bt.ext.adp.a2dp"
  g_reply = socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un me = {};
  me.sun_family = AF_UNIX;
  const char* name = "bt.ext.adp.a2dp";
  memcpy(me.sun_path + 1, name, strlen(name));
  socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + strlen(name));
  if (bind(g_reply, (sockaddr*)&me, len) != 0) {
    printf("s3a2dp: can't bind %s (%s)\n", name, strerror(errno));
    return 1;
  }
  btmtk_a2dp_setSockAddress(&me, len);
  int api = socket(AF_UNIX, SOCK_DGRAM, 0);
  timeval tv = {30, 0};
  setsockopt(api, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_un autobind = {};
  autobind.sun_family = AF_UNIX;
  bind(api, (sockaddr*)&autobind, sizeof(sa_family_t));  // the kernel picks an abstract name
  sockaddr_un mtkbt = {};
  mtkbt.sun_family = AF_UNIX;
  strcpy(mtkbt.sun_path, "/dev/socket/bt.int.adp");
  if (connect(api, (sockaddr*)&mtkbt, sizeof(mtkbt)) != 0) {
    printf("s3a2dp: can't reach mtkbt (%s)\n", strerror(errno));
    return 1;
  }
  pthread_t loop;
  pthread_create(&loop, nullptr, event_loop, nullptr);

  // AVRCP first, so the headset's AVRCP connection after A2DP is accepted
  printf("s3a2dp: AVRCP %s\n", avrcp_start() ? "activating" : "sockets failed");
  // 2. activate A2DP, 3. connect
  btmtk_a2dp_send_activate_req(g_reply, api);
  usleep(500000);
  printf("s3a2dp: connecting to %s\n", argv[1]);
  btmtk_a2dp_send_stream_open_req(0, addr, 1);
  int64_t until = now_ms() + 15000;
  while (now_ms() < until && g_state != 3) usleep(50000);
  printf("s3a2dp: %s after %lld ms\n", g_state == 3 ? "connected" : "not connected", (long long)(now_ms() - g_t0));
  if (g_state != 3) return 1;

  // 4. music to the headset: the audio policy needs to know an A2DP device is there
  if (android::defaultServiceManager()->checkService(android::String16("permission")) == nullptr)
    rondo::PermissionService::publish();
  int r = android::AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP,
                                                         AUDIO_POLICY_DEVICE_STATE_AVAILABLE, argv[1]);
  printf("s3a2dp: A2DP output available -> %d\n", r);
  // Android's audio service sets "force use for media" on every A2DP connection: none (A2DP allowed),
  // or no-A2DP when Bluetooth audio is off. Without it an old value can keep music on the wired output.
  printf("s3a2dp: force use for media was %d\n", android::AudioSystem::getForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA));
  android::AudioSystem::setForceUse(AUDIO_POLICY_FORCE_FOR_MEDIA, AUDIO_POLICY_FORCE_NONE);
  sleep(1);

  // 5. a quiet tone
  if (seconds > 0) tone(seconds);
  if (buttonSeconds > 0) {
    printf("s3a2dp: listening for the headset's buttons for %d s\n", buttonSeconds);
    sleep(buttonSeconds);
  }

  // 6. undo
  android::AudioSystem::setDeviceConnectionState(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP,
                                                 AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE, argv[1]);
  btmtk_a2dp_close_device(addr);
  sleep(2);
  g_stop = true;
  pthread_join(loop, nullptr);
  printf("s3a2dp: disconnected, state %d\n", btmtk_a2dp_get_ap_state());
  return 0;
}
