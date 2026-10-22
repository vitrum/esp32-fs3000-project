#pragma once
#include "Arduino.h"

enum wl_status_t { WL_IDLE_STATUS, WL_CONNECTED, WL_CONNECT_FAILED, WL_DISCONNECTED };
constexpr int WIFI_OFF = 0, WIFI_STA = 1;
class IPAddress {
public:
  String toString() const { return "192.0.2.10"; }
};
class HostWiFi {
public:
  void useStaticBuffers(bool) {}
  bool mode(int value) { ++modeCalls; if (failMode) return false; modeValue = value; return true; }
  void setAutoReconnect(bool) {}
  wl_status_t begin(const char *ssid, const char *pass) {
    ++beginCalls; network = ssid; password = pass; return beginResult;
  }
  bool beginSmartConfig() { ++provisionCalls; return true; }
  void stopSmartConfig() { ++stopProvisionCalls; }
  bool smartConfigDone() const { return provisionDone; }
  void disconnect(bool = false) { state = WL_DISCONNECTED; }
  void reconnect() { ++reconnectCalls; }
  wl_status_t status() const { return state; }
  String SSID() const { return network; }
  String psk() const { return password; }
  IPAddress localIP() const { return {}; }
  bool failMode = false, provisionDone = false;
  wl_status_t state = WL_DISCONNECTED, beginResult = WL_IDLE_STATUS;
  int modeValue = WIFI_OFF;
  unsigned modeCalls = 0, beginCalls = 0, reconnectCalls = 0;
  unsigned provisionCalls = 0, stopProvisionCalls = 0;
  String network, password;
};
inline HostWiFi WiFi;