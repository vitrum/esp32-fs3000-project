#pragma once


#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "saved_connections.h"

// ============================================================================
// Wi-Fi 网页实时数据服务
//
// 功能：
//   1) SmartConfig(ESP-Touch) 一键配网：手机 App 推送 SSID/密码，凭据持久化
//   2) 连上现场 Wi-Fi(STA) 后启动 Web 服务器 + mDNS：
//      - /            实时监控主页（HTML）
//      - /ws          WebSocket 实时数据推送
//      - /api/status  设备/网络状态
//      - /api/records SD 记录列表
//      - /api/download 下载记录（CSV/FIT）
//      - /api/control  开始/停止记录、切换功率源
//
// 设计原则：
//   - 异步非阻塞（AsyncWebServer），不阻塞主循环采集
//   - 可选启用；失败不影响屏幕/BLE/SD 主功能
//   - 与 plan/wifi-web.md、plan/time-source.md 对应
// ============================================================================

class WifiWeb {
public:
  // 启动 Wi-Fi 子系统。
  //   - Preferences 中已有凭据 → 直接 STA 连接
  //   - 否则若 autoSmartConfig=true → 进入 SmartConfig 等待手机配网
  bool begin(bool autoSmartConfig = true);
  bool setEnabled(bool enabled);

  // 主循环泵：SmartConfig 轮询、断线重连。非阻塞。
  void tick();

  // 1Hz 推送一帧实时数据到所有 WebSocket 客户端。
  void broadcastTelemetry(uint32_t tMs, float powerW, float cadRpm, float vgMps,
                          float vAirMps, bool windValid, float yawDeg,
                          float postureDeg, float rho, int powerSrc, bool logging,
                          float cda = -1, uint32_t utc = 0, bool armed = false,
                          bool saving = false, uint32_t durationMs = 0, uint32_t skipped = 0);

  void startSmartConfig();     // 主动触发配网
  void clearCredentials();     // 清除已存凭据

  bool connected() const { return _connected; }
  bool enabled() const { return _enabled; }
  bool provisioning() const { return _smartConfigRunning; }
  bool connecting() const { return _connecting; }
  IPAddress ip() const { return WiFi.localIP(); }
  String ssid() const { return _ssid; }
  uint32_t clientCount() const;
  uint32_t unixSec() const;

  // 主循环消费异步 HTTP 回调产生的请求。
  bool takeRecordingRequest(bool &start);
  bool takeSourceRequest(int &mode);

private:
  AsyncWebServer _server{80};
  AsyncWebSocket _ws{"/ws"};

  Preferences _prefs;
  bool _connected = false;
  bool _enabled = false;
  bool _smartConfigRunning = false;
  bool _started = false;
  bool _mdnsStarted = false;
  bool _wifiModeReady = false;
  bool _wifiBuffersConfigured = false;
  bool _timeSynchronized = false;
  bool _connecting = false;
  uint32_t _attemptStartedMs = 0;
  SavedConnections::Wifi _candidates[SavedConnections::kCapacity] = {};
  uint8_t _candidateCount = 0, _candidateIndex = 0;
  uint32_t _lastReconnectMs = 0;
  uint32_t _smartConfigStartMs = 0;
  uint32_t _smartConfigRetryAtMs = 0;
  uint32_t _startRetryNotBeforeMs = 0;
  uint32_t _disconnectedSinceMs = 0;
  uint8_t _reconnectAttempts = 0;

  String _ssid;
  String _pass;
  volatile int8_t _reqRecording = -1;
  volatile int  _reqSource = -1;
  portMUX_TYPE _requestMux = portMUX_INITIALIZER_UNLOCKED;

  bool isAllowedRecordName(String &name) const;
  void loadCredentials();
  bool saveCredentials(const String &ssid, const String &pass);
  void beginSmartConfig();
  void pollSmartConfig();
  bool connectStation();
  bool connectSavedStation();
  void startMdns();

  void registerRoutes();
  void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                 AwsEventType type, void *arg, uint8_t *data, size_t len);
  String currentStatusJson();
  bool recordsJson(String &json);
  String jsonEscape(const String &s);
};

// 全局单例（与项目其它模块风格一致）
extern WifiWeb wifiWeb;
