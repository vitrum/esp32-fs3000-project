#include "wifi_web.h"
#include "runtime_log.h"
#include <math.h>
#include "config.h"
#include "web_assets.h"
#include "csv_logger.h"
#include <dirent.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include "sd_download.h"
#include <new>
#include <esp_sntp.h>
#include <time.h>

WifiWeb wifiWeb;

namespace {
constexpr uint32_t kSdLockWaitMs = 1000;
constexpr uint32_t kWifiStartFailureCooldownMs = 30000;
}

bool WifiWeb::isAllowedRecordName(String &name) const {
  if (name.startsWith("/")) name.remove(0, 1);
  if (name.isEmpty() || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
      name.indexOf("..") >= 0)
    return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(name[i]);
    if (c < 0x20 || c == 0x7f || c == '"') return false;
  }
  return name.endsWith(".csv") || name.endsWith(".fit");
}

bool WifiWeb::takeRecordingRequest(bool &start) {
  portENTER_CRITICAL(&_requestMux);
  const int8_t request = _reqRecording;
  _reqRecording = -1;
  portEXIT_CRITICAL(&_requestMux);
  if (request < 0) return false;
  start = request == 1;
  return true;
}

bool WifiWeb::takeSourceRequest(int &mode) {
  portENTER_CRITICAL(&_requestMux);
  const int request = _reqSource;
  _reqSource = -1;
  portEXIT_CRITICAL(&_requestMux);
  if (request < 0) return false;
  mode = request;
  return true;
}

// ============================================================================
// 凭据持久化
// ============================================================================
void WifiWeb::loadCredentials() {
  SavedConnections saved;
  _candidateCount = _candidateIndex = 0;
  if (saved.load() && saved.wifiCount()) {
    _candidateCount = saved.wifiCount();
    for (uint8_t index = 0; index < _candidateCount; ++index) _candidates[index] = *saved.wifi(index);
    _ssid = _candidates[0].ssid;
    _pass = _candidates[0].password;
    return;
  }
  if (!_prefs.begin("aero-wifi", true)) {
    AppLog.println("[WiFi] Could not open preferences to load Wi-Fi credentials");
    return;
  }
  _ssid = _prefs.getString("ssid", "");
  _pass = _prefs.getString("pass", "");
  _prefs.end();
  if (!_ssid.isEmpty() && _ssid.length() <= 32 && _pass.length() <= 64) {
    snprintf(_candidates[0].ssid, sizeof(_candidates[0].ssid), "%s", _ssid.c_str());
    snprintf(_candidates[0].password, sizeof(_candidates[0].password), "%s", _pass.c_str());
    _candidateCount = 1;
  }
}

bool WifiWeb::saveCredentials(const String &ssid, const String &pass) {
  SavedConnections saved;
  if (!saved.load() || !saved.rememberWifi(ssid.c_str(), pass.c_str())) {
    AppLog.println("[WiFi] Could not persist the successful connection profile");
    return false;
  }
  if (!_prefs.begin("aero-wifi", false)) {
    AppLog.println("[WiFi] Could not open preferences to save Wi-Fi credentials");
    return false;
  }
  const size_t ssidWritten = _prefs.putString("ssid", ssid);
  const size_t passWritten = _prefs.putString("pass", pass);
  _prefs.end();
  if (ssidWritten == 0 || passWritten == 0) {
    AppLog.println("[WiFi] Could not save Wi-Fi credentials");
    return false;
  }
  return true;
}

void WifiWeb::clearCredentials() {
  SavedConnections saved;
  if (!saved.load() || !saved.clearWifi()) {
    AppLog.println("[WiFi] Could not clear saved network profiles");
    return;
  }
  if (!_prefs.begin("aero-wifi", false)) {
    AppLog.println("[WiFi] Could not open preferences to clear credentials");
    return;
  }
  const bool ssidRemoved = _prefs.remove("ssid");
  const bool passRemoved = _prefs.remove("pass");
  _prefs.end();
  if (!ssidRemoved && !passRemoved)
    AppLog.println("[WiFi] No saved Wi-Fi credentials to clear");
  _ssid = "";
  _pass = "";
  _candidateCount = _candidateIndex = 0;
  AppLog.println("[WiFi] credentials cleared");
}

// ============================================================================
// SmartConfig (ESP-Touch)
// ============================================================================
void WifiWeb::beginSmartConfig() {
  if (_smartConfigRunning) return;
  _smartConfigRetryAtMs = 0;
  AppLog.println("[WiFi] starting SmartConfig (ESP-Touch).");
  AppLog.println("[WiFi] Open the phone app (ESP Touch / Espressif) and push your SSID & password.");
  if (!_wifiModeReady) {
    if (!_wifiBuffersConfigured) {
      WiFi.useStaticBuffers(true);
      _wifiBuffersConfigured = true;
    }
    if (!WiFi.mode(WIFI_STA)) {
      AppLog.printf("[WiFi] STA initialization failed; internal heap free=%u largest=%u\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      _smartConfigRetryAtMs = millis() + 60000;
      return;
    }
    _wifiModeReady = true;
  }
  WiFi.beginSmartConfig();
  _smartConfigRunning = true;
  _smartConfigStartMs = millis();
}

void WifiWeb::startSmartConfig() {
  _startRetryNotBeforeMs = 0;
  if (!_started && !begin(true)) return;
  _enabled = true;
  _connected = false;
  _connecting = false;
  _candidateCount = _candidateIndex = 0;
  _disconnectedSinceMs = millis();
  if (_mdnsStarted) {
    MDNS.end();
    _mdnsStarted = false;
  }
  if (_wifiModeReady) WiFi.disconnect();
  _reconnectAttempts = 0;
  beginSmartConfig();
  _enabled = _smartConfigRunning;
}

void WifiWeb::pollSmartConfig() {
  if (!_smartConfigRunning) return;

  // 超时保护：长时间无人配网则停止，避免射频被一直占用
  if (millis() - _smartConfigStartMs >= WIFI_SMART_CONFIG_TIMEOUT_MS) {
    AppLog.println("[WiFi] SmartConfig timed out; stopping.");
    WiFi.stopSmartConfig();
    _smartConfigRunning = false;
    setEnabled(false);
    return;
  }

  if (WiFi.smartConfigDone()) {
    String ssid = WiFi.SSID();
    String pass = WiFi.psk();
    AppLog.printf("[WiFi] SmartConfig received credentials for SSID=%s\n",
                  ssid.c_str());
    _ssid = ssid;
    _pass = pass;
    _smartConfigRunning = false;
    WiFi.stopSmartConfig();
    _connecting = true;
    _attemptStartedMs = millis();
    _reconnectAttempts = 0;
    _lastReconnectMs = millis();
    _disconnectedSinceMs = millis();
    _smartConfigRetryAtMs = 0;
    AppLog.println("[WiFi] joining the configured network asynchronously");
  }
}

// ============================================================================
// STA 连接
// ============================================================================
bool WifiWeb::connectStation() {
  if (_ssid.isEmpty()) return false;
  if (!_wifiModeReady) {
    if (!_wifiBuffersConfigured) {
      WiFi.useStaticBuffers(true);
      _wifiBuffersConfigured = true;
    }
    if (!WiFi.mode(WIFI_STA)) {
      const uint32_t freeHeap =
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const uint32_t largestBlock =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      AppLog.printf("[WiFi] STA initialization failed; internal heap free=%lu largest=%lu\n",
                    (unsigned long)freeHeap, (unsigned long)largestBlock);
      _lastReconnectMs = millis();
      return false;
    }
    _wifiModeReady = true;
    WiFi.setAutoReconnect(false);
  }
  AppLog.printf("[WiFi] connecting to saved SSID=%s ...\n", _ssid.c_str());
  WiFi.disconnect();
  const wl_status_t status = WiFi.begin(_ssid.c_str(), _pass.c_str());
  if (status == WL_CONNECT_FAILED) {
    AppLog.println("[WiFi] station start failed; waiting before retry");
    _lastReconnectMs = millis();
    return false;
  }
  _connecting = true;
  _attemptStartedMs = millis();
  return true;
}

void WifiWeb::startMdns() {
  if (_mdnsStarted) return;
  if (MDNS.begin(WIFI_MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    _mdnsStarted = true;
    AppLog.printf("[WiFi] mDNS ready: http://%s.local/\n", WIFI_MDNS_HOST);
  } else {
    AppLog.println("[WiFi] mDNS start failed");
  }
}

bool WifiWeb::connectSavedStation() {
  for (;;) {
    if (connectStation()) return true;
    if (_candidateIndex + 1 >= _candidateCount) return false;
    ++_candidateIndex;
    _ssid = _candidates[_candidateIndex].ssid;
    _pass = _candidates[_candidateIndex].password;
  }
}

// ============================================================================
// 生命周期
// ============================================================================
bool WifiWeb::begin(bool autoSmartConfig) {
  if (_started) return true;
  if (_startRetryNotBeforeMs != 0 &&
      (int32_t)(millis() - _startRetryNotBeforeMs) < 0)
    return true;

  loadCredentials();
  bool radioStarted = false;
  if (_ssid.isEmpty()) {
    AppLog.println("[WiFi] no saved credentials.");
    if (autoSmartConfig) {
      beginSmartConfig();
      radioStarted = _smartConfigRunning;
    } else {
      AppLog.println("[WiFi] SmartConfig disabled; Wi-Fi idle.");
      return false;
    }
  } else {
    radioStarted = connectSavedStation();
    _disconnectedSinceMs = millis();
  }
  if (!radioStarted) {
    if (_wifiModeReady) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      _wifiModeReady = false;
    }
    _enabled = false;
    _startRetryNotBeforeMs = millis() + kWifiStartFailureCooldownMs;
    AppLog.println("[WiFi] radio start failed; server not started");
    return false;
  }

  const uint32_t largestBlock =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (largestBlock < 20 * 1024) {
    AppLog.printf("[WiFi] not enough contiguous internal heap for AsyncTCP: largest=%lu\n",
                  (unsigned long)largestBlock);
    if (_smartConfigRunning) WiFi.stopSmartConfig();
    _smartConfigRunning = false;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _wifiModeReady = false;
    _enabled = false;
    _startRetryNotBeforeMs = millis() + kWifiStartFailureCooldownMs;
    return false;
  }

  registerRoutes();
  _server.begin();
  _enabled = true;
  _started = true;
  AppLog.println("[WiFi] HTTP server started on port 80 (no login required)");
  return true;
}

bool WifiWeb::setEnabled(bool enabled) {
  if (enabled == _enabled) return true;
  if (enabled) {
    _startRetryNotBeforeMs = 0;
    if (!_started) return begin(true);
    loadCredentials();
    _reconnectAttempts = 0;
    _lastReconnectMs = millis();
    _disconnectedSinceMs = millis();
    bool radioStarted = false;
    if (_ssid.isEmpty()) {
      beginSmartConfig();
      radioStarted = _smartConfigRunning;
    } else {
      radioStarted = connectSavedStation();
    }
    if (!radioStarted && _wifiModeReady) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      _wifiModeReady = false;
    }
    _enabled = radioStarted;
    if (!radioStarted)
      _startRetryNotBeforeMs = millis() + kWifiStartFailureCooldownMs;
    return radioStarted;
  }

  if (_smartConfigRunning) WiFi.stopSmartConfig();
  _smartConfigRunning = false;
  _smartConfigRetryAtMs = 0;
  _startRetryNotBeforeMs = 0;
  _connected = false;
  _connecting = false;
  _enabled = false;
  if (_mdnsStarted) {
    MDNS.end();
    _mdnsStarted = false;
  }
  if (_wifiModeReady) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  _wifiModeReady = false;
  AppLog.println("[WiFi] disabled by user");
  return true;
}

void WifiWeb::tick() {
  if (!_started || !_enabled) return;

  pollSmartConfig();

  if (!_smartConfigRunning) {
    wl_status_t st = WiFi.status();
    bool nowConnected = (st == WL_CONNECTED);
    if (nowConnected && !_connected) {
      _connected = true;
      _connecting = false;
      if (!saveCredentials(_ssid, _pass))
        AppLog.println("[WiFi] Connected, but profile was not saved");
      configTime(0, 0, "ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org");
      AppLog.println("[TIME] network connected; requesting NTP UTC asynchronously");
      startMdns();
      AppLog.printf("[WiFi] reconnected. IP=%s\n", WiFi.localIP().toString().c_str());
    } else if (!nowConnected && _connected) {
      _connected = false;
      _disconnectedSinceMs = millis();
      _reconnectAttempts = 0;
      if (_mdnsStarted) {
        MDNS.end();
        _mdnsStarted = false;
      }
      AppLog.println("[WiFi] disconnected");
      setEnabled(false);
      AppLog.println("[WiFi] waiting for manual reconnection");
    }
    if (!nowConnected && _connecting && millis() - _attemptStartedMs >= 15000) {
      _connecting = false;
      if (_candidateIndex + 1 < _candidateCount) {
        ++_candidateIndex;
        _ssid = _candidates[_candidateIndex].ssid;
        _pass = _candidates[_candidateIndex].password;
        if (!connectSavedStation()) setEnabled(false);
      } else {
        setEnabled(false);
        AppLog.println("[WiFi] saved network list exhausted; waiting for manual setup");
      }
    }
  }
  if (_connected && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
      !_timeSynchronized) {
    const time_t utc = time(nullptr);
    if (utc >= 1577836800 && (uint64_t)utc <= 4102444799ULL) {
      _timeSynchronized = true;
      AppLog.printf("[TIME] NTP synchronized: UTC=%lu\n", (unsigned long)utc);
    }
  }
}

uint32_t WifiWeb::unixSec() const {
  if (!_timeSynchronized) return 0;
  const time_t utc = time(nullptr);
  return utc >= 1577836800 && (uint64_t)utc <= 4102444799ULL ? (uint32_t)utc : 0;
}

// ============================================================================
// WebSocket
// ============================================================================
uint32_t WifiWeb::clientCount() const {
  return _ws.count();
}

void WifiWeb::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    AppLog.printf("[WS] client #%u connected from %s\n", client->id(),
                  client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    AppLog.printf("[WS] client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    // 后续可解析前端上行控制（当前控制走 /api/control）
    (void)arg; (void)data; (void)len;
  }
}

void WifiWeb::broadcastTelemetry(uint32_t tMs, float powerW, float cadRpm,
                                 float vgMps, float vAirMps, bool windValid,
                                 float yawDeg, float postureDeg, float rho,
                                 int powerSrc, bool logging, float cda, uint32_t utc,
                                 bool armed, bool saving, uint32_t durationMs, uint32_t skipped,
                                 uint16_t heartRate, bool heartValid) {
  if (!_connected || _ws.count() == 0) return;

  const char *srcName = powerSrc == 3 ? "VIRTUAL"
                        : powerSrc == 1 ? "BLE" : "NONE";
  if (!isfinite(powerW)) powerW = 0.0f;
  if (!isfinite(cadRpm)) cadRpm = 0.0f;
  if (!isfinite(vgMps)) vgMps = 0.0f;
  if (!isfinite(vAirMps)) vAirMps = 0.0f;
  if (!isfinite(yawDeg)) yawDeg = 0.0f;
  if (!isfinite(postureDeg)) postureDeg = 0.0f;
  if (!isfinite(rho)) rho = 0.0f;
  if (!isfinite(cda) || !windValid || powerSrc == 0) cda = -1;
  if (utc == 0) utc = unixSec();
  char heart[8] = "null";
  if (heartValid && heartRate) snprintf(heart, sizeof(heart), "%u", heartRate);
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"t\":%lu,\"p\":%.1f,\"cad\":%.0f,\"vg\":%.3f,\"vair\":%.3f,"
           "\"windValid\":%s,\"yaw\":%.1f,\"posture\":%.1f,"
           "\"rho\":%.4f,\"src\":\"%s\","
           "\"log\":%s,\"cda\":%.4f,\"utc\":%lu,\"armed\":%s,\"saving\":%s,\"duration\":%lu,\"skipped\":%lu,\"hr\":%s}",
           (unsigned long)tMs, powerW, cadRpm, vgMps, vAirMps,
           windValid ? "true" : "false", yawDeg,
           postureDeg, rho, srcName, logging ? "true" : "false", cda,
           (unsigned long)utc, armed ? "true" : "false", saving ? "true" : "false",
           (unsigned long)durationMs, (unsigned long)skipped, heart);

  _ws.textAll(buf);
}

// ============================================================================
// HTTP 路由
// ============================================================================
void WifiWeb::registerRoutes() {
  _ws.onEvent([this](AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t,
                     void *a, uint8_t *d, size_t l) {
    onWsEvent(s, c, t, a, d, l);
  });
  _server.addHandler(&_ws);

  _server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", WIFI_WEB_INDEX_HTML);
  });

  _server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
    request->send(200, "application/json", currentStatusJson());
  });

  _server.on("/api/records", HTTP_GET, [this](AsyncWebServerRequest *request) {
    String json;
    if (!recordsJson(json)) {
      request->send(503, "application/json", "{\"ok\":false,\"error\":\"sd_busy\"}");
      return;
    }
    request->send(200, "application/json", json);
  });

  _server.on("/api/download", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!request->hasArg("name")) {
      request->send(400, "text/plain", "missing name");
      return;
    }
    String name = request->arg("name");
    if (!isAllowedRecordName(name)) {
      request->send(400, "text/plain", "invalid record name");
      return;
    }
    const String path = "/" + name;
    if (!SdDownloadResponse::tryAcquire()) {
      request->send(503, "text/plain", "Another download is active");
      return;
    }
    if (!lockSdCard(0)) {
      SdDownloadResponse::releaseSlot();
      request->send(503, "text/plain", "SD card busy");
      return;
    }
    File file = sdFs().open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      unlockSdCard();
      SdDownloadResponse::releaseSlot();
      request->send(404, "text/plain", "not found");
      return;
    }
    const size_t fileSize = file.size();
    file.close();
    unlockSdCard();

    AsyncWebServerResponse *response =
        new (std::nothrow) SdDownloadResponse(path, fileSize);
    if (!response) {
      SdDownloadResponse::releaseSlot();
      request->send(500, "text/plain", "could not create download response");
      return;
    }
    response->addHeader("Content-Disposition",
                        "attachment; filename=\"" + name + "\"");
    request->send(response);
  });

  _server.on("/api/control", HTTP_POST, [this](AsyncWebServerRequest *request) {
    if (!request->hasArg("cmd")) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"missing_cmd\"}");
      return;
    }
    const String cmd = request->arg("cmd");
    int sourceMode = -1;
    if (cmd == "source") {
      if (!request->hasArg("src")) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"missing_source\"}");
        return;
      }
      const String source = request->arg("src");
      if (source != "0" && source != "1" && source != "3") {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_source\"}");
        return;
      }
      sourceMode = source.toInt();
    } else if (cmd != "start" && cmd != "stop") {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_cmd\"}");
      return;
    }
    portENTER_CRITICAL(&_requestMux);
    if (cmd == "start" || cmd == "stop") {
      _reqRecording = cmd == "start" ? 1 : 0;
    } else {
      _reqSource = sourceMode;
    }
    portEXIT_CRITICAL(&_requestMux);
    request->send(202, "application/json", "{\"ok\":true,\"queued\":true}");
  });

  _server.on("/api/wifi/provision", HTTP_POST,
             [this](AsyncWebServerRequest *request) {
    startSmartConfig();
    request->send(202, "application/json", "{\"ok\":true,\"provisioning\":true}");
  });

  _server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "not found");
  });
}

String WifiWeb::currentStatusJson() {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"provisioning\":%s,"
           "\"clients\":%u,\"host\":\"%s.local\"}",
           _connected ? "true" : "false",
           jsonEscape(_connected ? WiFi.SSID() : _ssid).c_str(),
           WiFi.localIP().toString().c_str(),
           _smartConfigRunning ? "true" : "false",
           (unsigned)_ws.count(), WIFI_MDNS_HOST);
  return String(buf);
}

bool WifiWeb::recordsJson(String &out) {
  if (!lockSdCard(pdMS_TO_TICKS(kSdLockWaitMs))) return false;
  out = "[";
  out.reserve(512);
  DIR *dir = opendir(sdMountPoint());
  if (!dir) {
    unlockSdCard();
    return false;
  }

  bool first = true;
  size_t guard = 0;
  struct dirent *entry;
  while (guard++ < 400 && (entry = readdir(dir)) != nullptr) {
    if (!entry->d_name || entry->d_name[0] == '.') continue;
    char fullPath[300];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", sdMountPoint(), entry->d_name);
    struct stat st;
    if (stat(fullPath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

    String name("/");
    name += entry->d_name;
    if (!name.endsWith(".csv") && !name.endsWith(".fit")) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"name\":\"";
    out += jsonEscape(name);
    out += "\",\"size\":";
    out += String((unsigned long)st.st_size);
    out += "}";
  }
  closedir(dir);
  out += "]";
  unlockSdCard();
  return true;
}

// ============================================================================
// 工具
// ============================================================================
String WifiWeb::jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if ((uint8_t)c < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char)c);
      o += escaped;
    }
    else o += c;
  }
  return o;
}
