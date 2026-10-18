#include "wifi_web.h"
#include <math.h>
#include "config.h"
#include "web_assets.h"
#include "csv_logger.h"
#include <dirent.h>
#include <sys/stat.h>

#include "csv_logger.h"  // sdFs()

WifiWeb wifiWeb;

namespace {
constexpr uint32_t kSdLockWaitMs = 1000;
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
  if (!_prefs.begin("aero-wifi", true)) {
    Serial.println("[WiFi] Could not open preferences to load Wi-Fi credentials");
    return;
  }
  _ssid = _prefs.getString("ssid", "");
  _pass = _prefs.getString("pass", "");
  _prefs.end();
}

bool WifiWeb::saveCredentials(const String &ssid, const String &pass) {
  if (!_prefs.begin("aero-wifi", false)) {
    Serial.println("[WiFi] Could not open preferences to save Wi-Fi credentials");
    return false;
  }
  const size_t ssidWritten = _prefs.putString("ssid", ssid);
  const size_t passWritten = _prefs.putString("pass", pass);
  _prefs.end();
  if (ssidWritten == 0 || passWritten == 0) {
    Serial.println("[WiFi] Could not save Wi-Fi credentials");
    return false;
  }
  return true;
}

void WifiWeb::clearCredentials() {
  if (!_prefs.begin("aero-wifi", false)) {
    Serial.println("[WiFi] Could not open preferences to clear credentials");
    return;
  }
  const bool ssidRemoved = _prefs.remove("ssid");
  const bool passRemoved = _prefs.remove("pass");
  _prefs.end();
  if (!ssidRemoved && !passRemoved)
    Serial.println("[WiFi] No saved Wi-Fi credentials to clear");
  _ssid = "";
  _pass = "";
  Serial.println("[WiFi] credentials cleared");
}

// ============================================================================
// SmartConfig (ESP-Touch)
// ============================================================================
void WifiWeb::beginSmartConfig() {
  if (_smartConfigRunning) return;
  _smartConfigRetryAtMs = 0;
  Serial.println("[WiFi] starting SmartConfig (ESP-Touch).");
  Serial.println("[WiFi] Open the phone app (ESP Touch / Espressif) and push your SSID & password.");
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();
  _smartConfigRunning = true;
  _smartConfigStartMs = millis();
}

void WifiWeb::startSmartConfig() {
  _connected = false;
  _disconnectedSinceMs = millis();
  if (_mdnsStarted) {
    MDNS.end();
    _mdnsStarted = false;
  }
  WiFi.disconnect();
  _reconnectAttempts = 0;
  beginSmartConfig();
}

void WifiWeb::pollSmartConfig() {
  if (!_smartConfigRunning) return;

  // 超时保护：长时间无人配网则停止，避免射频被一直占用
  if (millis() - _smartConfigStartMs >= WIFI_SMART_CONFIG_TIMEOUT_MS) {
    Serial.println("[WiFi] SmartConfig timed out; stopping.");
    WiFi.stopSmartConfig();
    _smartConfigRunning = false;
    if (_ssid.isEmpty()) _smartConfigRetryAtMs = millis() + 5000;
    return;
  }

  if (WiFi.smartConfigDone()) {
    String ssid = WiFi.SSID();
    String pass = WiFi.psk();
    Serial.printf("[WiFi] SmartConfig received credentials for SSID=%s\n",
                  ssid.c_str());
    if (!saveCredentials(ssid, pass))
      Serial.println("[WiFi] Credentials work for this session but were not persisted");
    _ssid = ssid;
    _pass = pass;
    _smartConfigRunning = false;
    _reconnectAttempts = 0;
    _lastReconnectMs = millis();
    _disconnectedSinceMs = millis();
    _smartConfigRetryAtMs = 0;
    Serial.println("[WiFi] joining the configured network asynchronously");
  }
}

// ============================================================================
// STA 连接
// ============================================================================
void WifiWeb::connectStation() {
  if (_ssid.isEmpty()) return;
  Serial.printf("[WiFi] connecting to saved SSID=%s ...\n", _ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(_ssid.c_str(), _pass.c_str());
}

void WifiWeb::startMdns() {
  if (_mdnsStarted) return;
  if (MDNS.begin(WIFI_MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    _mdnsStarted = true;
    Serial.printf("[WiFi] mDNS ready: http://%s.local/\n", WIFI_MDNS_HOST);
  } else {
    Serial.println("[WiFi] mDNS start failed");
  }
}

// ============================================================================
// 生命周期
// ============================================================================
bool WifiWeb::begin(bool autoSmartConfig) {
  if (_started) return true;
  _started = true;
  _enabled = true;

  loadCredentials();
  if (_ssid.isEmpty()) {
    Serial.println("[WiFi] no saved credentials.");
    if (autoSmartConfig) {
      beginSmartConfig();
    } else {
      Serial.println("[WiFi] SmartConfig disabled; Wi-Fi idle.");
      return false;
    }
  } else {
    connectStation();
    _disconnectedSinceMs = millis();
  }

  registerRoutes();
  _server.begin();
  Serial.println("[WiFi] HTTP server started on port 80 (no login required)");
  return true;
}

bool WifiWeb::setEnabled(bool enabled) {
  if (enabled == _enabled) return true;
  if (enabled) {
    if (!_started) return begin(true);
    _enabled = true;
    _reconnectAttempts = 0;
    _lastReconnectMs = millis();
    _disconnectedSinceMs = millis();
    WiFi.mode(WIFI_STA);
    if (_ssid.isEmpty()) beginSmartConfig();
    else connectStation();
    return true;
  }

  if (_smartConfigRunning) WiFi.stopSmartConfig();
  _smartConfigRunning = false;
  _smartConfigRetryAtMs = 0;
  _connected = false;
  _enabled = false;
  if (_mdnsStarted) {
    MDNS.end();
    _mdnsStarted = false;
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[WiFi] disabled by user");
  return true;
}

void WifiWeb::tick() {
  if (!_started || !_enabled) return;

  pollSmartConfig();

  if (!_smartConfigRunning && _ssid.isEmpty() &&
      _smartConfigRetryAtMs != 0 &&
      (int32_t)(millis() - _smartConfigRetryAtMs) >= 0) {
    beginSmartConfig();
  }

  if (!_smartConfigRunning) {
    wl_status_t st = WiFi.status();
    bool nowConnected = (st == WL_CONNECTED);
    if (nowConnected && !_connected) {
      _connected = true;
      startMdns();
      Serial.printf("[WiFi] reconnected. IP=%s\n", WiFi.localIP().toString().c_str());
    } else if (!nowConnected && _connected) {
      _connected = false;
      _disconnectedSinceMs = millis();
      _reconnectAttempts = 0;
      if (_mdnsStarted) {
        MDNS.end();
        _mdnsStarted = false;
      }
      Serial.println("[WiFi] disconnected");
    }
    if (!nowConnected && !_ssid.isEmpty() &&
        millis() - _lastReconnectMs >= WIFI_RECONNECT_MS) {
      _lastReconnectMs = millis();
      if (_reconnectAttempts < UINT8_MAX) ++_reconnectAttempts;
      if (_reconnectAttempts >= 6 &&
          millis() - _disconnectedSinceMs >= 60000) {
        Serial.println("[WiFi] saved network unavailable; entering SmartConfig");
        startSmartConfig();
      } else {
        WiFi.disconnect();
        connectStation();
      }
    }
  }
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
    Serial.printf("[WS] client #%u connected from %s\n", client->id(),
                  client->remoteIP().toString().c_str());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    // 后续可解析前端上行控制（当前控制走 /api/control）
    (void)arg; (void)data; (void)len;
  }
}

void WifiWeb::broadcastTelemetry(uint32_t tMs, float powerW, float cadRpm,
                                 float vgMps, float vAirMps, bool windValid,
                                 float yawDeg, float postureDeg, float rho,
                                 int powerSrc, bool logging) {
  if (!_connected || _ws.count() == 0) return;

  const char *srcName = powerSrc == 3 ? "VIRTUAL"
                        : powerSrc == 2 ? "ANT"
                        : powerSrc == 1 ? "BLE" : "NONE";
  if (!isfinite(powerW)) powerW = 0.0f;
  if (!isfinite(cadRpm)) cadRpm = 0.0f;
  if (!isfinite(vgMps)) vgMps = 0.0f;
  if (!isfinite(vAirMps)) vAirMps = 0.0f;
  if (!isfinite(yawDeg)) yawDeg = 0.0f;
  if (!isfinite(postureDeg)) postureDeg = 0.0f;
  if (!isfinite(rho)) rho = 0.0f;
  char buf[320];
  snprintf(buf, sizeof(buf),
           "{\"t\":%lu,\"p\":%.1f,\"cad\":%.0f,\"vg\":%.3f,\"vair\":%.3f,"
           "\"windValid\":%s,\"yaw\":%.1f,\"posture\":%.1f,"
           "\"rho\":%.4f,\"src\":\"%s\","
           "\"log\":%s}",
           (unsigned long)tMs, powerW, cadRpm, vgMps, vAirMps,
           windValid ? "true" : "false", yawDeg,
           postureDeg, rho, srcName, logging ? "true" : "false");

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
    if (!lockSdCard(pdMS_TO_TICKS(kSdLockWaitMs))) {
      request->send(503, "text/plain", "SD card busy");
      return;
    }
    File file = sdFs().open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      unlockSdCard();
      request->send(404, "text/plain", "not found");
      return;
    }
    const size_t fileSize = file.size();
    file.close();
    unlockSdCard();

    AsyncWebServerResponse *response = request->beginResponse(
        "application/octet-stream", fileSize,
        [path](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
          if (!lockSdCard(pdMS_TO_TICKS(kSdLockWaitMs))) return 0;
          File chunk = sdFs().open(path, FILE_READ);
          if (!chunk || chunk.isDirectory() || !chunk.seek(index)) {
            if (chunk) chunk.close();
            unlockSdCard();
            return 0;
          }
          const size_t count = chunk.read(buffer, maxLen);
          chunk.close();
          unlockSdCard();
          return count;
        });
    if (!response) {
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
      if (source != "0" && source != "1" && source != "2" && source != "3") {
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
