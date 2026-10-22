#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 3
namespace NIMBLE_PROPERTY {
constexpr uint32_t READ = 1, WRITE = 2, NOTIFY = 4;
}
constexpr uint8_t BLE_ADDR_PUBLIC = 0;
constexpr int BLE_HS_EDONE = 14;
class NimBLEUUID {
public:
  NimBLEUUID(const char *value) : value_(value) {}
  std::string toString() const { return value_; }
  bool operator==(const NimBLEUUID &other) const { return value_ == other.value_; }
private:
  std::string value_;
};
class NimBLEAddress {
public:
  NimBLEAddress(const std::string &value = "11:22:33:44:55:66", uint8_t type = 0)
      : value_(value), type_(type) {}
  std::string toString() const { return value_; }
  uint8_t getType() const { return type_; }
private:
  std::string value_;
  uint8_t type_;
};
class NimBLEAdvertisedDevice {
public:
  bool isConnectable() const { return connectable; }
  bool isAdvertisingService(const NimBLEUUID &uuid) const {
    return std::find(services.begin(), services.end(), uuid.toString()) != services.end();
  }
  std::string getName() const { return name; }
  NimBLEAddress getAddress() const { return address; }
  int8_t getRSSI() const { return -45; }
  bool connectable = true;
  std::string name = "Trainer";
  NimBLEAddress address;
  std::vector<std::string> services{"0x1818"};
};
class NimBLEScanResults {
public:
  int getCount() const { return devices.size(); }
  const NimBLEAdvertisedDevice *getDevice(int index) const { return &devices.at(index); }
  std::vector<NimBLEAdvertisedDevice> devices;
};
class NimBLEScanCallbacks {
public:
  virtual ~NimBLEScanCallbacks() = default;
  virtual void onScanEnd(const NimBLEScanResults &, int) {}
};
class NimBLEScan {
public:
  void setMaxResults(uint8_t) {}
  void clearResults() { if (!active) results.devices.clear(); }
  void setScanCallbacks(NimBLEScanCallbacks *value, bool) { callbacks = value; }
  void setActiveScan(bool value) { activeScan = value; }
  void setInterval(uint16_t value) { interval = value; }
  void setWindow(uint16_t value) { window = value; }
  bool isScanning() const { return active; }
  bool start(uint32_t, bool) {
    ++starts;
    if (failStart) return false;
    active = true;
    if (startHook) startHook();
    if (!manualCompletion) complete();
    return true;
  }
  bool stop() {
    ++stops;
    if (failStop) return false;
    active = false;
    return true;
  }
  void complete(int reason = 0) {
    active = false;
    if (callbacks) callbacks->onScanEnd(results, reason);
  }
  NimBLEScanResults getResults() const { return results; }
  NimBLEScanCallbacks *callbacks = nullptr;
  NimBLEScanResults results;
  std::function<void()> startHook;
  std::atomic<bool> active{false};
  std::atomic<unsigned> starts{0};
  unsigned stops = 0;
  bool failStart = false, failStop = false, manualCompletion = false, activeScan = false;
  uint16_t interval = 0, window = 0;
};
class NimBLEClient;
class NimBLEClientCallbacks {
public:
  virtual ~NimBLEClientCallbacks() = default;
  virtual void onDisconnect(NimBLEClient *, int) {}
};
class NimBLERemoteCharacteristic {
public:
  using Notify = void (*)(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool);
  explicit NimBLERemoteCharacteristic(const char *uuid = "0x2a63") : uuid(uuid) {}
  NimBLEUUID getUUID() const { return uuid; }
  bool canNotify() const { return true; }
  bool canIndicate() const { return false; }
  bool subscribe(bool, Notify value, bool) { notify = value; return true; }
  void emit(std::vector<uint8_t> data) { assert(notify); notify(this, data.data(), data.size(), true); }
  NimBLEUUID uuid;
  Notify notify = nullptr;
};
class NimBLERemoteService {
public:
  NimBLEUUID getUUID() const { return uuid; }
  const std::vector<NimBLERemoteCharacteristic *> &getCharacteristics(bool) { return characteristics; }
  NimBLERemoteCharacteristic *getCharacteristic(const NimBLEUUID &target) {
    for (auto *characteristic : characteristics) if (characteristic->getUUID() == target) return characteristic;
    return nullptr;
  }
  NimBLEUUID uuid{"0x1818"};
  NimBLERemoteCharacteristic measurement;
  std::vector<NimBLERemoteCharacteristic *> characteristics{&measurement};
};
class NimBLEClient {
public:
  void setClientCallbacks(NimBLEClientCallbacks *value, bool) { callbacks = value; }
  void setConnectTimeout(uint32_t) {}
  bool connect(const NimBLEAddress &address) { peer = address; connected = connectResult; return connected; }
  bool isConnected() const { return connected; }
  void disconnect() { connected = false; if (callbacks) callbacks->onDisconnect(this, 19); }
  bool cancelConnect() { return true; }
  int getLastError() const { return connected ? 0 : 13; }
  const std::vector<NimBLERemoteService *> &getServices(bool) { return services; }
  NimBLERemoteService *getService(const NimBLEUUID &uuid) {
    for (auto *entry : services) if (entry->getUUID() == uuid) return entry;
    return nullptr;
  }
  NimBLEAddress peer;
  bool connectResult = true, connected = false;
  NimBLEClientCallbacks *callbacks = nullptr;
  NimBLERemoteService service;
  std::vector<NimBLERemoteService *> services{&service};
};
class NimBLEUtils {
public:
  static const char *returnCodeToString(int) { return "mock error"; }
};
class NimBLEConnInfo {
public:
  explicit NimBLEConnInfo(uint16_t handle = 7) : handle_(handle) {}
  uint16_t getConnHandle() const { return handle_; }
private:
  uint16_t handle_;
};
class NimBLECharacteristic;
class NimBLEServer;
class NimBLEServerCallbacks {
public:
  virtual ~NimBLEServerCallbacks() = default;
  virtual void onConnect(NimBLEServer *, NimBLEConnInfo &) {}
  virtual void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) {}
};
class NimBLECharacteristicCallbacks {
public:
  virtual ~NimBLECharacteristicCallbacks() = default;
  virtual void onRead(NimBLECharacteristic *, NimBLEConnInfo &) {}
  virtual void onWrite(NimBLECharacteristic *, NimBLEConnInfo &) {}
  virtual void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &, uint16_t) {}
};
class NimBLECharacteristic {
public:
  NimBLECharacteristic(uint32_t properties, uint16_t maxLength)
      : properties(properties), maxLength(maxLength) {}
  void setCallbacks(NimBLECharacteristicCallbacks *value) { callbacks = value; }
  void setValue(const char *text) { value = text; }
  std::string getValue() const { return value; }
  bool notify(const uint8_t *data, size_t length, uint16_t) {
    ++attempts;
    if (failNotify) return false;
    notifications.emplace_back(reinterpret_cast<const char *>(data), length);
    return true;
  }
  void subscribe(uint16_t setting = 1) {
    NimBLEConnInfo info;
    callbacks->onSubscribe(this, info, setting);
  }
  void command(const std::string &text) {
    value = text;
    NimBLEConnInfo info;
    callbacks->onWrite(this, info);
  }
  std::string read() {
    NimBLEConnInfo info;
    callbacks->onRead(this, info);
    return value;
  }
  uint32_t properties;
  uint16_t maxLength;
  bool failNotify = false;
  unsigned attempts = 0;
  NimBLECharacteristicCallbacks *callbacks = nullptr;
  std::string value;
  std::vector<std::string> notifications;
};
class NimBLEService {
public:
  NimBLECharacteristic *createCharacteristic(const char *uuid, uint32_t properties, uint16_t length) {
    chars[uuid].reset(new NimBLECharacteristic(properties, length));
    return chars[uuid].get();
  }
  NimBLECharacteristic *getCharacteristic(const char *uuid) const {
    auto entry = chars.find(uuid);
    return entry == chars.end() ? nullptr : entry->second.get();
  }
  std::map<std::string, std::unique_ptr<NimBLECharacteristic>> chars;
};
class NimBLEAdvertising {
public:
  void enableScanResponse(bool) {}
  void setMinInterval(uint16_t) {}
  void setMaxInterval(uint16_t) {}
  bool setName(const std::string &) { return true; }
  bool addServiceUUID(const char *) { return true; }
  bool isAdvertising() const { return active; }
  bool start() { ++attempts; active = !fail; return active; }
  bool active = false, fail = false;
  bool failStop = false;
  unsigned attempts = 0;
};
class NimBLEServer {
public:
  void setCallbacks(NimBLEServerCallbacks *value, bool) { callbacks = value; }
  void advertiseOnDisconnect(bool) {}
  NimBLEService *createService(const char *uuid) {
    services[uuid].reset(new NimBLEService);
    return services[uuid].get();
  }
  NimBLEService *getServiceByUUID(const char *uuid) const {
    auto entry = services.find(uuid);
    return entry == services.end() ? nullptr : entry->second.get();
  }
  bool start() { return true; }
  bool stopAdvertising();
  bool disconnect(uint16_t handle) {
    disconnected.push_back(handle);
    NimBLEConnInfo info(handle);
    callbacks->onDisconnect(this, info, 19);
    return true;
  }
  uint16_t getPeerMTU(uint16_t) const { return mtu; }
  void connect(uint16_t handle = 7);
  NimBLEServerCallbacks *callbacks = nullptr;
  uint16_t mtu = 23;
  std::vector<uint16_t> disconnected;
  std::map<std::string, std::unique_ptr<NimBLEService>> services;
};
class NimBLEDevice {
public:
  static void init(const char *) { initialized = true; }
  static bool deinit(bool) { destroy(); return true; }
  static NimBLEScan *getScan() { return &scan; }
  static NimBLEClient *createClient() {
    auto *client = new NimBLEClient;
    client->service.uuid = NimBLEUUID(nextService.c_str());
    client->service.measurement.uuid = NimBLEUUID(nextCharacteristic.c_str());
    clients.push_back(client);
    return client;
  }
  static void deleteClient(NimBLEClient *client) {
    clients.erase(std::remove(clients.begin(), clients.end(), client), clients.end());
    delete client;
  }
  inline static NimBLEScan scan;
  inline static std::string nextService = "0x1818";
  inline static std::string nextCharacteristic = "0x2a63";
  inline static std::vector<NimBLEClient *> clients;
  static bool isInitialized() { return initialized; }
  static NimBLEServer *createServer() {
    if (!server) server.reset(new NimBLEServer);
    return server.get();
  }
  static NimBLEAdvertising *getAdvertising() { return &advertising; }
  static void destroy() { server.reset(); advertising = {}; initialized = false; }
  static bool initialized;
  static std::unique_ptr<NimBLEServer> server;
  static NimBLEAdvertising advertising;
};
inline bool NimBLEServer::stopAdvertising() {
  if (NimBLEDevice::advertising.failStop) return false;
  NimBLEDevice::advertising.active = false;
  return true;
}
inline void NimBLEServer::connect(uint16_t handle) {
  NimBLEDevice::advertising.active = false;
  NimBLEConnInfo info(handle);
  callbacks->onConnect(this, info);
}
