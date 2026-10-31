#include "display_lcd.h"
#include "runtime_log.h"
#include "config.h"
#include <cstring>
#include "saved_connections.h"
#include "dashboard_settings.h"

extern const lv_font_t lv_font_noto_sans_sc_14;

namespace {
static DisplayLcd *g_display = nullptr;
constexpr uint8_t CST328_ADDR = 0x1A;
}

uint16_t DisplayLcd::rgb565FromLvColor(lv_color_t c) {
  uint8_t r = c.ch.red;
  uint8_t g = c.ch.green;
  uint8_t b = c.ch.blue;
  return (uint16_t)(((r << 11) & 0xF800) | ((g << 5) & 0x07E0) | (b & 0x001F));
}

void DisplayLcd::lvglFlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  if (!g_display || !g_display->_gfx) {
    lv_disp_flush_ready(drv);
    return;
  }

  int32_t w = lv_area_get_width(area);
  int32_t h = lv_area_get_height(area);
  if (w <= 0 || h <= 0) {
    lv_disp_flush_ready(drv);
    return;
  }

  // 关键修复：不再每帧 malloc。
  // 旧实现每次 flush 都 malloc(w*h*2) 再 free；多面板/全屏刷新时切块大、
  // 运行一段时间堆碎片化后 malloc 可能返回 NULL，于是直接 flush_ready 跳过
  // 绘制 —— 表现为"界面不刷新但主循环日志照常"。这里改用静态缓冲，
  // 按行分段绘制，容量取 draw buffer 像素数（240*320/10 = 7680），永不越界/失败。
  static uint16_t s_flushBuf[240 * 320 / 10];

  int32_t cap = (int32_t)(sizeof(s_flushBuf) / sizeof(s_flushBuf[0]));
  int32_t rowsPerChunk = cap / w;
  if (rowsPerChunk < 1) rowsPerChunk = 1;

  for (int32_t y0 = 0; y0 < h; y0 += rowsPerChunk) {
    int32_t rows = (h - y0 < rowsPerChunk) ? (h - y0) : rowsPerChunk;
    int32_t n = rows * w;
    const lv_color_t *src = color_p + (size_t)y0 * w;
    for (int32_t i = 0; i < n; ++i) s_flushBuf[i] = rgb565FromLvColor(src[i]);
    g_display->_gfx->draw16bitRGBBitmap(area->x1, area->y1 + y0, s_flushBuf, w, rows);
  }

  lv_disp_flush_ready(drv);
}

bool DisplayLcd::touchCommand(uint16_t reg) {
  Wire1.beginTransmission(CST328_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  return Wire1.endTransmission() == 0;
}

bool DisplayLcd::touchReadRegister(uint16_t reg, uint8_t *data, size_t len) {
  Wire1.beginTransmission(CST328_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom(CST328_ADDR, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; ++i) data[i] = (uint8_t)Wire1.read();
  return true;
}

bool DisplayLcd::touchWriteRegister(uint16_t reg, uint8_t value) {
  Wire1.beginTransmission(CST328_ADDR);
  Wire1.write((uint8_t)(reg >> 8));
  Wire1.write((uint8_t)reg);
  Wire1.write(value);
  return Wire1.endTransmission() == 0;
}

bool DisplayLcd::initTouch() {
  pinMode(PIN_TOUCH_INT, INPUT);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(50);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(5);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(50);

  Wire1.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 400000);
  if (!touchCommand(0xD101)) return false;

  uint8_t info[24] = {};
  if (!touchReadRegister(0xD1F4, info, sizeof(info))) return false;
  uint16_t signature = ((uint16_t)info[11] << 8) | info[10];
  if (signature != 0xCACA) {
    AppLog.printf("[TOUCH] unexpected CST328 signature 0x%04X\n", signature);
    touchCommand(0xD109);
    return false;
  }
  return touchCommand(0xD109);
}

bool DisplayLcd::readTouch(uint16_t &x, uint16_t &y) {
  uint8_t count = 0;
  if (!touchReadRegister(0xD005, &count, 1)) return false;
  count &= 0x0F;
  if (count == 0 || count > 5) {
    touchWriteRegister(0xD005, 0);
    return false;
  }

  uint8_t points[27] = {};
  if (!touchReadRegister(0xD000, points, sizeof(points))) {
    touchWriteRegister(0xD005, 0);
    return false;
  }
  touchWriteRegister(0xD005, 0);
  x = (uint16_t)((uint16_t)points[1] << 4) | (points[3] >> 4);
  y = (uint16_t)((uint16_t)points[2] << 4) | (points[3] & 0x0F);
  return x < 240 && y < 320;
}

void DisplayLcd::lvglTouchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  uint16_t x = 0;
  uint16_t y = 0;
  const bool pressed = g_display && g_display->touchReady_ &&
                       g_display->readTouch(x, y);
  if (g_display && g_display->navigationGuard_.filterTouch(pressed, millis())) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void DisplayLcd::onBackNavigation(lv_event_t *event) {
  if (!g_display || !g_display->navigationGuard_.beginBack(millis())) {
    lv_event_stop_processing(event);
    return;
  }
  lv_indev_t *input = lv_indev_get_act();
  if (input) lv_indev_wait_release(input);
}

void DisplayLcd::onPowerSourceButton(lv_event_t *) {
  if (!g_display) return;
  // 重新进入功率源页：重置 BLE 按钮缓冲，页面切换瞬间的点击不算数。
  g_display->bleScanTouch_.reset(millis());
  g_display->bleRequestInFlight_ = false;
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->sdPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onSettingsButton(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onResetDashboard(lv_event_t *) {
  if (!g_display) return;
  g_display->dashboard_.reset();
  g_display->showNotice("Dashboard layout reset.");
}

void DisplayLcd::onSettingsBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onRideLogButton(lv_event_t *) {
  if (!g_display) return;
  g_display->rideLogFromDashboard_ = lv_obj_has_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->rideLogPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onRideLogBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->rideLogPanel_, LV_OBJ_FLAG_HIDDEN);
  if (!g_display->rideLogFromDashboard_)
    lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onFitSave(lv_event_t *) {
  if (g_display) g_display->fitSaveRequested_ = true;
}

bool DisplayLcd::takeFitSaveRequest() {
  const bool requested = fitSaveRequested_;
  fitSaveRequested_ = false;
  return requested;
}

void DisplayLcd::setFitSaving(bool saving) {
  fitSaving_ = saving;
  setRecording(recording_, recordingArmed_);
  if (!fitSaveButton_) return;
  if (saving) lv_obj_add_state(fitSaveButton_, LV_STATE_DISABLED);
  else lv_obj_clear_state(fitSaveButton_, LV_STATE_DISABLED);
  const char *text = saving ? localized("SAVING", "保存中") : localized("SAVE FIT", "保存 FIT");
  if (strcmp(lv_label_get_text(fitSaveLabel_), text) != 0) lv_label_set_text(fitSaveLabel_, text);
}

void DisplayLcd::showFitResult(const char *message) { showNotice(message); }

void DisplayLcd::onRecordToggle(lv_event_t *event) {
  if (!g_display) return;
  const bool start = (uintptr_t)lv_event_get_user_data(event) == 1;
  if (g_display->recordingRequested_ || g_display->fitSaving_) return;
  if (start == (g_display->recording_ || g_display->recordingArmed_)) return;
  g_display->recordingRequestStart_ = start;
  g_display->recordingRequested_ = true;
  lv_obj_add_state(g_display->recordButton_, LV_STATE_DISABLED);
  lv_obj_add_state(g_display->stopRecordButton_, LV_STATE_DISABLED);
}

bool DisplayLcd::takeRecordingRequest(bool &start) {
  if (!recordingRequested_) return false;
  recordingRequested_ = false;
  start = recordingRequestStart_;
  return true;
}

void DisplayLcd::setRecording(bool recording, bool armed) {
  recording_ = recording;
  recordingArmed_ = armed;
  if (!recordButton_) return;
  if (recording || armed || fitSaving_ || recordingRequested_)
    lv_obj_add_state(recordButton_, LV_STATE_DISABLED);
  else lv_obj_clear_state(recordButton_, LV_STATE_DISABLED);
  if (stopRecordButton_) {
    if ((!recording && !armed) || fitSaving_ || recordingRequested_)
      lv_obj_add_state(stopRecordButton_, LV_STATE_DISABLED);
    else lv_obj_clear_state(stopRecordButton_, LV_STATE_DISABLED);
  }
  if (recordLabel_) {
    const char *text = armed ? localized("WAITING", "等待数据")
                            : localized(LV_SYMBOL_PLAY " START", LV_SYMBOL_PLAY " 开始");
    if (strcmp(lv_label_get_text(recordLabel_), text) != 0) lv_label_set_text(recordLabel_, text);
  }
}

void DisplayLcd::updateRideLog(const char *path, bool recording, uint32_t samples,
                              uint32_t dropped, float power, float cadence, float speed,
                              uint32_t durationMs, uint32_t skipped) {
  if (!rideLogText_) return;
  snprintf(ridePath_, sizeof(ridePath_), "%s", path ? path : "");
  rideDurationMs_ = durationMs; rideSamples_ = samples; rideDropped_ = dropped; rideSkipped_ = skipped;
  ridePower_ = power; rideCadence_ = cadence; rideSpeed_ = speed;
  if (samples < rideLastSample_) rideHistory_[0] = '\0';
  if (samples != rideLastSample_) {
    char row[64];
    snprintf(row, sizeof(row), "#%lu %.0fW %.0frpm %.1fkm/h\n",
             (unsigned long)samples, power, cadence, speed * 3.6f);
    while (strlen(rideHistory_) + strlen(row) >= sizeof(rideHistory_)) {
      const char *next = strchr(rideHistory_, '\n');
      if (!next) { rideHistory_[0] = '\0'; break; }
      memmove(rideHistory_, next + 1, strlen(next + 1) + 1);
    }
    strcat(rideHistory_, row);
    rideLastSample_ = samples;
  }
  refreshRideLog();
}

void DisplayLcd::refreshRideLog() {
  if (!rideLogText_) return;
  const uint32_t seconds = rideDurationMs_ / 1000;
  char duration[40];
  snprintf(duration, sizeof(duration), "%s  %02lu:%02lu:%02lu",
           recordingArmed_ ? localized("WAITING", "等待数据") : localized("TIME", "录制时长"),
           (unsigned long)(seconds / 3600), (unsigned long)(seconds / 60 % 60),
           (unsigned long)(seconds % 60));
  lv_label_set_text(rideDurationLabel_, duration);
  char text[800];
  snprintf(text, sizeof(text),
           localized("%s\n%s\n\nSamples: %lu\nDropped: %lu\nSkipped invalid: %lu\n\nPower: %.0f W\nCadence: %.0f rpm\nSpeed: %.1f km/h\n\nRecent samples:\n%s\nSTOP saves FIT automatically.\nOriginal CSV is retained.",
                     "%s\n%s\n\n样本: %lu\n丢失: %lu\n跳过无效: %lu\n\n功率: %.0f W\n踏频: %.0f rpm\n速度: %.1f km/h\n\n最近样本:\n%s\n停止后自动保存 FIT。\n保留原始 CSV。"),
           fitSaving_ ? localized("SAVING", "保存中") : recordingArmed_ ? localized("WAITING", "等待数据") :
           recording_ ? localized("RECORDING", "记录中") : localized("STOPPED", "已停止"),
           ridePath_[0] ? ridePath_ : localized("No recording yet", "尚未记录"),
           (unsigned long)rideSamples_, (unsigned long)rideDropped_, (unsigned long)rideSkipped_,
           ridePower_, rideCadence_, rideSpeed_ * 3.6f, rideHistory_);
  lv_label_set_text(rideLogText_, text);
}

void DisplayLcd::onLanguageButton(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->languagePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onLanguageBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->languagePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onWifiButton(lv_event_t *) {
  if (!g_display) return;
  g_display->refreshWifiStatus();
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->wifiPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onWifiBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->wifiPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onWifiSetup(lv_event_t *) {
  if (g_display) g_display->wifiSetupRequested_ = true;
}

void DisplayLcd::onLanguageSelected(lv_event_t *event) {
  if (!g_display) return;
  const bool chinese = (uintptr_t)lv_event_get_user_data(event) == 1;
  g_display->setLanguage(chinese);
}

void DisplayLcd::onWifiSelected(lv_event_t *event) {
  if (!g_display) return;
  g_display->requestedWifiEnable_ =
      (uintptr_t)lv_event_get_user_data(event) == 1;
  g_display->wifiEnableRequestPending_ = true;
}

bool DisplayLcd::takeWifiEnableRequest(bool &enabled) {
  if (!wifiEnableRequestPending_) return false;
  wifiEnableRequestPending_ = false;
  enabled = requestedWifiEnable_;
  return true;
}

void DisplayLcd::updateWifiStatus(bool enabled, bool connected,
                                  bool provisioning, const char *ssid,
                                  const char *ip) {
  wifiEnabled_ = enabled;
  wifiConnected_ = connected;
  wifiProvisioning_ = provisioning;
  snprintf(wifiSsid_, sizeof(wifiSsid_), "%s", ssid ? ssid : "");
  snprintf(wifiIp_, sizeof(wifiIp_), "%s", ip ? ip : "");
  refreshWifiStatus();
}

void DisplayLcd::refreshWifiStatus() {
  if (!settingsWifiButtons_[0] || !settingsWifiStatus_) return;
  for (size_t i = 0; i < 2; ++i) {
    const bool selected = wifiEnabled_ == (i == 1);
    lv_obj_set_style_bg_color(settingsWifiButtons_[i],
                              selected ? lv_color_make(37, 99, 235)
                                       : lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_text_color(settingsWifiButtonLabels_[i],
                                selected ? lv_color_white()
                                         : lv_color_make(30, 41, 59), 0);
  }

  char status[120];
  if (!wifiEnabled_) {
    snprintf(status, sizeof(status), "%s",
             localized("Wi-Fi is off", "Wi-Fi 已关闭"));
  } else if (wifiProvisioning_) {
    snprintf(status, sizeof(status), "%s",
             localized("Waiting for Wi-Fi setup", "等待 Wi-Fi 配网"));
  } else if (!wifiConnected_) {
    snprintf(status, sizeof(status), "%s",
             localized("Connecting to Wi-Fi...", "正在连接 Wi-Fi..."));
  } else if (chinese_) {
    snprintf(status, sizeof(status), "已连接：%.28s\nhttp://aeroprobe.local/\nhttp://%.15s/",
             wifiSsid_, wifiIp_);
  } else {
    snprintf(status, sizeof(status), "Connected: %.28s\nhttp://aeroprobe.local/\nhttp://%.15s/",
             wifiSsid_, wifiIp_);
  }

  if (strcmp(lv_label_get_text(settingsWifiStatus_), status) != 0)
    lv_label_set_text(settingsWifiStatus_, status);
}

void DisplayLcd::onBleFavorite(lv_event_t *) {
  if (!g_display || !g_display->connectedBlePower_ ||
      !g_display->bleFavoriteTouch_.accept(millis())) return;
  SavedConnections saved;
  if (!saved.loadFavorites() || !saved.setFavorite(g_display->connectedBleAddress_,
      g_display->connectedBleType_, g_display->connectedBleName_,
      !saved.isFavorite(g_display->connectedBleAddress_, g_display->connectedBleType_))) {
    g_display->showBleConnectFailure("Could not save favorite (limit 4). Existing favorites retained.");
    return;
  }
  g_display->updateBleFavoriteAvailability(true);
}

void DisplayLcd::setConnectedBle(const char *address, uint8_t type, bool powerService) {
  snprintf(connectedBleAddress_, sizeof(connectedBleAddress_), "%s", address);
  snprintf(connectedBleName_, sizeof(connectedBleName_), "%s", address);
  connectedBleType_ = type;
  for (size_t index = 0; index < bleDeviceCount_; ++index)
    if (!strcasecmp(bleDevices_[index].address, address) && bleDevices_[index].addressType == type)
      snprintf(connectedBleName_, sizeof(connectedBleName_), "%s", bleDevices_[index].name);
  SavedConnections saved;
  if (saved.loadFavorites())
    for (uint8_t index = 0; index < saved.favoriteCount(); ++index)
      if (!strcasecmp(saved.favorite(index)->device.address, address) && saved.favorite(index)->device.addressType == type)
        snprintf(connectedBleName_, sizeof(connectedBleName_), "%s", saved.favorite(index)->name);
  connectedBlePower_ = powerService;
  updateBleFavoriteAvailability(powerService);
}

void DisplayLcd::updateBleFavoriteAvailability(bool connected) {
  if (!bleFavoriteButton_) return;
  connectedBlePower_ = connectedBlePower_ && connected;
  if (connectedBlePower_) lv_obj_clear_state(bleFavoriteButton_, LV_STATE_DISABLED);
  else lv_obj_add_state(bleFavoriteButton_, LV_STATE_DISABLED);
  lv_label_set_text(bleFavoriteLabel_, connectedBleAddress_[0] ? connectedBleName_ : "--");
  SavedConnections saved;
  const bool favorite = saved.loadFavorites() && saved.isFavorite(connectedBleAddress_, connectedBleType_);
  lv_obj_set_style_line_color(bleFavoriteStar_, favorite ? lv_color_make(217, 119, 6) : lv_color_make(100, 116, 139), 0);
  lv_obj_set_style_line_width(bleFavoriteStar_, favorite ? 4 : 2, 0);
}

void DisplayLcd::onBleSelected(lv_event_t *) {
  if (!g_display) return;
  if (!g_display->bleScanTouch_.accept(millis())) return;
  // 上一次 BLE 请求还没被主循环消费：直接忽略，避免重复扫描。
  if (g_display->bleRequestInFlight_) return;
  g_display->requestedPowerMode_ = 1;
  g_display->requestedBleScan_ = true;
  g_display->bleRequestInFlight_ = true;
  g_display->selectedPowerMode_ = 1;
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  g_display->showBleLoading("Scanning for BLE power meters and smart trainers...");
}

void DisplayLcd::onBleScanAgain(lv_event_t *) {
  if (!g_display) return;
  if (!g_display->bleScanTouch_.accept(millis())) return;
  if (g_display->bleRequestInFlight_) return;
  g_display->requestedBleScan_ = true;
  g_display->bleRequestInFlight_ = true;
  g_display->showBleLoading("Scanning for BLE power meters and smart trainers...");
}

void DisplayLcd::onBleDeviceSelected(lv_event_t *event) {
  if (!g_display) return;
  if (!g_display->bleDeviceTouch_.accept(millis())) return;
  size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
  if (index >= g_display->bleDeviceCount_) return;

  g_display->selectedBleDevice_ = index;
  lv_label_set_text(g_display->bleConfirmNameLabel_,
                    g_display->bleDevices_[index].name);
  lv_label_set_text(g_display->bleConfirmAddressLabel_,
                    g_display->bleDevices_[index].address);
  if (g_display->bleDevices_[index].postureSensor) {
    char details[64];
    snprintf(details, sizeof(details), "%s\nPosture sensor", g_display->bleDevices_[index].address);
    lv_label_set_text(g_display->bleConfirmAddressLabel_, details);
  }
  if (g_display->bleDevices_[index].heartRate) {
    char details[64];
    snprintf(details, sizeof(details), "%s\nHeart Rate (HRS)", g_display->bleDevices_[index].address);
    lv_label_set_text(g_display->bleConfirmAddressLabel_, details);
  }
  if (g_display->bleDevices_[index].cyclingPower ||
      g_display->bleDevices_[index].fitnessMachine) {
    const char *profile = g_display->bleDevices_[index].cyclingPower
                              ? (g_display->bleDevices_[index].fitnessMachine
                                     ? g_display->localized("Cycling Power + FTMS trainer",
                                                            "功率计 + 智能骑行台")
                                     : g_display->localized("Cycling Power", "功率计"))
                              : (g_display->bleDevices_[index].fitnessMachine
                                     ? g_display->localized("FTMS smart trainer", "FTMS 智能骑行台")
                                     : g_display->localized("CSC cadence/speed only", "CSC 仅踏频/速度"));
    char details[64];
    snprintf(details, sizeof(details), "%s\n%s",
             g_display->bleDevices_[index].address, profile);
    lv_label_set_text(g_display->bleConfirmAddressLabel_, details);
  }
  lv_obj_clear_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onBleConfirm(lv_event_t *) {
  if (!g_display || g_display->selectedBleDevice_ >= g_display->bleDeviceCount_) return;
  // 触控缓冲：同一次按下只接受一次，彻底杜绝日志里那种同一设备被连续
  // 确认两次（Second confirm while the first still running）的情况。
  if (!g_display->bleConfirmTouch_.accept(millis())) return;
  if (g_display->bleRequestInFlight_) return;
  snprintf(g_display->requestedBleAddress_, sizeof(g_display->requestedBleAddress_),
           "%s", g_display->bleDevices_[g_display->selectedBleDevice_].address);
  g_display->requestedBlePosture_ = g_display->bleDevices_[g_display->selectedBleDevice_].postureSensor &&
      !g_display->bleDevices_[g_display->selectedBleDevice_].cyclingPower &&
      !g_display->bleDevices_[g_display->selectedBleDevice_].fitnessMachine;
    g_display->requestedBleHeart_ = g_display->bleDevices_[g_display->selectedBleDevice_].heartRate &&
        !g_display->bleDevices_[g_display->selectedBleDevice_].cyclingPower &&
        !g_display->bleDevices_[g_display->selectedBleDevice_].fitnessMachine;
  // 连同扫描时记录的地址类型一起提交，主循环透传给 connectDevice()。
  g_display->requestedBleAddressType_ =
      g_display->bleDevices_[g_display->selectedBleDevice_].addressType;
  g_display->requestedBleConnect_ = true;
  g_display->bleRequestInFlight_ = true;
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  g_display->showBleLoading("Connecting to selected BLE device...");
}

void DisplayLcd::onBleBack(lv_event_t *) {
  if (!g_display) return;
  if (!g_display->bleCancelTouch_.accept(millis())) return;
  g_display->requestedBleCancel_ = true;
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onBleConfirmBack(lv_event_t *) {
  if (!g_display) return;
  if (!g_display->bleCancelTouch_.accept(millis())) return;
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onBleLoadingBack(lv_event_t *) {
  if (!g_display) return;
  if (!g_display->bleCancelTouch_.accept(millis())) return;
  if (g_display->bleOperationActive_) {
    g_display->requestedBleCancel_ = true;
    lv_obj_t *buttonLabel = lv_obj_get_child(g_display->bleLoadingBackButton_, 0);
    lv_label_set_text(buttonLabel,
                      g_display->localized("WAIT...", "请稍候..."));
    lv_obj_add_state(g_display->bleLoadingBackButton_, LV_STATE_DISABLED);
    lv_label_set_text(g_display->bleLoadingLabel_,
                      g_display->localized("Cancelling operation...\nPlease wait for BLE to release.",
                                           "正在取消操作...\n请等待蓝牙释放。"));
    return;
  }

  lv_obj_add_flag(g_display->bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onVirtualSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 3;
  g_display->selectedPowerMode_ = 3;
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onVirtualBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onVirtualSliderChanged(lv_event_t *event) {
  if (!g_display) return;
  lv_obj_t *slider = lv_event_get_target(event);
  const int value = lv_slider_get_value(slider);
  char text[24];
  if ((uintptr_t)lv_event_get_user_data(event) == 1) {
    g_display->virtualPowerW_ = (float)value;
    snprintf(text, sizeof(text), "%d W", value);
    lv_label_set_text(g_display->virtualPowerValueLabel_, text);
  } else {
    g_display->virtualCadenceRpm_ = (float)value;
    snprintf(text, sizeof(text), "%d rpm", value);
    lv_label_set_text(g_display->virtualCadenceValueLabel_, text);
  }
}

void DisplayLcd::onAutoSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 0;
  g_display->selectedPowerMode_ = 0;
  g_display->refreshLanguage();
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onCancelSelection(lv_event_t *) {
  if (g_display && g_display->powerSourcePanel_) {
    lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  }
}

// ---- 声音设置 ----
void DisplayLcd::onSoundButton(lv_event_t *) {
  if (!g_display) return;
  g_display->refreshSoundVolume();
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->soundPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onSoundBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->soundPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onVolumeSliderChanged(lv_event_t *event) {
  if (!g_display) return;
  const int vol = lv_slider_get_value(lv_event_get_target(event));
  g_display->currentVolume_ = (uint8_t)vol;
  // Commit on release to avoid writing NVS for every drag event.
  if (lv_event_get_code(event) == LV_EVENT_RELEASED)
    g_display->requestedVolume_ = vol;
  g_display->refreshSoundVolume();
}

bool DisplayLcd::takeVolumeRequest(uint8_t &vol) {
  if (requestedVolume_ < 0) return false;
  vol = (uint8_t)requestedVolume_;
  requestedVolume_ = -1;
  return true;
}

void DisplayLcd::setVolume(uint8_t vol) {
  currentVolume_ = vol > 100 ? 100 : vol;
  refreshSoundVolume();
}

void DisplayLcd::refreshSoundVolume() {
  if (!soundVolumeLabel_) return;
  char text[32];
  snprintf(text, sizeof(text), localized("Volume: %u%%", "音量: %u%%"),
           (unsigned)currentVolume_);
  lv_label_set_text(soundVolumeLabel_, text);

  if (soundVolumeSlider_ &&
      lv_slider_get_value(soundVolumeSlider_) != currentVolume_)
    lv_slider_set_value(soundVolumeSlider_, currentVolume_, LV_ANIM_OFF);
}

void DisplayLcd::onSdCardButton(lv_event_t *) {
  if (!g_display) return;
  snprintf(g_display->sdCurrentDirectory_, sizeof(g_display->sdCurrentDirectory_), "/");
  g_display->sdListRequested_ = true;   // 由主循环加载文件列表
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(g_display->sdStatusLabel_,
                    g_display->localized("Loading files...", "正在读取文件..."));
  for (size_t i = 0; i < 12; ++i)
    lv_obj_add_flag(g_display->sdRowButtons_[i], LV_OBJ_FLAG_HIDDEN);
  if (g_display->sdCurrentPathLabel_)
    lv_label_set_text(g_display->sdCurrentPathLabel_, "/");
  if (g_display->sdUpButton_)
    lv_obj_add_flag(g_display->sdUpButton_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->sdPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onSdBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->sdPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onSdFileClicked(lv_event_t *event) {
  if (!g_display) return;
  const size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
  if (index >= g_display->sdFileCount_) return;
  const SdFileEntry &entry = g_display->sdFiles_[index];
  if (entry.isDirectory) {
    char nextDirectory[sizeof(g_display->sdCurrentDirectory_)];
    const int length = strcmp(g_display->sdCurrentDirectory_, "/") == 0
      ? snprintf(nextDirectory, sizeof(nextDirectory), "/%s", entry.name)
      : snprintf(nextDirectory, sizeof(nextDirectory), "%s/%s",
             g_display->sdCurrentDirectory_, entry.name);
    if (length < 0 || (size_t)length >= sizeof(nextDirectory)) {
      lv_label_set_text(g_display->sdStatusLabel_,
                        g_display->localized("Path is too deep", "目录层级过深"));
      return;
    }
    snprintf(g_display->sdCurrentDirectory_,
             sizeof(g_display->sdCurrentDirectory_), "%s", nextDirectory);
    g_display->sdListRequested_ = true;
    lv_label_set_text(g_display->sdStatusLabel_,
                      g_display->localized("Loading...", "正在读取..."));
    for (size_t i = 0; i < 12; ++i)
      lv_obj_add_flag(g_display->sdRowButtons_[i], LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (!entry.isAudio) {
    lv_label_set_text(g_display->sdNowPlayingLabel_,
                      g_display->localized("Not an audio file", "不是音频文件"));
    return;
  }
  const int pathLength = strcmp(g_display->sdCurrentDirectory_, "/") == 0
      ? snprintf(g_display->sdPlayPath_, sizeof(g_display->sdPlayPath_),
                 "/%s", entry.name)
      : snprintf(g_display->sdPlayPath_, sizeof(g_display->sdPlayPath_),
                 "%s/%s", g_display->sdCurrentDirectory_, entry.name);
  if (pathLength < 0 || (size_t)pathLength >= sizeof(g_display->sdPlayPath_)) {
    lv_label_set_text(g_display->sdNowPlayingLabel_,
                      g_display->localized("Path is too long", "文件路径过长"));
    return;
  }
  g_display->sdPlayRequested_ = true;
}

void DisplayLcd::onSdUpClicked(lv_event_t *) {
  if (!g_display || strcmp(g_display->sdCurrentDirectory_, "/") == 0) return;
  char *separator = strrchr(g_display->sdCurrentDirectory_, '/');
  if (!separator || separator == g_display->sdCurrentDirectory_)
    snprintf(g_display->sdCurrentDirectory_,
             sizeof(g_display->sdCurrentDirectory_), "/");
  else
    *separator = '\0';
  g_display->sdListRequested_ = true;
  lv_label_set_text(g_display->sdStatusLabel_,
                    g_display->localized("Loading...", "正在读取..."));
  for (size_t i = 0; i < 12; ++i)
    lv_obj_add_flag(g_display->sdRowButtons_[i], LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onSdStopClicked(lv_event_t *) {
  if (!g_display) return;
  g_display->sdStopRequested_ = true;
}

bool DisplayLcd::takeSdListRequest(char *directory, size_t capacity) {
  if (!sdListRequested_) return false;
  sdListRequested_ = false;
  if (!directory || capacity == 0) return false;
  snprintf(directory, capacity, "%s", sdCurrentDirectory_);
  return true;
}

bool DisplayLcd::takeSdPlayRequest(char *path, size_t capacity) {
  if (!sdPlayRequested_) return false;
  sdPlayRequested_ = false;
  if (!path || capacity == 0) return false;
  snprintf(path, capacity, "%s", sdPlayPath_);
  return true;
}

bool DisplayLcd::takeSdStopRequest() {
  if (!sdStopRequested_) return false;
  sdStopRequested_ = false;
  return true;
}

// ---- GPS 页面 ----
void DisplayLcd::onGpsButton(lv_event_t *) {
  if (!g_display) return;
  // 已完成检测且确认未找到模块 → 直接提示，不进入 GPS 页。
  if (g_display->gpsDetectDone_ && !g_display->gpsModulePresent_) {
    g_display->showNotice(g_display->localized(
        "GPS module not found.\nCheck the UART wiring and power.",
        "找不到 GPS 模块。\n请检查 UART 接线和供电。"));
    return;
  }
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->gpsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onNoticeDismiss(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->noticePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::showNotice(const char *message) {
  if (!noticePanel_) return;
  if (noticeLabel_) lv_label_set_text(noticeLabel_, message ? message : "");
  lv_obj_move_foreground(noticePanel_);
  lv_obj_clear_flag(noticePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onGpsBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->gpsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onGpsRestartClicked(lv_event_t *) {
  if (!g_display) return;
  g_display->gpsRestartRequested_ = true;
  lv_label_set_text(g_display->gpsFixLabel_,
                    g_display->localized("Restarting GPS...", "正在重启 GPS..."));
}

bool DisplayLcd::takeGpsRestartRequest() {
  if (!gpsRestartRequested_) return false;
  gpsRestartRequested_ = false;
  return true;
}

void DisplayLcd::showGpsPanel() {
  if (!gpsPanel_) return;
  if (rideLogPanel_) lv_obj_add_flag(rideLogPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(soundPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(sdPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(gpsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::buildBleFavoriteButton() {
  bleFavoriteButton_ = lv_btn_create(powerSourcePanel_);
  lv_obj_set_size(bleFavoriteButton_, 208, 44);
  lv_obj_align(bleFavoriteButton_, LV_ALIGN_TOP_LEFT, 16, 244);
  lv_obj_set_style_radius(bleFavoriteButton_, 0, 0);
  lv_obj_set_style_pad_all(bleFavoriteButton_, 8, 0);
  lv_obj_set_style_bg_color(bleFavoriteButton_, lv_color_make(226, 232, 240), 0);
  lv_obj_add_event_cb(bleFavoriteButton_, onBleFavorite, LV_EVENT_CLICKED, nullptr);
  bleFavoriteStar_ = lv_line_create(bleFavoriteButton_);
  static const lv_point_t starPoints[] = {{12, 0}, {15, 8}, {24, 9}, {17, 15}, {20, 24},
                                         {12, 19}, {4, 24}, {7, 15}, {0, 9}, {9, 8}, {12, 0}};
  lv_line_set_points(bleFavoriteStar_, starPoints, 11);
  lv_obj_align(bleFavoriteStar_, LV_ALIGN_LEFT_MID, 0, 0);
  bleFavoriteLabel_ = lv_label_create(bleFavoriteButton_);
  lv_obj_set_width(bleFavoriteLabel_, 150);
  lv_label_set_long_mode(bleFavoriteLabel_, LV_LABEL_LONG_DOT);
  lv_obj_align(bleFavoriteLabel_, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_text_font(bleFavoriteLabel_, &lv_font_noto_sans_sc_14, 0);
  lv_obj_set_style_text_color(bleFavoriteLabel_, lv_color_make(30, 41, 59), 0);
  updateBleFavoriteAvailability(false);
}

void DisplayLcd::showSettings() {
  if (!settingsPanel_) return;
  if (rideLogPanel_) lv_obj_add_flag(rideLogPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(soundPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(sdPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(gpsPanel_, LV_OBJ_FLAG_HIDDEN);
  if (languagePanel_) lv_obj_add_flag(languagePanel_, LV_OBJ_FLAG_HIDDEN);
  if (wifiPanel_) lv_obj_add_flag(wifiPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(settingsPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::updateGpsStatus(bool enabled, bool fix, uint8_t sat, uint32_t hdop,
                                 double lat, double lon, float altM, float speedMps,
                                 const char *utc, const char *local, uint32_t ageMs) {
  if (!gpsPanel_) return;

  char buf[80];

  // 定位状态
  const char *fixText;
  lv_color_t fixColor;
  if (!enabled) {
    fixText = localized("GPS offline", "GPS 离线");
    fixColor = lv_color_make(148, 163, 184);
  } else if (fix) {
    fixText = localized("FIX ACQUIRED", "已定位");
    fixColor = lv_color_make(22, 163, 74);
  } else {
    fixText = localized("Searching satellites...", "正在搜星...");
    fixColor = lv_color_make(217, 119, 6);
  }
  lv_label_set_text(gpsFixLabel_, fixText);
  lv_obj_set_style_text_color(gpsFixLabel_, fixColor, 0);
  (void)ageMs;

  // UTC 时间
  snprintf(buf, sizeof(buf), "UTC  %s", (utc && utc[0]) ? utc : "--");
  lv_label_set_text(gpsUtcLabel_, buf);

  // 本地时间
  snprintf(buf, sizeof(buf), "%s  %s",
           localized("LOCAL", "本地"), (local && local[0]) ? local : "--");
  lv_label_set_text(gpsLocalLabel_, buf);

  // 经纬度
  if (fix) {
    snprintf(buf, sizeof(buf), "%.5f, %.5f", lat, lon);
  } else {
    snprintf(buf, sizeof(buf), "-- , --");
  }
  lv_label_set_text(gpsCoordLabel_, buf);

  // 卫星 / HDOP
  snprintf(buf, sizeof(buf), "%s %u    HDOP %.1f",
           localized("SAT", "卫星"), (unsigned)sat, (double)hdop / 100.0);
  lv_label_set_text(gpsSatLabel_, buf);

  // 海拔
  snprintf(buf, sizeof(buf), "%s  %.1f m", localized("ALT", "海拔"), (double)altM);
  lv_label_set_text(gpsAltLabel_, buf);

  // 地速
  snprintf(buf, sizeof(buf), "%s  %.1f km/h", localized("SPD", "地速"),
           (double)speedMps * 3.6);
  lv_label_set_text(gpsSpeedLabel_, buf);
}

void DisplayLcd::updateGpsIndicator(bool enabled, bool fix, uint8_t sat) {
  if (!gpsIndicatorLabel_ || !gpsIndicatorSpinner_ || !gpsSatelliteLabel_) return;

  char text[24];
  if (!enabled) {
    // GPS 关闭：灰色图标，无动画
    lv_label_set_text(gpsSatelliteLabel_, "--");
    lv_obj_clear_flag(gpsSatelliteLabel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_white(), 0);
    lv_obj_add_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);
    gpsIndicatorFixed_ = false;
  } else if (fix) {
    // 已定位：绿色图标 + 卫星数，无动画
    snprintf(text, sizeof(text), "%u", (unsigned)sat);
    lv_label_set_text(gpsSatelliteLabel_, text);
    lv_obj_clear_flag(gpsSatelliteLabel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_white(), 0);
    lv_obj_add_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);
    gpsIndicatorFixed_ = true;
  } else {
    // 搜星中：橙色图标 + 旋转动画，无卫星数
    lv_obj_add_flag(gpsSatelliteLabel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_white(), 0);
    lv_obj_clear_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);
    gpsIndicatorFixed_ = false;
  }
}

void DisplayLcd::buildGpsPanel() {
  gpsPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(gpsPanel_, 240, 320);
  lv_obj_center(gpsPanel_);
  lv_obj_clear_flag(gpsPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(gpsPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(gpsPanel_, 0, 0);
  lv_obj_set_style_radius(gpsPanel_, 0, 0);
  lv_obj_set_style_border_width(gpsPanel_, 0, 0);
  lv_obj_set_style_bg_color(gpsPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(gpsPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(gpsPanel_, "GPS", onGpsBack);
  gpsTitle_ = lv_obj_get_child(gpsPanel_, 1);

  // 定位状态（大字）
  gpsFixLabel_ = lv_label_create(gpsPanel_);
  lv_label_set_text(gpsFixLabel_, "Searching satellites...");
  lv_obj_set_width(gpsFixLabel_, 216);
  lv_obj_set_style_text_font(gpsFixLabel_, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(gpsFixLabel_, lv_color_make(217, 119, 6), 0);
  lv_obj_align(gpsFixLabel_, LV_ALIGN_TOP_LEFT, 12, 56);

  // 信息卡片容器
  lv_obj_t *card = lv_obj_create(gpsPanel_);
  lv_obj_set_size(card, 216, 138);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, 12, 82);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_bg_color(card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_make(226, 232, 240), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 5, 0);

  const char *initTexts[] = {"UTC  --", "LOCAL  --", "-- , --",
                             "SAT 0    HDOP --", "ALT  -- m", "SPD  -- km/h"};
  lv_obj_t **labels[] = {&gpsUtcLabel_, &gpsLocalLabel_, &gpsCoordLabel_,
                         &gpsSatLabel_, &gpsAltLabel_, &gpsSpeedLabel_};
  for (size_t i = 0; i < 6; ++i) {
    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, initTexts[i]);
    lv_obj_set_width(lbl, 196);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl, &lv_font_jetbrains_mono_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_make(30, 41, 59), 0);
    *labels[i] = lbl;
  }

  // 手动重启按钮
  gpsRestartButton_ = lv_btn_create(gpsPanel_);
  lv_obj_set_size(gpsRestartButton_, 216, 42);
  lv_obj_set_style_radius(gpsRestartButton_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(gpsRestartButton_, lv_color_make(37, 99, 235), 0);
  lv_obj_align(gpsRestartButton_, LV_ALIGN_TOP_LEFT, 12, 236);
  lv_obj_add_event_cb(gpsRestartButton_, onGpsRestartClicked, LV_EVENT_CLICKED, nullptr);
  gpsRestartLabel_ = lv_label_create(gpsRestartButton_);
  lv_label_set_text(gpsRestartLabel_, LV_SYMBOL_REFRESH " RESTART GPS");
  lv_obj_set_style_text_color(gpsRestartLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_font(gpsRestartLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(gpsRestartLabel_);

  lv_obj_add_flag(gpsPanel_, LV_OBJ_FLAG_HIDDEN);

  buildNoticePanel();
}

void DisplayLcd::buildNoticePanel() {
  noticePanel_ = lv_obj_create(screen_);
  lv_obj_set_size(noticePanel_, 200, 140);
  lv_obj_center(noticePanel_);
  lv_obj_clear_flag(noticePanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(noticePanel_, 0, 0);
  lv_obj_set_style_radius(noticePanel_, 10, 0);
  lv_obj_set_style_bg_color(noticePanel_, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(noticePanel_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(noticePanel_, lv_color_make(203, 213, 225), 0);
  lv_obj_set_style_border_width(noticePanel_, 1, 0);

  noticeLabel_ = lv_label_create(noticePanel_);
  lv_label_set_text(noticeLabel_, "Notice");
  lv_obj_set_width(noticeLabel_, 176);
  lv_obj_set_height(noticeLabel_, 78);
  lv_label_set_long_mode(noticeLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(noticeLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(noticeLabel_, &lv_font_noto_sans_sc_14, 0);
  lv_obj_set_style_text_color(noticeLabel_, lv_color_make(30, 41, 59), 0);
  lv_obj_align(noticeLabel_, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t *okButton = lv_btn_create(noticePanel_);
  lv_obj_set_size(okButton, 120, 32);
  lv_obj_set_style_radius(okButton, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(okButton, lv_color_make(37, 99, 235), 0);
  lv_obj_align(okButton, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_add_event_cb(okButton, onNoticeDismiss, LV_EVENT_CLICKED, nullptr);
  noticeBackLabel_ = lv_label_create(okButton);
  lv_label_set_text(noticeBackLabel_, localized("OK", "确定"));
  lv_obj_set_style_text_color(noticeBackLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_font(noticeBackLabel_, &lv_font_noto_sans_sc_14, 0);
  lv_obj_center(noticeBackLabel_);

  lv_obj_add_flag(noticePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::setSdFiles(const SdFileEntry *files, size_t count,
                            const char *status) {
  sdFileCount_ = files ? min(count, (size_t)12) : 0;
  snprintf(sdListStatus_, sizeof(sdListStatus_), "%s", status ? status : "");
  if (files)
    for (size_t i = 0; i < sdFileCount_; ++i) sdFiles_[i] = files[i];
  if (sdCurrentPathLabel_)
    lv_label_set_text(sdCurrentPathLabel_, sdCurrentDirectory_);
  if (sdUpButton_) {
    if (strcmp(sdCurrentDirectory_, "/") == 0)
      lv_obj_add_flag(sdUpButton_, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(sdUpButton_, LV_OBJ_FLAG_HIDDEN);
  }
  refreshSdList();
}

void DisplayLcd::updateAudioStatus(bool playing, const char *title) {
  audioPlaying_ = playing;
  snprintf(audioTitle_, sizeof(audioTitle_), "%s", title ? title : "");
  if (sdNowPlayingLabel_) {
    char text[128];
    if (playing && audioTitle_[0])
      snprintf(text, sizeof(text), localized("Playing: %s", "播放中: %s"),
               audioTitle_);
    else
      snprintf(text, sizeof(text), "%s",
               localized("Stopped", "已停止"));
    lv_label_set_text(sdNowPlayingLabel_, text);
  }
  if (sdStopButton_)
    lv_obj_set_style_bg_color(sdStopButton_,
                              playing ? lv_color_make(220, 38, 38)
                                      : lv_color_make(148, 163, 184), 0);
}

void DisplayLcd::refreshSdList() {
  if (!sdList_) return;

  char status[64];
  if (sdListStatus_[0])
    snprintf(status, sizeof(status), "%s", sdListStatus_);
  else if (sdFileCount_ == 0)
    snprintf(status, sizeof(status), "%s",
             localized("No files on SD card", "SD 卡内没有文件"));
  else
    snprintf(status, sizeof(status), localized("%u file(s)", "%u 个文件"),
             (unsigned)sdFileCount_);
  lv_label_set_text(sdStatusLabel_, status);

  for (size_t i = 0; i < 12; ++i) {
    lv_obj_t *button = sdRowButtons_[i];
    lv_obj_t *label = sdRowLabels_[i];
    if (i >= sdFileCount_) {
      lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    char text[288];
    const SdFileEntry &f = sdFiles_[i];
    if (f.isDirectory)
      snprintf(text, sizeof(text), "DIR  %s", f.name);
    else if (f.isAudio)
      snprintf(text, sizeof(text), LV_SYMBOL_PLAY " %s\n%lu KB",
               f.name, (unsigned long)(f.size / 1024));
    else
      snprintf(text, sizeof(text), LV_SYMBOL_FILE " %s\n%lu KB",
               f.name, (unsigned long)(f.size / 1024));
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label,
        chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_jetbrains_mono_14, 0);
    // 可播放项高亮背景
    lv_obj_set_style_bg_color(button,
      f.isDirectory ? lv_color_make(220, 252, 231)
              : (f.isAudio ? lv_color_make(219, 234, 254)
                     : lv_color_make(241, 245, 249)), 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
  }
}

void DisplayLcd::buildPanelHeader(lv_obj_t *panel, const char *title,
                                  lv_event_cb_t backCallback) {
  lv_obj_t *backButton = lv_btn_create(panel);
  lv_obj_set_size(backButton, 64, 30);
  lv_obj_set_style_radius(backButton, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(backButton, lv_color_make(226, 232, 240), 0);
  lv_obj_align(backButton, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_add_event_cb(backButton, onBackNavigation, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(backButton, backCallback, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(backButton);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " BACK");
  lv_obj_set_style_text_font(backLabel, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(backLabel, lv_color_make(51, 65, 85), 0);
  lv_obj_center(backLabel);

  lv_obj_t *titleLabel = lv_label_create(panel);
  lv_label_set_text(titleLabel, title);
  lv_obj_set_width(titleLabel, 142);
  lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(titleLabel, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(titleLabel, lv_color_make(37, 99, 235), 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, 84, 16);

  lv_obj_t *divider = lv_obj_create(panel);
  lv_obj_set_size(divider, 216, 1);
  lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 12, 48);
  lv_obj_set_style_bg_color(divider, lv_color_make(203, 213, 225), 0);
  lv_obj_set_style_border_width(divider, 0, 0);
}

void DisplayLcd::buildPowerSourcePanel() {
  powerSourcePanel_ = lv_obj_create(screen_);
  lv_obj_set_size(powerSourcePanel_, 240, 320);
  lv_obj_center(powerSourcePanel_);
  lv_obj_clear_flag(powerSourcePanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(powerSourcePanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(powerSourcePanel_, 0, 0);
  lv_obj_set_style_radius(powerSourcePanel_, 0, 0);
  lv_obj_set_style_border_width(powerSourcePanel_, 0, 0);
  lv_obj_set_style_bg_color(powerSourcePanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(powerSourcePanel_, LV_OPA_COVER, 0);
  buildPanelHeader(powerSourcePanel_, "POWER SOURCE", onCancelSelection);
  powerSourceTitle_ = lv_obj_get_child(powerSourcePanel_, 1);

  powerSourceHint_ = lv_label_create(powerSourcePanel_);
  lv_label_set_text(powerSourceHint_, "Choose a power source");
  lv_obj_set_width(powerSourceHint_, 216);
  lv_obj_set_style_text_font(powerSourceHint_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(powerSourceHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(powerSourceHint_, LV_ALIGN_TOP_LEFT, 12, 60);

  const char *labels[] = {"SCAN BLE DEVICES",
                          "VIRTUAL POWER METER", "AUTO SELECT"};
  lv_event_cb_t callbacks[] = {onBleSelected,
                               onVirtualSelected, onAutoSelected};
  const lv_color_t colors[] = {
    lv_color_make(219, 234, 254), lv_color_make(239, 244, 248),
    lv_color_make(239, 244, 248)
  };
  const lv_coord_t heights[] = {38, 38, 38};
  lv_coord_t y = 98;
  for (size_t i = 0; i < 3; ++i) {
    lv_obj_t *button = lv_btn_create(powerSourcePanel_);
    lv_obj_set_size(button, 208, heights[i]);
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 16, y);
    lv_obj_set_style_bg_color(button, colors[i], 0);
    lv_obj_set_style_border_color(button, lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_add_event_cb(button, callbacks[i], LV_EVENT_CLICKED, nullptr);
    powerSourceOptions_[i] = lv_label_create(button);
    lv_label_set_text(powerSourceOptions_[i], labels[i]);
    lv_obj_set_style_text_font(powerSourceOptions_[i], &lv_font_jetbrains_mono_14, 0);
    lv_obj_set_style_text_color(powerSourceOptions_[i], lv_color_make(30, 41, 59), 0);
    lv_obj_center(powerSourceOptions_[i]);
    y += heights[i] + 6;
  }
  buildBleFavoriteButton();
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);

  buildVirtualPowerPanel();

  bleDevicePanel_ = lv_obj_create(screen_);
  lv_obj_set_size(bleDevicePanel_, 240, 320);
  lv_obj_center(bleDevicePanel_);
  lv_obj_clear_flag(bleDevicePanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(bleDevicePanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(bleDevicePanel_, 0, 0);
  lv_obj_set_style_radius(bleDevicePanel_, 0, 0);
  lv_obj_set_style_border_width(bleDevicePanel_, 0, 0);
  lv_obj_set_style_bg_color(bleDevicePanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(bleDevicePanel_, LV_OPA_COVER, 0);

  buildPanelHeader(bleDevicePanel_, "BLE DEVICES", onBleBack);
  bleDeviceTitle_ = lv_obj_get_child(bleDevicePanel_, 1);

  bleDeviceHint_ = lv_label_create(bleDevicePanel_);
  lv_label_set_text(bleDeviceHint_, "Tap device, then confirm");
  lv_obj_set_width(bleDeviceHint_, 216);
  lv_obj_set_style_text_font(bleDeviceHint_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(bleDeviceHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(bleDeviceHint_, LV_ALIGN_TOP_LEFT, 12, 56);

  lv_obj_t *scanButton = lv_btn_create(bleDevicePanel_);
  lv_obj_set_size(scanButton, 216, 32);
  lv_obj_set_style_radius(scanButton, 0, LV_PART_MAIN);
  lv_obj_align(scanButton, LV_ALIGN_TOP_LEFT, 12, 80);
  lv_obj_set_style_bg_color(scanButton, lv_color_make(37, 99, 235), 0);
  lv_obj_add_event_cb(scanButton, onBleScanAgain, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *scanLabel = lv_label_create(scanButton);
  lv_label_set_text(scanLabel, "SCAN AGAIN");
  bleScanLabel_ = scanLabel;
  lv_obj_set_style_text_color(scanLabel, lv_color_white(), 0);
  lv_obj_set_style_text_font(scanLabel, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(scanLabel);

  bleStatusLabel_ = lv_label_create(bleDevicePanel_);
  lv_label_set_text(bleStatusLabel_, "No scan yet");
  lv_obj_set_width(bleStatusLabel_, 216);
  lv_label_set_long_mode(bleStatusLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(bleStatusLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(bleStatusLabel_, lv_color_make(37, 99, 235), 0);
  lv_obj_align(bleStatusLabel_, LV_ALIGN_TOP_LEFT, 12, 116);

  bleDeviceList_ = lv_obj_create(bleDevicePanel_);
  lv_obj_set_size(bleDeviceList_, 224, 180);
  lv_obj_align(bleDeviceList_, LV_ALIGN_TOP_LEFT, 8, 136);
  lv_obj_set_style_pad_all(bleDeviceList_, 0, 0);
  lv_obj_set_style_border_width(bleDeviceList_, 0, 0);
  lv_obj_set_style_bg_opa(bleDeviceList_, LV_OPA_TRANSP, 0);
  lv_obj_set_scroll_dir(bleDeviceList_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(bleDeviceList_, LV_SCROLLBAR_MODE_AUTO);
  for (size_t i = 0; i < kBleDeviceCapacity; ++i) {
    bleDeviceButtons_[i] = lv_btn_create(bleDeviceList_);
    lv_obj_set_size(bleDeviceButtons_[i], 212, 44);
    lv_obj_set_style_radius(bleDeviceButtons_[i], 0, LV_PART_MAIN);
    lv_obj_align(bleDeviceButtons_[i], LV_ALIGN_TOP_LEFT, 4,
           (lv_coord_t)(i * 48));
    lv_obj_set_style_bg_color(bleDeviceButtons_[i], lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_pad_all(bleDeviceButtons_[i], 1, 0);
    lv_obj_set_style_border_color(bleDeviceButtons_[i], lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(bleDeviceButtons_[i], 1, 0);
    lv_obj_add_event_cb(bleDeviceButtons_[i], onBleDeviceSelected,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_t *deviceLabel = lv_label_create(bleDeviceButtons_[i]);
    lv_obj_set_width(deviceLabel, 196);
    lv_obj_set_height(deviceLabel, 38);
    lv_label_set_long_mode(deviceLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(deviceLabel, &lv_font_jetbrains_mono_14, 0);
    lv_obj_set_style_text_color(deviceLabel, lv_color_make(30, 41, 59), 0);
    lv_obj_center(deviceLabel);
    lv_obj_add_flag(bleDeviceButtons_[i], LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);

  bleConfirmPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(bleConfirmPanel_, 240, 320);
  lv_obj_center(bleConfirmPanel_);
  lv_obj_clear_flag(bleConfirmPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(bleConfirmPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(bleConfirmPanel_, 0, 0);
  lv_obj_set_style_radius(bleConfirmPanel_, 0, 0);
  lv_obj_set_style_border_width(bleConfirmPanel_, 0, 0);
  lv_obj_set_style_bg_color(bleConfirmPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(bleConfirmPanel_, LV_OPA_COVER, 0);

  buildPanelHeader(bleConfirmPanel_, "CONFIRM DEVICE", onBleConfirmBack);
  bleConfirmTitle_ = lv_obj_get_child(bleConfirmPanel_, 1);

  bleConfirmNameLabel_ = lv_label_create(bleConfirmPanel_);
  lv_label_set_text(bleConfirmNameLabel_, "BLE device");
  lv_obj_set_width(bleConfirmNameLabel_, 216);
  lv_label_set_long_mode(bleConfirmNameLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(bleConfirmNameLabel_, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(bleConfirmNameLabel_, lv_color_make(23, 32, 51), 0);
  lv_obj_align(bleConfirmNameLabel_, LV_ALIGN_TOP_LEFT, 12, 76);

  bleConfirmAddressLabel_ = lv_label_create(bleConfirmPanel_);
  lv_label_set_text(bleConfirmAddressLabel_, "");
  lv_obj_set_width(bleConfirmAddressLabel_, 216);
  lv_obj_set_height(bleConfirmAddressLabel_, 44);
  lv_label_set_long_mode(bleConfirmAddressLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(bleConfirmAddressLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(bleConfirmAddressLabel_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(bleConfirmAddressLabel_, LV_ALIGN_TOP_LEFT, 12, 104);

  lv_obj_t *confirmButton = lv_btn_create(bleConfirmPanel_);
  lv_obj_set_size(confirmButton, 216, 42);
  lv_obj_set_style_radius(confirmButton, 0, LV_PART_MAIN);
  lv_obj_align(confirmButton, LV_ALIGN_TOP_LEFT, 12, 190);
  lv_obj_set_style_bg_color(confirmButton, lv_color_make(37, 99, 235), 0);
  lv_obj_add_event_cb(confirmButton, onBleConfirm, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *confirmLabel = lv_label_create(confirmButton);
  lv_label_set_text(confirmLabel, "CONNECT");
  bleConfirmButtonLabel_ = confirmLabel;
  lv_obj_set_style_text_color(confirmLabel, lv_color_white(), 0);
  lv_obj_set_style_text_font(confirmLabel, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(confirmLabel);

  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);

  bleLoadingPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(bleLoadingPanel_, 240, 320);
  lv_obj_center(bleLoadingPanel_);
  lv_obj_clear_flag(bleLoadingPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(bleLoadingPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(bleLoadingPanel_, 0, 0);
  lv_obj_set_style_radius(bleLoadingPanel_, 0, 0);
  lv_obj_set_style_border_width(bleLoadingPanel_, 0, 0);
  lv_obj_set_style_bg_color(bleLoadingPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(bleLoadingPanel_, LV_OPA_COVER, 0);

  buildPanelHeader(bleLoadingPanel_, "LOADING", onBleLoadingBack);
  bleLoadingBackButton_ = lv_obj_get_child(bleLoadingPanel_, 0);
  bleLoadingBackLabel_ = lv_obj_get_child(bleLoadingBackButton_, 0);

  bleLoadingTitle_ = lv_obj_get_child(bleLoadingPanel_, 1);
  bleLoadingSpinner_ = lv_spinner_create(bleLoadingPanel_, 900, 60);
  lv_obj_set_size(bleLoadingSpinner_, 42, 42);
  lv_obj_align(bleLoadingSpinner_, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_arc_color(bleLoadingSpinner_, lv_color_make(219, 227, 236),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_color(bleLoadingSpinner_, lv_color_make(37, 99, 235),
                             LV_PART_INDICATOR);

  bleLoadingLabel_ = lv_label_create(bleLoadingPanel_);
  lv_label_set_text(bleLoadingLabel_, "Waiting...");
  lv_obj_set_width(bleLoadingLabel_, 216);
  lv_obj_set_height(bleLoadingLabel_, 150);
  lv_label_set_long_mode(bleLoadingLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(bleLoadingLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(bleLoadingLabel_, lv_color_make(51, 65, 85), 0);
  lv_obj_align(bleLoadingLabel_, LV_ALIGN_TOP_LEFT, 12, 132);

  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);

  buildSettingsPanel();
}

void DisplayLcd::buildVirtualPowerPanel() {
  virtualPowerPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(virtualPowerPanel_, 240, 320);
  lv_obj_center(virtualPowerPanel_);
  lv_obj_clear_flag(virtualPowerPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(virtualPowerPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(virtualPowerPanel_, 0, 0);
  lv_obj_set_style_radius(virtualPowerPanel_, 0, 0);
  lv_obj_set_style_border_width(virtualPowerPanel_, 0, 0);
  lv_obj_set_style_bg_color(virtualPowerPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(virtualPowerPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(virtualPowerPanel_, "VIRTUAL POWER", onVirtualBack);
  virtualTitle_ = lv_obj_get_child(virtualPowerPanel_, 1);

  virtualPowerTitle_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualPowerTitle_, "POWER");
  lv_obj_set_style_text_font(virtualPowerTitle_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(virtualPowerTitle_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(virtualPowerTitle_, LV_ALIGN_TOP_LEFT, 16, 64);

  virtualPowerValueLabel_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualPowerValueLabel_, "0 W");
  lv_obj_set_style_text_font(virtualPowerValueLabel_, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(virtualPowerValueLabel_, lv_color_make(23, 32, 51), 0);
  lv_obj_align(virtualPowerValueLabel_, LV_ALIGN_TOP_RIGHT, -16, 62);

  lv_obj_t *powerSlider = lv_slider_create(virtualPowerPanel_);
  lv_obj_set_size(powerSlider, 208, 24);
  lv_slider_set_range(powerSlider, 0, 1500);
  lv_slider_set_value(powerSlider, (int)virtualPowerW_, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(powerSlider, lv_color_make(219, 227, 236), LV_PART_MAIN);
  lv_obj_set_style_bg_color(powerSlider, lv_color_make(37, 99, 235), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(powerSlider, lv_color_make(37, 99, 235), LV_PART_KNOB);
  lv_obj_align(powerSlider, LV_ALIGN_TOP_LEFT, 16, 94);
  lv_obj_add_event_cb(powerSlider, onVirtualSliderChanged, LV_EVENT_VALUE_CHANGED,
                      (void *)(uintptr_t)1);

  virtualCadenceTitle_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualCadenceTitle_, "CADENCE");
  lv_obj_set_style_text_font(virtualCadenceTitle_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(virtualCadenceTitle_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(virtualCadenceTitle_, LV_ALIGN_TOP_LEFT, 16, 146);

  virtualCadenceValueLabel_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualCadenceValueLabel_, "0 rpm");
  lv_obj_set_style_text_font(virtualCadenceValueLabel_, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(virtualCadenceValueLabel_, lv_color_make(23, 32, 51), 0);
  lv_obj_align(virtualCadenceValueLabel_, LV_ALIGN_TOP_RIGHT, -16, 144);

  lv_obj_t *cadenceSlider = lv_slider_create(virtualPowerPanel_);
  lv_obj_set_size(cadenceSlider, 208, 24);
  lv_slider_set_range(cadenceSlider, 0, 180);
  lv_slider_set_value(cadenceSlider, (int)virtualCadenceRpm_, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(cadenceSlider, lv_color_make(219, 227, 236), LV_PART_MAIN);
  lv_obj_set_style_bg_color(cadenceSlider, lv_color_make(37, 99, 235), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(cadenceSlider, lv_color_make(37, 99, 235), LV_PART_KNOB);
  lv_obj_align(cadenceSlider, LV_ALIGN_TOP_LEFT, 16, 176);
  lv_obj_add_event_cb(cadenceSlider, onVirtualSliderChanged, LV_EVENT_VALUE_CHANGED,
                      (void *)(uintptr_t)2);

  virtualHint_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualHint_, "Drag either slider to update\nlive power and cadence.");
  lv_obj_set_width(virtualHint_, 208);
  lv_label_set_long_mode(virtualHint_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(virtualHint_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(virtualHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(virtualHint_, LV_ALIGN_TOP_LEFT, 16, 224);

  lv_obj_add_flag(virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::buildSettingsPanel() {
  settingsPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(settingsPanel_, 240, 320);
  lv_obj_center(settingsPanel_);
  lv_obj_clear_flag(settingsPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(settingsPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(settingsPanel_, 0, 0);
  lv_obj_set_style_radius(settingsPanel_, 0, 0);
  lv_obj_set_style_border_width(settingsPanel_, 0, 0);
  lv_obj_set_style_bg_color(settingsPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(settingsPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(settingsPanel_, "SETTINGS", onSettingsBack);
  settingsTitle_ = lv_obj_get_child(settingsPanel_, 1);
  lv_obj_set_width(settingsTitle_, 108);
  auto *reset = lv_btn_create(settingsPanel_);
  lv_obj_set_size(reset, 32, 30);
  lv_obj_align(reset, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_event_cb(reset, onResetDashboard, LV_EVENT_LONG_PRESSED, nullptr);
  auto *resetIcon = lv_label_create(reset);
  lv_label_set_text(resetIcon, LV_SYMBOL_REFRESH);
  lv_obj_center(resetIcon);

  // 设置主页：功能入口按“一行两个并排”排布（3 行 × 2 列）。
  // 每个按钮 104×52，列起点 16 / 128，行起点 60 / 122 / 184。
  const lv_coord_t kColX[2] = {16, 128};
  const lv_coord_t kRowY[3] = {60, 122, 184};
  const lv_coord_t kBtnW = 96;
  const lv_coord_t kBtnH = 52;

  // 功率计入口
  settingsPowerButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsPowerButton_, kBtnW, kBtnH);
  lv_obj_set_style_radius(settingsPowerButton_, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsPowerButton_, lv_color_make(37, 99, 235), 0);
  lv_obj_align(settingsPowerButton_, LV_ALIGN_TOP_LEFT, kColX[0], kRowY[0]);
  lv_obj_add_event_cb(settingsPowerButton_, onPowerSourceButton,
                      LV_EVENT_CLICKED, nullptr);
  settingsPowerButtonLabel_ = lv_label_create(settingsPowerButton_);
  lv_label_set_text(settingsPowerButtonLabel_, LV_SYMBOL_POWER "\nPOWER");
  lv_obj_set_style_text_color(settingsPowerButtonLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_align(settingsPowerButtonLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(settingsPowerButtonLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(settingsPowerButtonLabel_);

  // 语言入口
  settingsLanguageEntryButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsLanguageEntryButton_, kBtnW, kBtnH);
  lv_obj_set_style_radius(settingsLanguageEntryButton_, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsLanguageEntryButton_, lv_color_make(79, 70, 229), 0);
  lv_obj_align(settingsLanguageEntryButton_, LV_ALIGN_TOP_LEFT, kColX[1], kRowY[0]);
  lv_obj_add_event_cb(settingsLanguageEntryButton_, onLanguageButton,
                      LV_EVENT_CLICKED, nullptr);
  settingsLanguageEntryLabel_ = lv_label_create(settingsLanguageEntryButton_);
  lv_label_set_text(settingsLanguageEntryLabel_, LV_SYMBOL_LIST "\nLANG");
  lv_obj_set_style_text_color(settingsLanguageEntryLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_align(settingsLanguageEntryLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(settingsLanguageEntryLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(settingsLanguageEntryLabel_);

  // Wi-Fi 入口
  settingsWifiEntryButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsWifiEntryButton_, kBtnW, kBtnH);
  lv_obj_set_style_radius(settingsWifiEntryButton_, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsWifiEntryButton_, lv_color_make(8, 145, 178), 0);
  lv_obj_align(settingsWifiEntryButton_, LV_ALIGN_TOP_LEFT, kColX[0], kRowY[1]);
  lv_obj_add_event_cb(settingsWifiEntryButton_, onWifiButton,
                      LV_EVENT_CLICKED, nullptr);
  settingsWifiEntryLabel_ = lv_label_create(settingsWifiEntryButton_);
  lv_label_set_text(settingsWifiEntryLabel_, LV_SYMBOL_WIFI "\nWI-FI");
  lv_obj_set_style_text_color(settingsWifiEntryLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_align(settingsWifiEntryLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(settingsWifiEntryLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(settingsWifiEntryLabel_);

  // 声音入口
  settingsSoundButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsSoundButton_, kBtnW, kBtnH);
  lv_obj_set_style_radius(settingsSoundButton_, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsSoundButton_, lv_color_make(124, 58, 237), 0);
  lv_obj_align(settingsSoundButton_, LV_ALIGN_TOP_LEFT, kColX[1], kRowY[1]);
  lv_obj_add_event_cb(settingsSoundButton_, onSoundButton, LV_EVENT_CLICKED, nullptr);
  settingsSoundButtonLabel_ = lv_label_create(settingsSoundButton_);
  lv_label_set_text(settingsSoundButtonLabel_, LV_SYMBOL_AUDIO "\nSOUND");
  lv_obj_set_style_text_color(settingsSoundButtonLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_align(settingsSoundButtonLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(settingsSoundButtonLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(settingsSoundButtonLabel_);

  // SD 卡文件浏览入口（音乐播放）
  settingsSdButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsSdButton_, kBtnW, kBtnH);
  lv_obj_set_style_radius(settingsSdButton_, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsSdButton_, lv_color_make(15, 118, 110), 0);
  lv_obj_align(settingsSdButton_, LV_ALIGN_TOP_LEFT, kColX[0], kRowY[2]);
  lv_obj_add_event_cb(settingsSdButton_, onSdCardButton, LV_EVENT_CLICKED, nullptr);
  settingsSdButtonLabel_ = lv_label_create(settingsSdButton_);
  lv_label_set_text(settingsSdButtonLabel_, LV_SYMBOL_SD_CARD "\nSD/MUSIC");
  lv_obj_set_style_text_color(settingsSdButtonLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_align(settingsSdButtonLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(settingsSdButtonLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(settingsSdButtonLabel_);

  // GPS 入口
  settingsGpsButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsGpsButton_, kBtnW, kBtnH);
  lv_obj_set_style_radius(settingsGpsButton_, 8, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsGpsButton_, lv_color_make(21, 128, 61), 0);
  lv_obj_align(settingsGpsButton_, LV_ALIGN_TOP_LEFT, kColX[1], kRowY[2]);
  lv_obj_add_event_cb(settingsGpsButton_, onGpsButton, LV_EVENT_CLICKED, nullptr);
  settingsGpsButtonLabel_ = lv_label_create(settingsGpsButton_);
  lv_label_set_text(settingsGpsButtonLabel_, LV_SYMBOL_GPS "\nGPS");
  lv_obj_set_style_text_color(settingsGpsButtonLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_align(settingsGpsButtonLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(settingsGpsButtonLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(settingsGpsButtonLabel_);

  lv_obj_t *logs = lv_btn_create(settingsPanel_);
  lv_obj_set_size(logs, 96, 52);
  lv_obj_align(logs, LV_ALIGN_TOP_LEFT, 16, 246);
  lv_obj_set_style_radius(logs, 8, 0);
  lv_obj_set_style_bg_color(logs, lv_color_make(51, 65, 85), 0);
  lv_obj_add_event_cb(logs, onRideLogButton, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *logsLabel = lv_label_create(logs);
  rideLogEntryLabel_ = logsLabel;
  lv_label_set_text(logsLabel, LV_SYMBOL_LIST "\nLOG");
  lv_obj_set_style_text_color(logsLabel, lv_color_white(), 0);
  lv_obj_center(logsLabel);

  lv_obj_t *usb = lv_btn_create(settingsPanel_);
  lv_obj_set_size(usb, 96, 52);
  lv_obj_align(usb, LV_ALIGN_TOP_LEFT, 128, 246);
  lv_obj_set_style_radius(usb, 8, 0);
  lv_obj_set_style_bg_color(usb, lv_color_make(15, 118, 110), 0);
  lv_obj_add_event_cb(usb, onUsbStorageButton, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *usbLabel = lv_label_create(usb);
  lv_label_set_text(usbLabel, LV_SYMBOL_USB "\nUSB");
  lv_obj_set_style_text_font(usbLabel, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(usbLabel, lv_color_white(), 0);
  lv_obj_center(usbLabel);

  rideLogPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(rideLogPanel_, 240, 320);
  lv_obj_center(rideLogPanel_);
  lv_obj_set_style_pad_all(rideLogPanel_, 0, 0);
  lv_obj_set_style_radius(rideLogPanel_, 0, 0);
  lv_obj_set_style_border_width(rideLogPanel_, 0, 0);
  lv_obj_clear_flag(rideLogPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(rideLogPanel_, lv_color_make(248, 250, 252), 0);
  buildPanelHeader(rideLogPanel_, "LOG", onRideLogBack);
  rideLogTitle_ = lv_obj_get_child(rideLogPanel_, 1);
  lv_obj_set_width(lv_obj_get_child(rideLogPanel_, 1), 50);
  fitSaveButton_ = lv_btn_create(rideLogPanel_);
  lv_obj_set_size(fitSaveButton_, 78, 30);
  lv_obj_set_style_pad_all(fitSaveButton_, 4, 0);
  lv_obj_align(fitSaveButton_, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_add_event_cb(fitSaveButton_, onFitSave, LV_EVENT_CLICKED, nullptr);
  fitSaveLabel_ = lv_label_create(fitSaveButton_);
  lv_label_set_text(fitSaveLabel_, "SAVE FIT");
  lv_obj_set_style_text_font(fitSaveLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(fitSaveLabel_);
  // 开始/停止记录按钮（本地入口，无需依赖网页）。
  recordButton_ = lv_btn_create(rideLogPanel_);
  lv_obj_set_size(recordButton_, 108, 34);
  lv_obj_set_style_pad_all(recordButton_, 4, 0);
  lv_obj_set_style_radius(recordButton_, 6, 0);
  lv_obj_set_style_bg_color(recordButton_, lv_color_make(22, 163, 74), 0);
  lv_obj_align(recordButton_, LV_ALIGN_TOP_LEFT, 8, 50);
  lv_obj_add_event_cb(recordButton_, onRecordToggle, LV_EVENT_CLICKED, (void *)1);
  recordLabel_ = lv_label_create(recordButton_);
  lv_label_set_text(recordLabel_, LV_SYMBOL_PLAY " START");
  lv_obj_set_style_text_color(recordLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_font(recordLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(recordLabel_);
  stopRecordButton_ = lv_btn_create(rideLogPanel_);
  lv_obj_set_size(stopRecordButton_, 108, 34);
  lv_obj_align(stopRecordButton_, LV_ALIGN_TOP_LEFT, 124, 50);
  lv_obj_set_style_pad_all(stopRecordButton_, 4, 0);
  lv_obj_set_style_bg_color(stopRecordButton_, lv_color_make(220, 38, 38), 0);
  lv_obj_add_event_cb(stopRecordButton_, onRecordToggle, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *stopLabel = lv_label_create(stopRecordButton_);
  stopRecordLabel_ = stopLabel;
  lv_label_set_text(stopLabel, LV_SYMBOL_STOP " STOP");
  lv_obj_set_style_text_font(stopLabel, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(stopLabel);
  lv_obj_add_state(stopRecordButton_, LV_STATE_DISABLED);
  rideDurationLabel_ = lv_label_create(rideLogPanel_);
  lv_obj_align(rideDurationLabel_, LV_ALIGN_TOP_LEFT, 12, 90);
  lv_obj_set_style_text_font(rideDurationLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_t *scroll = lv_obj_create(rideLogPanel_);
  lv_obj_set_size(scroll, 224, 192);
  lv_obj_align(scroll, LV_ALIGN_TOP_LEFT, 8, 112);
  lv_obj_set_style_pad_all(scroll, 6, 0);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  rideLogText_ = lv_label_create(scroll);
  lv_obj_set_width(rideLogText_, 204);
  lv_label_set_long_mode(rideLogText_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(rideLogText_, &lv_font_jetbrains_mono_14, 0);
  updateRideLog("", false, 0, 0, 0, 0, 0);
  lv_obj_add_flag(rideLogPanel_, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(settingsPanel_, LV_OBJ_FLAG_HIDDEN);

  buildLanguagePanel();
  buildWifiPanel();
  buildSoundPanel();
  buildSdPanel();
  buildGpsPanel();
}

void DisplayLcd::buildLanguagePanel() {
  languagePanel_ = lv_obj_create(screen_);
  lv_obj_set_size(languagePanel_, 240, 320);
  lv_obj_center(languagePanel_);
  lv_obj_clear_flag(languagePanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(languagePanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(languagePanel_, 0, 0);
  lv_obj_set_style_radius(languagePanel_, 0, 0);
  lv_obj_set_style_border_width(languagePanel_, 0, 0);
  lv_obj_set_style_bg_color(languagePanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(languagePanel_, LV_OPA_COVER, 0);
  buildPanelHeader(languagePanel_, "LANGUAGE", onLanguageBack);
  languageTitle_ = lv_obj_get_child(languagePanel_, 1);

  languageHint_ = lv_label_create(languagePanel_);
  lv_label_set_text(languageHint_, "Choose interface language");
  lv_obj_set_width(languageHint_, 216);
  lv_obj_set_style_text_font(languageHint_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(languageHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(languageHint_, LV_ALIGN_TOP_LEFT, 12, 58);

  const char *labels[] = {"English", "简体中文"};
  for (size_t i = 0; i < 2; ++i) {
    settingsLanguageButtons_[i] = lv_btn_create(languagePanel_);
    lv_obj_set_size(settingsLanguageButtons_[i], 102, 46);
    lv_obj_set_style_radius(settingsLanguageButtons_[i], 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(settingsLanguageButtons_[i],
                              lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_border_color(settingsLanguageButtons_[i],
                                  lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(settingsLanguageButtons_[i], 1, 0);
    lv_obj_align(settingsLanguageButtons_[i], LV_ALIGN_TOP_LEFT,
                 (lv_coord_t)(16 + i * 106), 92);
    lv_obj_add_event_cb(settingsLanguageButtons_[i], onLanguageSelected,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)(i == 1));
    settingsLanguageButtonLabels_[i] = lv_label_create(settingsLanguageButtons_[i]);
    lv_label_set_text(settingsLanguageButtonLabels_[i], labels[i]);
    lv_obj_set_style_text_font(settingsLanguageButtonLabels_[i],
                               i == 1 ? &lv_font_noto_sans_sc_14 : &lv_font_jetbrains_mono_14, 0);
    lv_obj_center(settingsLanguageButtonLabels_[i]);
  }

  lv_obj_add_flag(languagePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::buildWifiPanel() {
  wifiPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(wifiPanel_, 240, 320);
  lv_obj_center(wifiPanel_);
  lv_obj_clear_flag(wifiPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(wifiPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(wifiPanel_, 0, 0);
  lv_obj_set_style_radius(wifiPanel_, 0, 0);
  lv_obj_set_style_border_width(wifiPanel_, 0, 0);
  lv_obj_set_style_bg_color(wifiPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(wifiPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(wifiPanel_, "WI-FI", onWifiBack);
  wifiTitle_ = lv_obj_get_child(wifiPanel_, 1);

  settingsWifiLabel_ = lv_label_create(wifiPanel_);
  lv_label_set_text(settingsWifiLabel_, "Wi-Fi");
  lv_obj_set_style_text_font(settingsWifiLabel_, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(settingsWifiLabel_, lv_color_make(30, 41, 59), 0);
  lv_obj_align(settingsWifiLabel_, LV_ALIGN_TOP_LEFT, 16, 62);

  for (size_t i = 0; i < 2; ++i) {
    settingsWifiButtons_[i] = lv_btn_create(wifiPanel_);
    lv_obj_set_size(settingsWifiButtons_[i], 102, 46);
    lv_obj_set_style_radius(settingsWifiButtons_[i], 8, LV_PART_MAIN);
    lv_obj_set_style_border_color(settingsWifiButtons_[i],
                                  lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(settingsWifiButtons_[i], 1, 0);
    lv_obj_align(settingsWifiButtons_[i], LV_ALIGN_TOP_LEFT,
                 (lv_coord_t)(16 + i * 106), 92);
    lv_obj_add_event_cb(settingsWifiButtons_[i], onWifiSelected,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    settingsWifiButtonLabels_[i] = lv_label_create(settingsWifiButtons_[i]);
    lv_label_set_text(settingsWifiButtonLabels_[i], i == 1 ? "ON" : "OFF");
    lv_obj_set_style_text_font(settingsWifiButtonLabels_[i],
                               &lv_font_jetbrains_mono_14, 0);
    lv_obj_center(settingsWifiButtonLabels_[i]);
  }

  settingsWifiStatus_ = lv_label_create(wifiPanel_);
  lv_obj_set_width(settingsWifiStatus_, 208);
  lv_label_set_long_mode(settingsWifiStatus_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(settingsWifiStatus_, lv_color_make(71, 85, 105), 0);
  lv_obj_align(settingsWifiStatus_, LV_ALIGN_TOP_LEFT, 16, 156);

  refreshWifiStatus();
  lv_obj_t *setup = lv_btn_create(wifiPanel_);
  lv_obj_set_size(setup, 208, 36);
  lv_obj_align(setup, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_add_event_cb(setup, onWifiSetup, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *setupLabel = lv_label_create(setup);
  wifiSetupLabel_ = setupLabel;
  lv_label_set_text(setupLabel, "SET UP WI-FI");
  lv_obj_set_style_text_font(setupLabel, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(setupLabel);
  lv_obj_add_flag(wifiPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::buildSoundPanel() {
  soundPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(soundPanel_, 240, 320);
  lv_obj_center(soundPanel_);
  lv_obj_clear_flag(soundPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(soundPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(soundPanel_, 0, 0);
  lv_obj_set_style_radius(soundPanel_, 0, 0);
  lv_obj_set_style_border_width(soundPanel_, 0, 0);
  lv_obj_set_style_bg_color(soundPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(soundPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(soundPanel_, "SOUND", onSoundBack);
  soundTitle_ = lv_obj_get_child(soundPanel_, 1);

  soundHint_ = lv_label_create(soundPanel_);
  lv_label_set_text(soundHint_, "Drag the slider to set volume.\nPreview tones are disabled.");
  lv_obj_set_width(soundHint_, 216);
  lv_label_set_long_mode(soundHint_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(soundHint_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(soundHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(soundHint_, LV_ALIGN_TOP_LEFT, 12, 58);

  soundVolumeLabel_ = lv_label_create(soundPanel_);
  lv_label_set_text(soundVolumeLabel_, "Volume: 70%");
  lv_obj_set_style_text_font(soundVolumeLabel_, &lv_font_jetbrains_mono_16, 0);
  lv_obj_set_style_text_color(soundVolumeLabel_, lv_color_make(23, 32, 51), 0);
  lv_obj_align(soundVolumeLabel_, LV_ALIGN_TOP_LEFT, 12, 112);

  soundVolumeSlider_ = lv_slider_create(soundPanel_);
  lv_obj_set_size(soundVolumeSlider_, 208, 24);
  lv_slider_set_range(soundVolumeSlider_, 0, 100);
  lv_slider_set_value(soundVolumeSlider_, currentVolume_, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(soundVolumeSlider_, lv_color_make(219, 227, 236), LV_PART_MAIN);
  lv_obj_set_style_bg_color(soundVolumeSlider_, lv_color_make(37, 99, 235), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(soundVolumeSlider_, lv_color_make(37, 99, 235), LV_PART_KNOB);
  lv_obj_align(soundVolumeSlider_, LV_ALIGN_TOP_LEFT, 16, 148);
  lv_obj_add_event_cb(soundVolumeSlider_, onVolumeSliderChanged,
                      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(soundVolumeSlider_, onVolumeSliderChanged,
                      LV_EVENT_RELEASED, nullptr);

  refreshSoundVolume();
  lv_obj_add_flag(soundPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::buildSdPanel() {
  sdPanel_ = lv_obj_create(screen_);
  lv_obj_set_size(sdPanel_, 240, 320);
  lv_obj_center(sdPanel_);
  lv_obj_clear_flag(sdPanel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(sdPanel_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(sdPanel_, 0, 0);
  lv_obj_set_style_radius(sdPanel_, 0, 0);
  lv_obj_set_style_border_width(sdPanel_, 0, 0);
  lv_obj_set_style_bg_color(sdPanel_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(sdPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(sdPanel_, "SD CARD", onSdBack);
  sdTitle_ = lv_obj_get_child(sdPanel_, 1);

  sdCurrentPathLabel_ = lv_label_create(sdPanel_);
  lv_label_set_text(sdCurrentPathLabel_, "/");
  lv_obj_set_width(sdCurrentPathLabel_, 154);
  lv_label_set_long_mode(sdCurrentPathLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(sdCurrentPathLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_align(sdCurrentPathLabel_, LV_ALIGN_TOP_LEFT, 12, 52);

  sdUpButton_ = lv_btn_create(sdPanel_);
  lv_obj_set_size(sdUpButton_, 52, 24);
  lv_obj_set_style_radius(sdUpButton_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sdUpButton_, lv_color_make(226, 232, 240), 0);
  lv_obj_align(sdUpButton_, LV_ALIGN_TOP_RIGHT, -12, 49);
  lv_obj_add_event_cb(sdUpButton_, onSdUpClicked, LV_EVENT_CLICKED, nullptr);
  sdUpLabel_ = lv_label_create(sdUpButton_);
  lv_label_set_text(sdUpLabel_, localized("UP", "返回"));
  lv_obj_set_style_text_font(sdUpLabel_, &lv_font_noto_sans_sc_14, 0);
  lv_obj_center(sdUpLabel_);
  lv_obj_add_flag(sdUpButton_, LV_OBJ_FLAG_HIDDEN);

  sdStatusLabel_ = lv_label_create(sdPanel_);
  lv_label_set_text(sdStatusLabel_, "Loading files...");
  lv_obj_set_width(sdStatusLabel_, 216);
  lv_obj_set_style_text_font(sdStatusLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(sdStatusLabel_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(sdStatusLabel_, LV_ALIGN_TOP_LEFT, 12, 74);

  // 可滚动文件列表
  sdList_ = lv_obj_create(sdPanel_);
  lv_obj_set_size(sdList_, 216, 158);
  lv_obj_align(sdList_, LV_ALIGN_TOP_LEFT, 12, 94);
  lv_obj_set_style_pad_all(sdList_, 0, 0);
  lv_obj_set_style_radius(sdList_, 0, 0);
  lv_obj_set_style_border_width(sdList_, 0, 0);
  lv_obj_set_style_bg_opa(sdList_, LV_OPA_TRANSP, 0);
  lv_obj_set_flex_flow(sdList_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sdList_, 4, 0);

  for (size_t i = 0; i < 12; ++i) {
    lv_obj_t *button = lv_btn_create(sdList_);
    lv_obj_set_size(button, 210, 40);
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_make(241, 245, 249), 0);
    lv_obj_set_style_border_color(button, lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_pad_all(button, 2, 0);
    lv_obj_add_event_cb(button, onSdFileClicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)i);
    sdRowButtons_[i] = button;
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_width(label, 200);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_jetbrains_mono_14, 0);
    lv_obj_set_style_text_color(label, lv_color_make(30, 41, 59), 0);
    lv_obj_center(label);
    sdRowLabels_[i] = label;
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  }

  sdNowPlayingLabel_ = lv_label_create(sdPanel_);
  lv_label_set_text(sdNowPlayingLabel_, "Stopped");
  lv_obj_set_width(sdNowPlayingLabel_, 216);
  lv_label_set_long_mode(sdNowPlayingLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(sdNowPlayingLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_color(sdNowPlayingLabel_, lv_color_make(37, 99, 235), 0);
  lv_obj_align(sdNowPlayingLabel_, LV_ALIGN_TOP_LEFT, 12, 260);

  sdStopButton_ = lv_btn_create(sdPanel_);
  lv_obj_set_size(sdStopButton_, 216, 34);
  lv_obj_set_style_radius(sdStopButton_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sdStopButton_, lv_color_make(148, 163, 184), 0);
  lv_obj_align(sdStopButton_, LV_ALIGN_TOP_LEFT, 12, 282);
  lv_obj_add_event_cb(sdStopButton_, onSdStopClicked, LV_EVENT_CLICKED, nullptr);
  sdStopLabel_ = lv_label_create(sdStopButton_);
  lv_label_set_text(sdStopLabel_, LV_SYMBOL_STOP " STOP");
  lv_obj_set_style_text_color(sdStopLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_font(sdStopLabel_, &lv_font_jetbrains_mono_14, 0);
  lv_obj_center(sdStopLabel_);

  lv_obj_add_flag(sdPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::setLanguage(bool chinese) {
  if (chinese_ == chinese) return;
  chinese_ = chinese;
  Preferences preferences;
  if (!preferences.begin("aero-ui", false)) {
    AppLog.println("[UI] Could not open preferences to save language.");
  } else {
    size_t written = preferences.putBool("zh", chinese_);
    if (written == 0) {
      AppLog.println("[UI] Could not save language preference.");
    }
    preferences.end();
  }
  refreshLanguage();
}

void DisplayLcd::refreshBleDeviceButtons() {
  SavedConnections saved;
  saved.loadFavorites();
  for (size_t i = 0; i < kBleDeviceCapacity; ++i) {
    lv_obj_t *button = bleDeviceButtons_[i];
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (i >= bleDeviceCount_) {
      lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const char *profile = bleDevices_[i].heartRate ? "HRS / bpm" : bleDevices_[i].cyclingPower
                              ? (bleDevices_[i].fitnessMachine
                                     ? localized("CPS + FTMS", "功率计 + 骑行台")
                                     : localized("CPS", "功率计"))
                              : (bleDevices_[i].fitnessMachine
                                     ? localized("FTMS trainer", "FTMS 骑行台")
                                     : (bleDevices_[i].speedCadence
                                            ? localized("CSC cadence only", "CSC 仅踏频")
                                            : localized("BLE device", "蓝牙设备")));
    char text[64];
    bleDevices_[i].favorite = saved.isFavorite(bleDevices_[i].address, bleDevices_[i].addressType);
    snprintf(text, sizeof(text), "%s%s\n%s  %d dBm",
         bleDevices_[i].favorite ? "* " : "", bleDevices_[i].name, profile, bleDevices_[i].rssi);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
  }
}

void DisplayLcd::refreshLanguage() {
  AppLog.printf("[LANG] begin zh=%d\n", (int)chinese_);
  const lv_font_t *font = chinese_ ? &lv_font_noto_sans_sc_14
                                   : &lv_font_jetbrains_mono_14;
  const lv_font_t *titleFont = chinese_ ? &lv_font_noto_sans_sc_14
                                        : &lv_font_jetbrains_mono_16;
  dashboard_.setLanguage(chinese_);
  AppLog.println("[LANG] 1 main done");

  lv_label_set_text(powerSourceTitle_, localized("POWER SOURCE", "功率来源"));
  lv_label_set_text(powerSourceHint_, localized("Choose a power source", "选择功率来源"));
  const char *sourceLabels[] = {
    "SCAN BLE DEVICES",
    "VIRTUAL POWER METER", "AUTO SELECT"
  };
  const char *sourceLabelsZh[] = {
    "扫描蓝牙设备", "虚拟功率计", "自动选择"
  };
  for (size_t i = 0; i < 3; ++i) {
    lv_label_set_text(powerSourceOptions_[i],
                      localized(sourceLabels[i], sourceLabelsZh[i]));
    lv_obj_set_style_text_font(powerSourceOptions_[i], font, 0);
  }
  lv_obj_set_style_text_font(powerSourceTitle_, titleFont, 0);
  lv_obj_set_style_text_font(powerSourceHint_, font, 0);
  lv_label_set_text(bleDeviceTitle_, localized("BLE DEVICES", "蓝牙设备"));
  lv_label_set_text(bleDeviceHint_, localized("Tap device, then confirm", "点击设备后确认连接"));
  lv_label_set_text(bleScanLabel_, localized("SCAN AGAIN", "重新扫描"));
  lv_obj_set_style_text_font(bleDeviceTitle_, titleFont, 0);
  lv_obj_set_style_text_font(bleDeviceHint_, font, 0);
  lv_obj_set_style_text_font(bleScanLabel_, font, 0);
  if (!bleScanCompleted_) lv_label_set_text(bleStatusLabel_, localized("No scan yet", "尚未扫描"));
  else if (!bleDeviceCount_) lv_label_set_text(bleStatusLabel_, localized("No connectable BLE devices found", "未找到可连接的蓝牙设备"));
  else {
    char status[40];
    snprintf(status, sizeof(status), chinese_ ? "找到 %u 个设备" : "%u device(s) found", (unsigned)bleDeviceCount_);
    lv_label_set_text(bleStatusLabel_, status);
  }
  lv_obj_set_style_text_font(bleStatusLabel_, font, 0);
  refreshBleDeviceButtons();
  lv_label_set_text(bleConfirmTitle_, localized("CONFIRM DEVICE", "确认设备"));
  lv_label_set_text(bleConfirmButtonLabel_, localized("CONNECT", "连接"));
  lv_obj_set_style_text_font(bleConfirmTitle_, titleFont, 0);
  lv_obj_set_style_text_font(bleConfirmButtonLabel_, font, 0);
  lv_obj_set_style_text_font(bleConfirmAddressLabel_, font, 0);
  lv_label_set_text(bleLoadingTitle_, localized("LOADING", "正在加载"));
  lv_label_set_text(bleLoadingBackLabel_, localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
  lv_obj_set_style_text_font(bleLoadingTitle_, titleFont, 0);
  lv_obj_set_style_text_font(bleLoadingBackLabel_, font, 0);
  lv_obj_set_style_text_font(bleLoadingLabel_, font, 0);
  for (size_t index = 0; index < kBleDeviceCapacity; ++index)
    lv_obj_set_style_text_font(lv_obj_get_child(bleDeviceButtons_[index], 0), font, 0);
  lv_label_set_text(virtualTitle_, localized("VIRTUAL POWER", "虚拟功率计"));
  lv_label_set_text(virtualPowerTitle_, localized("POWER", "功率"));
  lv_label_set_text(virtualCadenceTitle_, localized("CADENCE", "踏频"));
  lv_label_set_text(virtualHint_, localized("Drag either slider to update\nlive power and cadence.", "拖动滑块设置实时功率\n和踏频。"));
  lv_obj_set_style_text_font(virtualTitle_, titleFont, 0);
  lv_obj_set_style_text_font(virtualPowerTitle_, font, 0);
  lv_obj_set_style_text_font(virtualCadenceTitle_, font, 0);
  lv_obj_set_style_text_font(virtualHint_, font, 0);
  lv_label_set_text(settingsTitle_, localized("SETTINGS", "设置"));
  lv_label_set_text(settingsPowerButtonLabel_, localized(LV_SYMBOL_POWER "\nPOWER", LV_SYMBOL_POWER "\n功率"));
  lv_label_set_text(settingsLanguageEntryLabel_, localized(LV_SYMBOL_LIST "\nLANG", LV_SYMBOL_LIST "\n语言"));
  lv_label_set_text(settingsWifiEntryLabel_, LV_SYMBOL_WIFI "\nWI-FI");
  lv_label_set_text(settingsSdButtonLabel_, localized(LV_SYMBOL_SD_CARD "\nSD/MUSIC", LV_SYMBOL_SD_CARD "\nSD/音乐"));
  lv_label_set_text(settingsSoundButtonLabel_, localized(LV_SYMBOL_AUDIO "\nSOUND", LV_SYMBOL_AUDIO "\n声音"));
  lv_label_set_text(settingsGpsButtonLabel_, LV_SYMBOL_GPS "\nGPS");
  lv_obj_set_style_text_font(settingsTitle_, titleFont, 0);
  for (auto *label : {settingsPowerButtonLabel_, settingsLanguageEntryLabel_, settingsWifiEntryLabel_, settingsSdButtonLabel_, settingsSoundButtonLabel_, settingsGpsButtonLabel_})
    lv_obj_set_style_text_font(label, font, 0);
  lv_label_set_text(languageTitle_, localized("LANGUAGE", "语言"));
  lv_label_set_text(languageHint_, localized("Choose interface language", "选择界面语言"));
  lv_obj_set_style_text_font(languageTitle_, titleFont, 0);
  lv_obj_set_style_text_font(languageHint_, font, 0);
  lv_label_set_text(wifiTitle_, "WI-FI");
  lv_obj_set_style_text_font(wifiTitle_, titleFont, 0);
  if (settingsWifiLabel_) lv_label_set_text(settingsWifiLabel_, "Wi-Fi");
  for (size_t index = 0; index < 2; ++index) {
    lv_label_set_text(settingsWifiButtonLabels_[index], index ? localized("ON", "开启") : localized("OFF", "关闭"));
    lv_obj_set_style_text_font(settingsWifiButtonLabels_[index], font, 0);
  }
  lv_label_set_text(gpsTitle_, "GPS");
  lv_label_set_text(gpsRestartLabel_, localized(LV_SYMBOL_REFRESH " RESTART GPS", LV_SYMBOL_REFRESH " 重启 GPS"));
  setRecording(recording_, recordingArmed_);
  setFitSaving(fitSaving_);
  lv_label_set_text(stopRecordLabel_, localized(LV_SYMBOL_STOP " STOP", LV_SYMBOL_STOP " 停止"));
  lv_label_set_text(rideLogTitle_, localized("LOG", "日志"));
  lv_label_set_text(rideLogEntryLabel_, localized(LV_SYMBOL_LIST "\nLOG", LV_SYMBOL_LIST "\n记录日志"));
  lv_label_set_text(wifiSetupLabel_, localized("SET UP WI-FI", "配置网络"));
  for (auto *label : {recordLabel_, stopRecordLabel_, fitSaveLabel_, rideLogTitle_, rideLogEntryLabel_, wifiSetupLabel_, rideLogText_, rideDurationLabel_, gpsRestartLabel_, gpsUtcLabel_, gpsLocalLabel_, gpsCoordLabel_, gpsSatLabel_, gpsAltLabel_, gpsSpeedLabel_})
    lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_font(gpsTitle_, titleFont, 0);
  lv_obj_set_style_text_font(gpsFixLabel_, titleFont, 0);
  refreshRideLog();
  lv_label_set_text(soundTitle_, localized("SOUND", "声音"));
  lv_label_set_text(soundHint_, localized("Drag the slider to set volume.\nPreview tones are disabled.", "拖动滑块设置音量。\n试听提示音已禁用。"));
  lv_obj_set_style_text_font(soundTitle_, titleFont, 0);
  lv_obj_set_style_text_font(soundHint_, font, 0);
  lv_obj_set_style_text_font(soundVolumeLabel_, font, 0);
  refreshSoundVolume();
  lv_label_set_text(sdTitle_, localized("SD CARD", "SD 卡"));
  lv_label_set_text(sdStopLabel_, localized(LV_SYMBOL_STOP " STOP", LV_SYMBOL_STOP " 停止"));
  lv_obj_set_style_text_font(sdTitle_, titleFont, 0);
  for (auto *label : {sdStatusLabel_, sdStopLabel_, sdNowPlayingLabel_}) lv_obj_set_style_text_font(label, font, 0);
  refreshSdList();
  char playing[128];
  if (audioPlaying_ && audioTitle_[0]) snprintf(playing, sizeof(playing), localized("Playing: %s", "播放中: %s"), audioTitle_);
  else snprintf(playing, sizeof(playing), "%s", localized("Stopped", "已停止"));
  lv_label_set_text(sdNowPlayingLabel_, playing);
  lv_obj_set_style_text_font(settingsWifiStatus_, font, 0);
  lv_obj_set_style_text_font(settingsLanguageButtonLabels_[0], &lv_font_jetbrains_mono_14, 0);
  lv_obj_set_style_text_font(settingsLanguageButtonLabels_[1], &lv_font_noto_sans_sc_14, 0);
  for (size_t i = 0; i < 2; ++i) {
    const bool selected = chinese_ == (i == 1);
    lv_obj_set_style_bg_color(settingsLanguageButtons_[i],
                              selected ? lv_color_make(219, 234, 254)
                                       : lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_text_color(settingsLanguageButtonLabels_[i],
                                selected ? lv_color_make(29, 78, 216)
                                         : lv_color_make(51, 65, 85), 0);
  }
  refreshWifiStatus();
  AppLog.println("[LANG] 10 langbtn+wifi done");

  if (noticeBackLabel_) {
    lv_label_set_text(noticeBackLabel_, localized("OK", "确定"));
    lv_obj_set_style_text_font(noticeBackLabel_, font, 0);
  }

  AppLog.println("[LANG] 11 panels begin");
  lv_obj_t *panels[] = {
    powerSourcePanel_, bleDevicePanel_, bleConfirmPanel_,
    bleLoadingPanel_, virtualPowerPanel_, settingsPanel_, soundPanel_, sdPanel_,
    gpsPanel_, languagePanel_, wifiPanel_, rideLogPanel_
  };
  for (lv_obj_t *panel : panels) {
    lv_obj_t *backButton = lv_obj_get_child(panel, 0);
    lv_obj_t *backLabel = lv_obj_get_child(backButton, 0);
    lv_label_set_text(backLabel,
                      localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
    lv_obj_set_style_text_font(backLabel, font, 0);
  }
  AppLog.println("[LANG] 12 done");
}

void DisplayLcd::begin() {
  g_display = this;

  chinese_ = false;
  {
    Preferences preferences;
    if (preferences.begin("aero-ui", true)) {
      chinese_ = preferences.getBool("zh", false);
      preferences.end();
    }
  }

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  _bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK,
                              PIN_LCD_MOSI, -1, HSPI);
  _gfx = new Arduino_ST7789(_bus, PIN_LCD_RST, 0, true);
  _gfx->begin();
  _gfx->fillScreen(BLACK);

  lv_init();

  static lv_color_t drawBuffer[240 * 320 / 10];
  static lv_disp_draw_buf_t dispBuf;
  lv_disp_draw_buf_init(&dispBuf, drawBuffer, nullptr, 240 * 320 / 10);

  static lv_disp_drv_t dispDrv;
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = 240;
  dispDrv.ver_res = 320;
  dispDrv.flush_cb = lvglFlushCb;
  dispDrv.draw_buf = &dispBuf;
  lv_disp_drv_register(&dispDrv);

  touchReady_ = initTouch();
  if (touchReady_) {
    static lv_indev_drv_t indevDrv;
    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = lvglTouchReadCb;
    lv_indev_drv_register(&indevDrv);
    AppLog.println("[TOUCH] CST328 ready");
  } else {
    AppLog.println("[TOUCH] CST328 unavailable; touch controls disabled");
  }

  screen_ = lv_obj_create(nullptr);
  lv_obj_set_size(screen_, 240, 320);
  lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(screen_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

  lv_obj_set_style_pad_all(screen_, 0, 0);
  dashboard_.create(screen_, onRideLogButton, onSettingsButton);
  gpsIndicatorSpinner_ = dashboard_.spinner;
  gpsIndicatorLabel_ = dashboard_.gps;
  gpsSatelliteLabel_ = dashboard_.satellites;
  timeLabel_ = dashboard_.clock;
  statusLabel_ = dashboard_.sdIcon;
  powerSourceButton_ = dashboard_.settingsButton;
  DashboardLayout::Settings layout;
  if (DashboardSettings::load(layout)) dashboard_.load(layout);
  buildPowerSourcePanel();
  refreshLanguage();
  lv_scr_load(screen_);
  _gfx->fillScreen(BLACK);
}

bool DisplayLcd::takePowerSourceRequest(int &mode) {
  if (requestedPowerMode_ < 0) return false;
  mode = requestedPowerMode_;
  requestedPowerMode_ = -1;
  return true;
}

bool DisplayLcd::takeBleScanRequest() {
  if (!requestedBleScan_) return false;
  requestedBleScan_ = false;
  return true;
}

bool DisplayLcd::takeBleConnectRequest(char *address, size_t capacity,
                                      uint8_t *addressType, bool *postureDevice, bool *heartDevice) {
  if (!requestedBleConnect_) return false;
  requestedBleConnect_ = false;
  if (!address || capacity == 0) return false;
  snprintf(address, capacity, "%s", requestedBleAddress_);
  if (addressType) *addressType = requestedBleAddressType_;
  if (postureDevice) *postureDevice = requestedBlePosture_;
    if (heartDevice) *heartDevice = requestedBleHeart_;
  requestedBleAddress_[0] = '\0';
  requestedBleAddressType_ = 0xFF;
  return true;
}

bool DisplayLcd::takeBleCancelRequest() {
  if (!requestedBleCancel_) return false;
  requestedBleCancel_ = false;
  // 用户明确返回：允许下一次进入 BLE 页面立即点击。
  bleScanTouch_.reset(millis());
  bleCancelTouch_.reset(millis());
  return true;
}

void DisplayLcd::showBleLoading(const char *status) {
  bleOperationActive_ = true;
  requestedBleCancel_ = false;
  // 面板刚切换，吞掉这段时间内的手指残留/穿透点击。
  bleCancelTouch_.reset(millis());
  lv_obj_clear_state(bleLoadingBackButton_, LV_STATE_DISABLED);
  lv_label_set_text(bleLoadingBackLabel_,
                    localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
  lv_label_set_text(bleLoadingTitle_, localized("LOADING", "正在搜索"));
  const char *message = status ? status : "Waiting for BLE device...";
  if (strcmp(message, "Scanning for BLE power meters and smart trainers...") == 0)
    message = localized(message, "正在搜索蓝牙功率计和智能骑行台...");
  else if (strcmp(message, "Connecting to selected BLE device...") == 0)
    message = localized(message, "正在连接所选蓝牙设备...");
  else if (strcmp(message, "Waiting for BLE device...") == 0)
    message = localized(message, "等待蓝牙设备...");
  lv_label_set_text(bleLoadingLabel_, message);
  lv_obj_clear_flag(bleLoadingSpinner_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(bleLoadingLabel_, 132);
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::finishBleCancelled() {
  bleOperationActive_ = false;
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  // 从 loading 面板切回设备列表：缓冲一段时间，防止回弹的点击立刻又选中设备。
  bleDeviceTouch_.reset(millis());
  bleScanTouch_.reset(millis());
}

void DisplayLcd::showBleConnectFailure(const char *error) {
  bleOperationActive_ = false;
  bleRequestInFlight_ = false;   // 失败后允许用户重新发起请求
  bleCancelTouch_.reset(millis());
  lv_label_set_text(bleLoadingTitle_,
                    localized("CONNECTION FAILED", "连接失败"));
  lv_label_set_text(bleLoadingLabel_,
                    error ? error : "BLE connection failed without an error description.");
  lv_obj_add_flag(bleLoadingSpinner_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(bleLoadingLabel_, 66);
  lv_obj_set_height(bleLoadingLabel_, 240);
  lv_obj_clear_state(bleLoadingBackButton_, LV_STATE_DISABLED);
  lv_label_set_text(bleLoadingBackLabel_,
                    localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::setBleDevices(const BlePowerDevice *devices, size_t count,
                               const char *status) {
  bleOperationActive_ = false;
  bleRequestInFlight_ = false;
  // 设备列表刚刷新：给触控一个缓冲窗口，避免扫描结束时手指恰好落下而误选。
  bleDeviceTouch_.reset(millis());
  bleScanTouch_.reset(millis());
  bleScanCompleted_ = true;
  bleDeviceCount_ = min(count, kBleDeviceCapacity);
  lv_obj_scroll_to_y(bleDeviceList_, 0, LV_ANIM_OFF);
  if (devices)
    for (size_t i = 0; i < bleDeviceCount_; ++i) bleDevices_[i] = devices[i];
  (void)status;
  refreshLanguage();
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onUsbStorageButton(lv_event_t *) {
  if (!g_display) return;
  g_display->showUsbStorage(false, false, false, false);
}

void DisplayLcd::onUsbStorageBack(lv_event_t *) {
  if (!g_display || g_display->usbStorageActive_) return;
  lv_obj_add_flag(g_display->usbStoragePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onUsbStorageMode(lv_event_t *event) {
  if (!g_display || !g_display->usbStorageTouch_.accept(millis())) return;
  const bool enabled = (uintptr_t)lv_event_get_user_data(event) == 1;
  if (enabled == g_display->usbStorageActive_) return;
  g_display->usbStorageError_ = false;
  g_display->usbStorageRequested_ = enabled;
  g_display->usbStorageRequestPending_ = true;
}

void DisplayLcd::showUsbStorage(bool ready, bool ejected, bool hostConnected, bool enabled) {
  if (!screen_) return;
  usbStorageActive_ = enabled;
  if (!usbStoragePanel_) {
    usbStoragePanel_ = lv_obj_create(screen_);
    lv_obj_set_size(usbStoragePanel_, 240, 320);
    lv_obj_center(usbStoragePanel_);
    lv_obj_clear_flag(usbStoragePanel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(usbStoragePanel_, 0, 0);
    lv_obj_set_style_border_width(usbStoragePanel_, 0, 0);
    lv_obj_set_style_pad_all(usbStoragePanel_, 0, 0);
    lv_obj_set_style_bg_color(usbStoragePanel_, lv_color_make(248, 250, 252), 0);
    buildPanelHeader(usbStoragePanel_, "USB STORAGE", onUsbStorageBack);
    usbStorageStatus_ = lv_label_create(usbStoragePanel_);
    lv_obj_set_width(usbStorageStatus_, 216);
    lv_obj_set_style_text_font(usbStorageStatus_, &lv_font_jetbrains_mono_14, 0);
    lv_obj_set_style_text_color(usbStorageStatus_, lv_color_make(30, 41, 59), 0);
    lv_obj_align(usbStorageStatus_, LV_ALIGN_TOP_LEFT, 12, 76);
    for (unsigned index = 0; index < 2; ++index) {
      usbStorageButtons_[index] = lv_btn_create(usbStoragePanel_);
      lv_obj_set_size(usbStorageButtons_[index], 100, 44);
      lv_obj_align(usbStorageButtons_[index], LV_ALIGN_TOP_LEFT, 16 + index * 108, 240);
      lv_obj_add_event_cb(usbStorageButtons_[index], onUsbStorageMode, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
      usbStorageButtonLabels_[index] = lv_label_create(usbStorageButtons_[index]);
      lv_obj_center(usbStorageButtonLabels_[index]);
    }
  }
  const char *status = !enabled ? "USB STORAGE OFF\n\nRIDING MODE" :
      ejected ? "SD EJECTED\nREAD/WRITE\n\nRECORDING OFF" :
      !ready ? "SD UNAVAILABLE\nNO MEDIUM\n\nRECORDING OFF" :
      hostConnected ? "SD READY\nREAD/WRITE\n\nUSB CONNECTED\nRECORDING OFF" :
                      "SD READY\nREAD/WRITE\n\nUSB DISCONNECTED\nRECORDING OFF";
  if (!usbStorageError_ && strcmp(lv_label_get_text(usbStorageStatus_), status)) lv_label_set_text(usbStorageStatus_, status);
  for (unsigned index = 0; index < 2; ++index) {
    lv_label_set_text(usbStorageButtonLabels_[index], index ? localized("ON", "开启") : localized("OFF", "关闭"));
    lv_obj_set_style_text_font(usbStorageButtonLabels_[index], chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_jetbrains_mono_14, 0);
    if (enabled == (index == 1)) lv_obj_add_state(usbStorageButtons_[index], LV_STATE_DISABLED);
    else lv_obj_clear_state(usbStorageButtons_[index], LV_STATE_DISABLED);
  }
  if (enabled) lv_obj_add_state(lv_obj_get_child(usbStoragePanel_, 0), LV_STATE_DISABLED);
  else lv_obj_clear_state(lv_obj_get_child(usbStoragePanel_, 0), LV_STATE_DISABLED);
  lv_obj_clear_flag(usbStoragePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(usbStoragePanel_);
}

void DisplayLcd::closePowerMeterPanels() {
  bleOperationActive_ = false;
  bleRequestInFlight_ = false;
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
}

bool DisplayLcd::saveDashboardSettings(bool force) {
  const auto failed = [this, force]() {
    if (force && !dashboardSaveNoticeShown_) {
      showNotice("Could not save dashboard settings. Shutdown/restart paused; retrying.");
      dashboardSaveNoticeShown_ = true;
    }
    return false;
  };
  if (dashboardSaveFailed_ && millis() - dashboardSaveFailedAt_ < 1000) return failed();
  DashboardLayout::Settings layout;
  if (!dashboard_.takeSettings(layout, force)) return true;
  if (!DashboardSettings::save(layout)) {
    dashboard_.retrySave();
    dashboardSaveFailed_ = true;
    dashboardSaveFailedAt_ = millis();
    AppLog.println("[UI] Could not save dashboard layout; pending settings retained");
    return failed();
  }
  dashboardSaveFailed_ = false;
  dashboardSaveNoticeShown_ = false;
  return true;
}

void DisplayLcd::tick() {
  static uint32_t lastTick = 0;
  uint32_t now = millis();
  uint32_t dt = now - lastTick;
  if (dt == 0) {
    dt = 5;
  }
  lastTick = now;
  lv_tick_inc(dt);
  saveDashboardSettings(false);
  if (statusLabel_) dashboard_.storage(sdActivity.state(sdCardReady(), now), now);
  lv_timer_handler();
}

void DisplayLcd::update(const WindVector &w, bool windValid, float powerW,
                        float cadRpm, float vgMps, float postureDeg, float rho,
                        int pmSrc, bool logOk) {
  if (!screen_) {
    return;
  }

  powerHistory_[powerHistoryIndex_ % powerHistorySize_] = powerW;
  powerHistoryIndex_++;

  float sum3 = 0.0f;
  int count3 = 0;
  int len3 = min(powerHistoryIndex_, powerHistorySize_);
  for (int i = 0; i < len3; ++i) {
    int idx = (powerHistoryIndex_ - 1 - i + powerHistorySize_) % powerHistorySize_;
    sum3 += powerHistory_[idx];
    count3++;
  }
  power3sAvg_ = count3 > 0 ? sum3 / count3 : 0.0f;

  char buf[64];
  float cda = -1.0f;
  int zone = 0;
  const float airspeed = w.vAir;
  if (windValid && pmSrc != 0 && power3sAvg_ > 0.0f &&
      airspeed >= CDA_MIN_AIRSPEED_MPS && rho > 0.0f) {
    const float groundSpeed = airspeed;
    const float wheelPower = power3sAvg_ * CDA_DRIVETRAIN_EFFICIENCY;
    const float rollingPower = CDA_TOTAL_MASS_KG * 9.80665f *
                               CDA_ROLLING_RESISTANCE * groundSpeed;
    const float aeroPower = wheelPower - rollingPower;
    if (aeroPower > 0.0f) {
      cda = aeroPower / (0.5f * rho * groundSpeed * groundSpeed * groundSpeed);
      if (cda < 0.23f) zone = 7;
      else if (cda < 0.27f) zone = 6;
      else if (cda < 0.31f) zone = 5;
      else if (cda < 0.36f) zone = 4;
      else if (cda < 0.42f) zone = 3;
      else if (cda < 0.50f) zone = 2;
      else zone = 1;
    }
  }

  estimatedCda_ = cda;
  currentZone_ = (uint32_t)zone;
  dashboard_.update(cda, zone, w.vAir, windValid, powerW, cadRpm);

  lv_obj_invalidate(screen_);
}

void DisplayLcd::updateClock(uint32_t utc) {
  if (!timeLabel_) return;
  char text[12] = "--:--:--";
  if (utc != 0) {
    const uint32_t seconds = ((uint64_t)utc + 8 * 3600) % 86400;
    snprintf(text, sizeof(text), "%02u:%02u:%02u",
             (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60),
             (unsigned)(seconds % 60));
  }
  lv_label_set_text(timeLabel_, text);
}
