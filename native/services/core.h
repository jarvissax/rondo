// rondo-core: the system services Android's system_server ran for 1.0.x, natively, in a process of their
// own (forked at startup like rondo-player): Wi-Fi first; Bluetooth, FM, the dashboard, power and the
// server's API follow (private/native-plan.md, N4). The drawing process never waits on them: their
// state is in shared memory under a sequence lock, read every frame without a system call, and
// commands go over a socket.
#pragma once
#include <stdint.h>
#include <sys/types.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rondo {

constexpr int kMaxWifiNetworks = 48, kMaxWifiSaved = 48, kMaxBtDevices = 32;

enum class WifiSecurity : uint8_t { Open, Wep, Psk, Eap };

struct WifiNetwork {  // one per name, the strongest access point
  char ssid[33];
  uint8_t security;  // WifiSecurity
  int16_t rssi;      // dBm
};

struct WifiSaved {
  int32_t id;  // wpa_supplicant's network id
  char ssid[33];
};

struct BtDevice {  // a paired device, or one a search found
  char address[18];  // "AA:BB:CC:DD:EE:FF"
  char name[64];     // "" until known
  uint32_t cod;      // the class of device
  int8_t rssi;
  uint8_t paired, pairing;
  uint8_t a2dp;      // 0 not connected, 1 connecting, 2 connected (music plays there)
};

// Written by rondo-core under a sequence lock: read with CoreHost::state().
struct CoreState {
  uint32_t seq;
  uint32_t serial;  // changes whenever anything below does
  // Wi-Fi
  int32_t wifiEnabled;  // 0 off, 1 turning on, 2 on, 3 turning off
  int32_t wifiState;    // 0 not connected, 1 connecting, 2 checking the password, 3 getting an address, 4 connected
  char ssid[33];        // the network being joined or joined
  char failedSsid[33];  // the last attempt's wrong password
  int32_t rssi, linkSpeed;  // dBm, Mb/s while connected
  uint32_t ip;              // network order, while connected
  int32_t networkCount, savedCount;
  WifiNetwork networks[kMaxWifiNetworks];  // strongest first
  WifiSaved saved[kMaxWifiSaved];
  // Bluetooth
  int32_t btPower;      // 0 off, 1 turning on, 2 on, 3 turning off; -1: the stack can't be reached
  int32_t btSearching;
  int32_t btCount;
  uint32_t btKeySerial;  // a headset's button (AVRCP): the operation in btKey, the serial moves on
  int32_t btKey;
  BtDevice bt[kMaxBtDevices];  // paired ones and found ones, in the order they came
};

class CoreHost {
 public:
  // Forks rondo-core: call before any other thread exists.
  bool start();
  CoreState state() const;  // a consistent copy

  void wifiEnable(bool on);
  void wifiScan();
  void wifiConnect(int id);  // a saved network
  void wifiJoin(const std::string& ssid, WifiSecurity security, const std::string& password);
  void wifiForget(int id);

  // Bluetooth: where the device names and the last headphones are kept; on/off; searching (only while
  // the Bluetooth screen is shown); pairing, connecting, forgetting by address
  void btConfigure(const std::string& stateFile);
  void btPower(bool on);
  void btSearch(bool on);
  void btPair(const std::string& address);
  void btConnect(const std::string& address);
  void btDisconnect(const std::string& address);
  void btForget(const std::string& address);

  // The server's API (core/api.h), fetched by rondo-core: the reply comes to `done` on the UI thread
  // from pump(). cacheMs: answered from rondo-core's memory if fetched that recently.
  using Reply = std::function<void(int status, const std::string& body)>;
  // cacheDir: where the answers kept for a while are on disk too ("": memory only)
  void apiConfigure(const std::string& baseUrl, const std::string& token, const std::string& cacheDir = "");
  uint32_t get(const std::string& path, int cacheMs, Reply done);
  uint32_t post(const std::string& url, const std::string& json, Reply done);  // a full URL, no token
  void cancel(uint32_t id);
  // The socket the replies come on (for poll()), and the replies that came: true when one was handed out.
  int fd() const { return sock_; }
  bool pump();

 private:
  void send(uint32_t type, const void* payload = nullptr, uint32_t len = 0);
  int sock_ = -1;
  CoreState* shm_ = nullptr;
  uint32_t nextId_ = 1;
  std::map<uint32_t, Reply> pending_;
  std::string in_;  // bytes read and not yet a whole reply
};

// A screen's requests: dropped (their replies ignored) when it closes.
class Requests {
 public:
  ~Requests() { cancelAll(); }
  void get(const std::string& path, int cacheMs, CoreHost::Reply done);
  void cancelAll();

 private:
  std::vector<uint32_t> ids_;
};
// the one CoreHost (the app's g_core)
CoreHost& coreHost();

// WifiManager.calculateSignalLevel(rssi, 4) and the words 1.0.x shows for it
int wifiLevel(int rssi);
const char* wifiSignal(int rssi);

}  // namespace rondo
