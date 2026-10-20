#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 3
namespace NIMBLE_PROPERTY {
constexpr uint32_t READ = 1, WRITE = 2, NOTIFY = 4;
}
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
  NimBLEDevice::advertising.active = false;
  return true;
}
inline void NimBLEServer::connect(uint16_t handle) {
  NimBLEDevice::advertising.active = false;
  NimBLEConnInfo info(handle);
  callbacks->onConnect(this, info);
}
