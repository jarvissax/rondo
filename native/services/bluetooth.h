// rondo-core's Bluetooth service (bluetooth.cpp): what Android's BluetoothService, BluetoothA2dpService
// and MtkBt did for 1.0.x, talking to MediaTek's mtkbt daemon directly as S3 proved.
//
// The adapter (power, searching, pairing, forgetting) is ILM messages to mtkbt's socket, its replies and
// events on the abstract socket "bt.ext.adp". A2DP is MediaTek's libmtkbtextadpa2dp.so over
// "bt.ext.adp.a2dp"; a connection is routed to (AudioSystem's A2DP output, "force use for media" none)
// as Android's audio service did. AVRCP (the headset's buttons) over "bt.ext.adp.avrcp".
//
// Paired devices are mtkbt's own (/data/@btmtk/devdb, classes in /data/@btmtk/cod). mtkbt keeps no
// names, so Rondo keeps the names it has seen, and the headphones last connected, in a file of its
// own: when Bluetooth comes on, those are connected again (Android's A2DP auto-connect).
#pragma once
#include <pthread.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core.h"

namespace rondo {

class Bluetooth {
 public:
  using Publish = std::function<void(const std::function<void(CoreState&)>&)>;
  explicit Bluetooth(Publish publish) : publish_(std::move(publish)) {}

  void configure(const std::string& stateFile);
  void power(bool on);
  void search(bool on);
  void pair(const std::string& address);
  void connect(const std::string& address);
  void disconnect(const std::string& address);
  void forget(const std::string& address);

 private:
  struct Dev {
    uint8_t addr[8];  // MediaTek's bt_addr_struct: LAP (u32), UAP, a pad byte, NAP (u16)
    std::string address, name;
    uint32_t cod = 0;
    int rssi = 0;
    bool paired = false, found = false, pairing = false;
    int a2dp = 0;  // 0 not connected, 1 connecting, 2 connected
  };

  void post(std::function<void()> job);
  static void* run(void* self);
  void loop();
  bool open();  // the sockets, once
  void sendGap(uint32_t id, uint8_t* m, uint16_t payload);
  void sendAvrcp(uint32_t id, uint8_t* m, uint16_t payload);
  void onGap(uint8_t* m, ssize_t n);
  void onA2dp(uint8_t* m, ssize_t n);
  void onAvrcp(uint8_t* m, ssize_t n);
  void a2dpState(int state);
  void route(bool on, const std::string& address);
  void startSearch();
  void stopSearch();
  void readPaired();
  Dev* find(const uint8_t* addr);
  Dev& add(const uint8_t* addr);
  void learnName(const uint8_t* addr, const char* name, int len);
  void load();
  void save();
  void publish();

  Publish publish_;
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
  std::deque<std::function<void()>> jobs_;
  int wake_[2] = {-1, -1};
  bool started_ = false;

  // the Bluetooth thread's
  std::string stateFile_;
  int gap_ = -1, a2dp_ = -1, a2dpApi_ = -1, avrcp_ = -1, avrcpApi_ = -1;
  int power_ = 0;  // as CoreState::btPower
  int64_t powerSince_ = 0;
  bool a2dpActive_ = false, avrcpActive_ = false;
  bool wantSearch_ = false, searching_ = false;
  int cancels_ = 0;  // cancels sent whose answers are still to come (they don't end a later search)
  int64_t searchSince_ = 0;
  std::vector<Dev> devs_;
  std::map<std::string, std::string> names_;  // by address, kept in the state file
  std::string last_;                          // the headphones last connected
  std::string routed_;                        // the A2DP output given to the audio policy
  std::string pendingConnect_;                // to connect once the other one has gone
  int a2dpStateNow_ = 1;
  int64_t connectSince_ = 0;  // a connection asked for (given up after 25 s)
  uint32_t keySerial_ = 0;
  int key_ = 0;
};

}  // namespace rondo
