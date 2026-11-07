// rondo-core's Wi-Fi service (wifi.cpp).
#pragma once
#include <pthread.h>

#include <atomic>
#include <deque>
#include <functional>
#include <string>

#include "core.h"

namespace rondo {

class Wifi {
 public:
  // publish: runs a change on the shared state and makes it visible; snapshot: the state now
  using Publish = std::function<void(const std::function<void(CoreState&)>&)>;
  explicit Wifi(Publish publish);
  std::function<CoreState()> snapshot_;

  void start();  // takes over the running supplicant, or turns Wi-Fi on as Android last had it
  void enable(bool on);
  void scan();
  void connect(int id);
  void join(const std::string& ssid, WifiSecurity security, const std::string& password);
  void forget(int id);

 private:
  // one worker runs every job in order (the command loop never blocks); the event thread waits for
  // the supplicant's events
  void post(std::function<void()> job);
  void workLoop();
  void eventLoop();
  bool command(const std::string& c, std::string* reply = nullptr);
  void update(const std::function<void(CoreState&)>& f);
  void turnOn();
  void turnOff();
  void attached();
  void getAddress();
  void configure(const char* ip, const char* gw, uint32_t prefix, const char* dns1, const char* dns2);
  void lostAddress();
  void poll();
  void readScanResults();
  void readSaved();

  Publish publish_;
  pthread_mutex_t mu_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv_ = PTHREAD_COND_INITIALIZER;
  std::deque<std::function<void()>> jobs_;
  std::atomic<bool> monitoring_{false};
  bool enableAllOnConnect_ = false;
  std::string eventSsid_;  // the network named by the last state change (the event thread's)
  int64_t renewAt_ = 0;
};

}  // namespace rondo
