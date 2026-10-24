#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>

#include "lv_conf.h"
#include <lvgl.h>

#include "ble_power.h"
#include "wind_probe.h"
#include "csv_logger.h"   // SdFileEntry
#include "ui_navigation_guard.h"
#include "ui_touch_buffer.h"

// ST7789 240x320 display for wind, CdA, live power, cadence, and status.
class DisplayLcd {
public:
  void begin();
  void tick();
  void update(const WindVector &w, bool windValid, float powerW, float cadRpm,
              float vgMps, float postureDeg, float rho, int pmSrc, bool logOk);
  bool takePowerSourceRequest(int &mode);
  float virtualPowerW() const { return virtualPowerW_; }
  float virtualCadenceRpm() const { return virtualCadenceRpm_; }
  bool takeBleScanRequest();
  // 取出用户确认连接的地址，并带回该设备在扫描时记录的真实地址类型
  // （addressType 可为 nullptr）。
  bool takeBleConnectRequest(char *address, size_t capacity,
                             uint8_t *addressType = nullptr);
  bool takeBleCancelRequest();
  // 主循环处理完一次 BLE 操作后调用，允许 UI 再次接受 BLE 请求。
  void clearBleRequestInFlight() { bleRequestInFlight_ = false; }
  void showBleLoading(const char *status);
  void finishBleCancelled();
  void showBleConnectFailure(const char *error);
  void setBleDevices(const BlePowerDevice *devices, size_t count, const char *status);
  void closePowerMeterPanels();
  bool takeWifiEnableRequest(bool &enabled);
  void updateWifiStatus(bool enabled, bool connected, bool provisioning,
                        const char *ssid, const char *ip);

  // ---- SD 卡文件浏览 ----
  // 用户点击设置里的 "SD CARD" 时置位，主循环据此触发文件列表加载。
  bool takeSdListRequest(char *directory, size_t capacity);
  // 主循环加载完文件后回填列表（音频文件带 ▶ 标记）。
  void setSdFiles(const SdFileEntry *files, size_t count, const char *status);
  // 用户点击某文件请求播放：输出完整路径（"/name"）到 path。
  bool takeSdPlayRequest(char *path, size_t capacity);
  // 用户点击 STOP：停止播放。
  bool takeSdStopRequest();
  // 更新当前播放状态文字（由主循环轮询 audio 模块）。
  void updateAudioStatus(bool playing, const char *title);

  // ---- 声音设置 ----
  // 用户在声音页选择了新音量（0~100）：主循环据此设置音量并试听。
  bool takeVolumeRequest(uint8_t &vol);
  // 主循环回填当前音量，用于高亮所选档位。
  void setVolume(uint8_t vol);
  bool takeFitSaveRequest();
  // 用户在日志页点击"开始/停止记录"：主循环据此调用 logger.start()/stop()。
  // start=true 表示请求开始记录，false 表示请求停止。
  bool takeRecordingRequest(bool &start);
  // 主循环回填当前记录状态，用于切换按钮文字（RECORD / STOP）与颜色。
  void setRecording(bool recording);
  void updateRideLog(const char *path, bool recording, uint32_t samples,
                     uint32_t dropped, float power, float cadence, float speed);
  void setFitSaving(bool saving);
  void showFitResult(const char *message);

  // ---- GPS 页面 ----
  // 用户在 GPS 页点击"重启 GPS"：主循环据此调用 gps.restart()。
  bool takeGpsRestartRequest();
  // 主循环轮询 GPS 状态并回填到页面（仅在 GPS 页可见时刷新）。
  // fix：是否已定位；sat：卫星数；hdop：HDOP 值；
  // lat/lon：经纬度；altM：海拔(m)；speedMps：地速(m/s)；
  // utc：UTC 时间字符串；local：本地时间字符串；ageMs：定位年龄(ms)。
  void updateGpsStatus(bool enabled, bool fix, uint8_t sat, uint32_t hdop,
                       double lat, double lon, float altM, float speedMps,
                       const char *utc, const char *local, uint32_t ageMs);
  // 直接打开 GPS 页面（供 BOOT 键快捷进入）。
  void showGpsPanel();
  // 直接打开设置页面（供 BOOT 键使用）。
  void showSettings();
  // 更新主界面顶部的 GPS 状态图标（非阻塞）：enabled=GPS 是否启用；
  // fix=是否已定位；sat=卫星数。搜星中显示旋转动画，定位后显示绿色图标+卫星数。
  void updateGpsIndicator(bool enabled, bool fix, uint8_t sat);
  void updateClock(uint32_t utc);
  float estimatedCda() const { return estimatedCda_; }
  // 主循环回填 GPS 模块检测结果。present=是否找到；done=是否已完成一次判定。
  // 用于"点击 GPS 按钮时提示找不到模块"。
  void setGpsModulePresent(bool present, bool done) {
    gpsModulePresent_ = present;
    gpsDetectDone_ = done;
  }
private:
  Arduino_DataBus *_bus = nullptr;
  Arduino_GFX     *_gfx = nullptr;

  lv_obj_t *screen_ = nullptr;
  float estimatedCda_ = -1;
  lv_obj_t *cdaTitleLabel_ = nullptr;
  lv_obj_t *windLabel_ = nullptr;
  lv_obj_t *powerLabel_ = nullptr;
  lv_obj_t *avg3Label_ = nullptr;
  lv_obj_t *cadenceLabel_ = nullptr;
  lv_obj_t *timeLabel_ = nullptr;
  lv_obj_t *statusLabel_ = nullptr;
  // 主界面顶部 GPS 状态指示（图标 + 卫星数 + 搜星动画）
  lv_obj_t *gpsIndicatorSpinner_ = nullptr;
  lv_obj_t *gpsIndicatorLabel_ = nullptr;
  bool      gpsIndicatorFixed_ = false;   // 当前是否显示为已定位（避免重复刷新）
  lv_obj_t *powerSourceButton_ = nullptr;
  lv_obj_t *powerSourceLabel_ = nullptr;
  lv_obj_t *powerSourcePanel_ = nullptr;
  lv_obj_t *powerSourceTitle_ = nullptr;
  lv_obj_t *powerSourceHint_ = nullptr;
  lv_obj_t *powerSourceOptions_[3] = {};
  lv_obj_t *settingsPanel_ = nullptr;
  lv_obj_t *settingsTitle_ = nullptr;
  lv_obj_t *rideLogPanel_ = nullptr;
  lv_obj_t *rideLogText_ = nullptr;
  lv_obj_t *fitSaveButton_ = nullptr;
  lv_obj_t *fitSaveLabel_ = nullptr;
  bool fitSaveRequested_ = false;
  lv_obj_t *recordButton_ = nullptr;
  lv_obj_t *recordLabel_ = nullptr;
  bool recordingRequested_ = false;
  bool recordingRequestStart_ = false;
  bool recording_ = false;
  bool rideLogFromDashboard_ = false;
  char rideHistory_[360] = {};
  uint32_t rideLastSample_ = 0;
  lv_obj_t *settingsLanguageLabel_ = nullptr;
  lv_obj_t *settingsPowerLabel_ = nullptr;
  lv_obj_t *settingsPowerButton_ = nullptr;
  lv_obj_t *settingsPowerButtonLabel_ = nullptr;
  // 设置主页 3×2 入口按钮（功率/语言/Wi-Fi/声音/SD/GPS）
  lv_obj_t *settingsLanguageEntryButton_ = nullptr;
  lv_obj_t *settingsLanguageEntryLabel_ = nullptr;
  lv_obj_t *settingsWifiEntryButton_ = nullptr;
  lv_obj_t *settingsWifiEntryLabel_ = nullptr;
  // 独立「语言」页面
  lv_obj_t *languagePanel_ = nullptr;
  lv_obj_t *languageTitle_ = nullptr;
  lv_obj_t *languageHint_ = nullptr;
  lv_obj_t *settingsLanguageButtons_[2] = {};
  lv_obj_t *settingsLanguageButtonLabels_[2] = {};
  // 独立「Wi-Fi」页面
  lv_obj_t *wifiPanel_ = nullptr;
  lv_obj_t *wifiTitle_ = nullptr;
  lv_obj_t *settingsWifiLabel_ = nullptr;
  lv_obj_t *settingsWifiButtons_[2] = {};
  lv_obj_t *settingsWifiButtonLabels_[2] = {};
  lv_obj_t *settingsWifiStatus_ = nullptr;
  lv_obj_t *settingsSdButton_ = nullptr;
  lv_obj_t *settingsSdButtonLabel_ = nullptr;
  lv_obj_t *settingsSoundButton_ = nullptr;
  lv_obj_t *settingsSoundButtonLabel_ = nullptr;
  lv_obj_t *settingsGpsButton_ = nullptr;
  lv_obj_t *settingsGpsButtonLabel_ = nullptr;
  lv_obj_t *gpsPanel_ = nullptr;
  lv_obj_t *gpsTitle_ = nullptr;
  lv_obj_t *gpsFixLabel_ = nullptr;
  lv_obj_t *gpsUtcLabel_ = nullptr;
  lv_obj_t *gpsLocalLabel_ = nullptr;
  lv_obj_t *gpsCoordLabel_ = nullptr;
  lv_obj_t *gpsSatLabel_ = nullptr;
  lv_obj_t *gpsAltLabel_ = nullptr;
  lv_obj_t *gpsSpeedLabel_ = nullptr;
  lv_obj_t *gpsRestartButton_ = nullptr;
  lv_obj_t *gpsRestartLabel_ = nullptr;
  lv_obj_t *soundPanel_ = nullptr;
  lv_obj_t *soundTitle_ = nullptr;
  lv_obj_t *soundVolumeLabel_ = nullptr;
  lv_obj_t *soundHint_ = nullptr;
  lv_obj_t *soundVolumeSlider_ = nullptr;
  uint8_t   currentVolume_ = 70;
  int       requestedVolume_ = -1;
  lv_obj_t *sdPanel_ = nullptr;
  lv_obj_t *sdTitle_ = nullptr;
  lv_obj_t *sdStatusLabel_ = nullptr;
  lv_obj_t *sdCurrentPathLabel_ = nullptr;
  lv_obj_t *sdList_ = nullptr;              // 可滚动列表容器
  lv_obj_t *sdRowButtons_[12] = {};         // 文件行按钮
  lv_obj_t *sdRowLabels_[12] = {};          // 行文字
  lv_obj_t *sdUpButton_ = nullptr;
  lv_obj_t *sdUpLabel_ = nullptr;
  lv_obj_t *sdStopButton_ = nullptr;
  lv_obj_t *sdStopLabel_ = nullptr;
  lv_obj_t *sdNowPlayingLabel_ = nullptr;
  lv_obj_t *virtualPowerPanel_ = nullptr;
  lv_obj_t *virtualPowerValueLabel_ = nullptr;
  lv_obj_t *virtualCadenceValueLabel_ = nullptr;
  lv_obj_t *virtualTitle_ = nullptr;
  lv_obj_t *virtualPowerTitle_ = nullptr;
  lv_obj_t *virtualCadenceTitle_ = nullptr;
  lv_obj_t *virtualHint_ = nullptr;
  lv_obj_t *bleDevicePanel_ = nullptr;
  lv_obj_t *bleConfirmPanel_ = nullptr;
  lv_obj_t *bleLoadingPanel_ = nullptr;
  lv_obj_t *bleLoadingTitle_ = nullptr;
  lv_obj_t *bleLoadingLabel_ = nullptr;
  lv_obj_t *bleLoadingSpinner_ = nullptr;
  lv_obj_t *bleLoadingBackButton_ = nullptr;
  lv_obj_t *bleStatusLabel_ = nullptr;
  lv_obj_t *bleDeviceTitle_ = nullptr;
  lv_obj_t *bleDeviceHint_ = nullptr;
  lv_obj_t *bleScanLabel_ = nullptr;
  lv_obj_t *bleConfirmTitle_ = nullptr;
  lv_obj_t *bleConfirmButtonLabel_ = nullptr;
  lv_obj_t *bleLoadingBackLabel_ = nullptr;
  lv_obj_t *bleConfirmNameLabel_ = nullptr;
  lv_obj_t *bleConfirmAddressLabel_ = nullptr;
  lv_obj_t *bleDeviceButtons_[4] = {};
  lv_obj_t *cdaLabel_ = nullptr;
  lv_obj_t *zoneLabel_ = nullptr;
  lv_obj_t *postureLabel_ = nullptr;
  lv_obj_t *windLabelTitle_ = nullptr;

  int powerHistorySize_ = 3;
  float powerHistory_[3] = {0};
  int powerHistoryIndex_ = 0;

  float power3sAvg_ = 0.0f;
  float virtualPowerW_ = 0.0f;
  float virtualCadenceRpm_ = 0.0f;
  bool chinese_ = false;
  bool wifiEnabled_ = false;
  bool wifiConnected_ = false;
  bool wifiProvisioning_ = false;
  bool requestedWifiEnable_ = false;
  bool wifiEnableRequestPending_ = false;
  char wifiSsid_[33] = {};
  char wifiIp_[16] = {};
  bool bleScanCompleted_ = false;
  uint32_t currentZone_ = 0;
  int requestedPowerMode_ = -1;
  int selectedPowerMode_ = 0;
  bool requestedBleScan_ = false;
  bool requestedBleConnect_ = false;
  // 扫描阶段记录的被选设备真实地址类型，随连接请求一起回传给主循环。
  uint8_t requestedBleAddressType_ = 0xFF;
  bool requestedBleCancel_ = false;
  bool bleOperationActive_ = false;
  char requestedBleAddress_[18] = {};
  BlePowerDevice bleDevices_[4] = {};
  size_t bleDeviceCount_ = 0;
  size_t selectedBleDevice_ = 0;
  bool touchReady_ = false;
  UiNavigationGuard navigationGuard_;

  // BLE 相关按钮的触控缓冲：吃掉重复点击与界面切换后的穿透点击。
  TouchBuffer bleScanTouch_{BLE_UI_TOUCH_GUARD_MS};
  TouchBuffer bleDeviceTouch_{BLE_UI_TOUCH_GUARD_MS};
  TouchBuffer bleConfirmTouch_{BLE_UI_TOUCH_GUARD_MS};
  TouchBuffer bleCancelTouch_{BLE_UI_TOUCH_GUARD_MS};
  // 当前是否有 UI 侧提交的 BLE 请求尚未被主循环消费。
  // 期间拒绝新的同类点击，防止同一个请求被重复投递。
  bool bleRequestInFlight_ = false;

  // ---- SD 浏览状态 ----
  bool sdListRequested_ = false;
  bool sdPlayRequested_ = false;
  bool sdStopRequested_ = false;
  bool gpsRestartRequested_ = false;
  bool gpsModulePresent_ = false;
  bool gpsDetectDone_ = false;
  lv_obj_t *noticePanel_ = nullptr;      // 通用提示弹窗（如"找不到 GPS 模块"）
  lv_obj_t *noticeLabel_ = nullptr;
  lv_obj_t *noticeBackLabel_ = nullptr;
  char sdPlayPath_[256] = {};
  char sdCurrentDirectory_[128] = "/";
  SdFileEntry sdFiles_[12] = {};
  size_t sdFileCount_ = 0;
  char sdListStatus_[64] = {};
  bool audioPlaying_ = false;
  char audioTitle_[96] = {};

  static void lvglFlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
  static void lvglTouchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data);
  static void onBackNavigation(lv_event_t *event);
  static void onPowerSourceButton(lv_event_t *event);
  static void onSettingsButton(lv_event_t *event);
  static void onSettingsBack(lv_event_t *event);
  static void onRideLogButton(lv_event_t *event);
  static void onRideLogBack(lv_event_t *event);
  static void onFitSave(lv_event_t *event);
  static void onRecordToggle(lv_event_t *event);
  static void onLanguageSelected(lv_event_t *event);
  static void onLanguageButton(lv_event_t *event);
  static void onLanguageBack(lv_event_t *event);
  static void onWifiButton(lv_event_t *event);
  static void onWifiBack(lv_event_t *event);
  static void onWifiSelected(lv_event_t *event);
  static void onBleSelected(lv_event_t *event);
  static void onBleScanAgain(lv_event_t *event);
  static void onBleDeviceSelected(lv_event_t *event);
  static void onBleConfirm(lv_event_t *event);
  static void onBleBack(lv_event_t *event);
  static void onBleConfirmBack(lv_event_t *event);
  static void onBleLoadingBack(lv_event_t *event);
  static void onAutoSelected(lv_event_t *event);
  static void onVirtualSelected(lv_event_t *event);
  static void onVirtualBack(lv_event_t *event);
  static void onVirtualSliderChanged(lv_event_t *event);
  static void onCancelSelection(lv_event_t *event);
  static void onSoundButton(lv_event_t *event);
  static void onSoundBack(lv_event_t *event);
  static void onVolumeSliderChanged(lv_event_t *event);
  static void onSdCardButton(lv_event_t *event);
  static void onSdBack(lv_event_t *event);
  static void onSdFileClicked(lv_event_t *event);
  static void onSdUpClicked(lv_event_t *event);
  static void onSdStopClicked(lv_event_t *event);
  static void onGpsButton(lv_event_t *event);
  static void onGpsBack(lv_event_t *event);
  static void onGpsRestartClicked(lv_event_t *event);
  static void onNoticeDismiss(lv_event_t *event);
  void showNotice(const char *message);
  static uint16_t rgb565FromLvColor(lv_color_t c);
  bool readTouch(uint16_t &x, uint16_t &y);
  bool initTouch();
  bool touchCommand(uint16_t reg);
  bool touchReadRegister(uint16_t reg, uint8_t *data, size_t len);
  bool touchWriteRegister(uint16_t reg, uint8_t value);
  void buildPowerSourcePanel();
  void buildVirtualPowerPanel();
  void buildSettingsPanel();
  void buildLanguagePanel();
  void buildWifiPanel();
  void buildSoundPanel();
  void buildSdPanel();
  void buildGpsPanel();
  void buildNoticePanel();
  void refreshSoundVolume();
  void refreshSdList();
  void refreshLanguage();
  void refreshBleDeviceButtons();
  void setLanguage(bool chinese);
  void refreshWifiStatus();
  const char *localized(const char *english, const char *chinese) const {
    return chinese_ ? chinese : english;
  }
  void buildPanelHeader(lv_obj_t *panel, const char *title, lv_event_cb_t backCallback);
};
