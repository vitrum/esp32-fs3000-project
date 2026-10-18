#include "display_lcd.h"
#include "config.h"
#include <cstring>

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
    Serial.printf("[TOUCH] unexpected CST328 signature 0x%04X\n", signature);
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
  // 即时响应：检测到落指立即上报 PR，单击即可触发按钮（与原始行为一致）。
  uint16_t x = 0;
  uint16_t y = 0;
  if (g_display && g_display->touchReady_ && g_display->readTouch(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void DisplayLcd::onPowerSourceButton(lv_event_t *) {
  if (!g_display) return;
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

void DisplayLcd::onSettingsBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
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

void DisplayLcd::onLanguageSelected(lv_event_t *event) {
  if (!g_display) return;
  const bool chinese = (uintptr_t)lv_event_get_user_data(event) == 1;
  // 中文界面暂时禁用（渲染卡死待修复）：忽略选择，始终保持英文。
  if (chinese) {
    Serial.println("[UI] Chinese language temporarily disabled; staying English.");
    return;
  }
  g_display->setLanguage(false);
}

void DisplayLcd::onWifiSwitchChanged(lv_event_t *event) {
  if (!g_display) return;
  g_display->requestedWifiEnable_ =
      lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
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
  if (!settingsWifiSwitch_ || !settingsWifiStatus_) return;
  if (wifiEnabled_)
    lv_obj_add_state(settingsWifiSwitch_, LV_STATE_CHECKED);
  else
    lv_obj_clear_state(settingsWifiSwitch_, LV_STATE_CHECKED);

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

void DisplayLcd::onBleSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 1;
  g_display->requestedBleScan_ = true;
  g_display->selectedPowerMode_ = 1;
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  g_display->showBleLoading("Scanning for BLE power meters and smart trainers...");
}

void DisplayLcd::onBleScanAgain(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedBleScan_ = true;
  g_display->showBleLoading("Scanning for BLE power meters and smart trainers...");
}

void DisplayLcd::onBleDeviceSelected(lv_event_t *event) {
  if (!g_display) return;
  size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
  if (index >= g_display->bleDeviceCount_) return;

  g_display->selectedBleDevice_ = index;
  lv_label_set_text(g_display->bleConfirmNameLabel_,
                    g_display->bleDevices_[index].name);
  lv_label_set_text(g_display->bleConfirmAddressLabel_,
                    g_display->bleDevices_[index].address);
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
  snprintf(g_display->requestedBleAddress_, sizeof(g_display->requestedBleAddress_),
           "%s", g_display->bleDevices_[g_display->selectedBleDevice_].address);
  g_display->requestedBleConnect_ = true;
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  g_display->showBleLoading("Connecting to selected BLE device...");
}

void DisplayLcd::onBleBack(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedBleCancel_ = true;
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onBleConfirmBack(lv_event_t *) {
  if (!g_display) return;
  lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onBleLoadingBack(lv_event_t *) {
  if (!g_display) return;
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

void DisplayLcd::onAntSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 2;
  g_display->selectedPowerMode_ = 2;
  g_display->refreshLanguage();
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
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

void DisplayLcd::onVolumeSelected(lv_event_t *event) {
  if (!g_display) return;
  const int vol = (int)(uintptr_t)lv_event_get_user_data(event);
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

  static const uint8_t kVolumeOptions[kVolumeOptionCount] = {0, 25, 50, 75, 100};
  for (size_t i = 0; i < kVolumeOptionCount; ++i) {
    const bool selected = currentVolume_ == kVolumeOptions[i];
    lv_obj_set_style_bg_color(soundVolumeButtons_[i],
                              selected ? lv_color_make(37, 99, 235)
                                       : lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_text_color(soundVolumeButtonLabels_[i],
                                selected ? lv_color_white()
                                         : lv_color_make(30, 41, 59), 0);
  }
}

void DisplayLcd::onSdCardButton(lv_event_t *) {
  if (!g_display) return;
  g_display->sdListRequested_ = true;   // 由主循环加载文件列表
  lv_obj_add_flag(g_display->settingsPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(g_display->sdStatusLabel_,
                    g_display->localized("Loading files...", "正在读取文件..."));
  for (size_t i = 0; i < 12; ++i)
    lv_obj_add_flag(g_display->sdRowButtons_[i], LV_OBJ_FLAG_HIDDEN);
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
  if (!g_display->sdFiles_[index].isAudio) {
    lv_label_set_text(g_display->sdNowPlayingLabel_,
                      g_display->localized("Not an audio file", "不是音频文件"));
    return;
  }
  snprintf(g_display->sdPlayPath_, sizeof(g_display->sdPlayPath_), "/%s",
           g_display->sdFiles_[index].name);
  g_display->sdPlayRequested_ = true;
}

void DisplayLcd::onSdStopClicked(lv_event_t *) {
  if (!g_display) return;
  g_display->sdStopRequested_ = true;
}

bool DisplayLcd::takeSdListRequest() {
  if (!sdListRequested_) return false;
  sdListRequested_ = false;
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

void DisplayLcd::showSettings() {
  if (!settingsPanel_) return;
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
    fixText = localized("GPS disabled", "GPS 已关闭");
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
  if (!gpsIndicatorLabel_ || !gpsIndicatorSpinner_) return;

  char text[24];
  if (!enabled) {
    // GPS 关闭：灰色图标，无动画
    snprintf(text, sizeof(text), LV_SYMBOL_GPS " --");
    lv_label_set_text(gpsIndicatorLabel_, text);
    lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_make(148, 163, 184), 0);
    lv_obj_add_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(gpsIndicatorLabel_, 34);
    gpsIndicatorFixed_ = false;
  } else if (fix) {
    // 已定位：绿色图标 + 卫星数，无动画
    snprintf(text, sizeof(text), LV_SYMBOL_GPS " %u", (unsigned)sat);
    lv_label_set_text(gpsIndicatorLabel_, text);
    lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_make(22, 163, 74), 0);
    lv_obj_add_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(gpsIndicatorLabel_, 34);
    gpsIndicatorFixed_ = true;
  } else {
    // 搜星中：橙色图标 + 旋转动画，无卫星数
    snprintf(text, sizeof(text), LV_SYMBOL_GPS);
    lv_label_set_text(gpsIndicatorLabel_, text);
    lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_make(217, 119, 6), 0);
    lv_obj_clear_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(gpsIndicatorLabel_, 34);
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
  lv_obj_set_style_text_font(gpsFixLabel_, &lv_font_montserrat_16, 0);
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
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(gpsRestartLabel_, &lv_font_montserrat_14, 0);
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
  sdFileCount_ = min(count, (size_t)12);
  if (files)
    for (size_t i = 0; i < sdFileCount_; ++i) sdFiles_[i] = files[i];
  (void)status;
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
  if (sdFileCount_ == 0)
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
    char text[96];
    const SdFileEntry &f = sdFiles_[i];
    // 音频文件显示 ▶，其它显示普通图标；并显示大小(KB)
    if (f.isAudio)
      snprintf(text, sizeof(text), LV_SYMBOL_PLAY " %s\n%lu KB",
               f.name, (unsigned long)(f.size / 1024));
    else
      snprintf(text, sizeof(text), LV_SYMBOL_FILE " %s\n%lu KB",
               f.name, (unsigned long)(f.size / 1024));
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label,
        chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);
    // 可播放项高亮背景
    lv_obj_set_style_bg_color(button,
        f.isAudio ? lv_color_make(219, 234, 254) : lv_color_make(241, 245, 249), 0);
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
  lv_obj_add_event_cb(backButton, backCallback, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(backButton);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " BACK");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(backLabel, lv_color_make(51, 65, 85), 0);
  lv_obj_center(backLabel);

  lv_obj_t *titleLabel = lv_label_create(panel);
  lv_label_set_text(titleLabel, title);
  lv_obj_set_width(titleLabel, 142);
  lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_16, 0);
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
  lv_obj_set_style_text_font(powerSourceHint_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(powerSourceHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(powerSourceHint_, LV_ALIGN_TOP_LEFT, 12, 60);

  const char *labels[] = {"SCAN BLE DEVICES", "ANT+ POWER METER",
                          "VIRTUAL POWER METER", "AUTO SELECT"};
  lv_event_cb_t callbacks[] = {onBleSelected, onAntSelected,
                               onVirtualSelected, onAutoSelected};
  const lv_color_t colors[] = {
    lv_color_make(219, 234, 254), lv_color_make(239, 244, 248),
    lv_color_make(239, 244, 248), lv_color_make(239, 244, 248)
  };
  const lv_coord_t heights[] = {38, 38, 38, 38};
  lv_coord_t y = 98;
  for (size_t i = 0; i < 4; ++i) {
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
    lv_obj_set_style_text_font(powerSourceOptions_[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(powerSourceOptions_[i], lv_color_make(30, 41, 59), 0);
    lv_obj_center(powerSourceOptions_[i]);
    y += heights[i] + 6;
  }
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
  lv_obj_set_style_text_font(bleDeviceHint_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(scanLabel, &lv_font_montserrat_14, 0);
  lv_obj_center(scanLabel);

  bleStatusLabel_ = lv_label_create(bleDevicePanel_);
  lv_label_set_text(bleStatusLabel_, "No scan yet");
  lv_obj_set_width(bleStatusLabel_, 216);
  lv_label_set_long_mode(bleStatusLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(bleStatusLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bleStatusLabel_, lv_color_make(37, 99, 235), 0);
  lv_obj_align(bleStatusLabel_, LV_ALIGN_TOP_LEFT, 12, 116);

  for (size_t i = 0; i < 4; ++i) {
    bleDeviceButtons_[i] = lv_btn_create(bleDevicePanel_);
    lv_obj_set_size(bleDeviceButtons_[i], 216, 38);
    lv_obj_set_style_radius(bleDeviceButtons_[i], 0, LV_PART_MAIN);
    lv_obj_align(bleDeviceButtons_[i], LV_ALIGN_TOP_LEFT, 12,
                 (lv_coord_t)(136 + i * 40));
    lv_obj_set_style_bg_color(bleDeviceButtons_[i], lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_pad_all(bleDeviceButtons_[i], 1, 0);
    lv_obj_set_style_border_color(bleDeviceButtons_[i], lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(bleDeviceButtons_[i], 1, 0);
    lv_obj_add_event_cb(bleDeviceButtons_[i], onBleDeviceSelected,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_t *deviceLabel = lv_label_create(bleDeviceButtons_[i]);
    lv_obj_set_width(deviceLabel, 196);
    lv_label_set_long_mode(deviceLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(deviceLabel, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(bleConfirmNameLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(bleConfirmNameLabel_, lv_color_make(23, 32, 51), 0);
  lv_obj_align(bleConfirmNameLabel_, LV_ALIGN_TOP_LEFT, 12, 76);

  bleConfirmAddressLabel_ = lv_label_create(bleConfirmPanel_);
  lv_label_set_text(bleConfirmAddressLabel_, "");
  lv_obj_set_width(bleConfirmAddressLabel_, 216);
  lv_obj_set_height(bleConfirmAddressLabel_, 44);
  lv_label_set_long_mode(bleConfirmAddressLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(bleConfirmAddressLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(confirmLabel, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(bleLoadingLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(virtualPowerTitle_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(virtualPowerTitle_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(virtualPowerTitle_, LV_ALIGN_TOP_LEFT, 16, 64);

  virtualPowerValueLabel_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualPowerValueLabel_, "0 W");
  lv_obj_set_style_text_font(virtualPowerValueLabel_, &lv_font_montserrat_16, 0);
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
  lv_obj_set_style_text_font(virtualCadenceTitle_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(virtualCadenceTitle_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(virtualCadenceTitle_, LV_ALIGN_TOP_LEFT, 16, 146);

  virtualCadenceValueLabel_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualCadenceValueLabel_, "0 rpm");
  lv_obj_set_style_text_font(virtualCadenceValueLabel_, &lv_font_montserrat_16, 0);
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
  lv_obj_set_style_text_font(virtualHint_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsPowerButtonLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsLanguageEntryLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsWifiEntryLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsSoundButtonLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsSdButtonLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsGpsButtonLabel_, &lv_font_montserrat_14, 0);
  lv_obj_center(settingsGpsButtonLabel_);

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
  lv_obj_set_style_text_font(languageHint_, &lv_font_montserrat_14, 0);
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
                               i == 1 ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(settingsWifiLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(settingsWifiLabel_, lv_color_make(30, 41, 59), 0);
  lv_obj_align(settingsWifiLabel_, LV_ALIGN_TOP_LEFT, 16, 62);

  settingsWifiSwitch_ = lv_switch_create(wifiPanel_);
  lv_obj_align(settingsWifiSwitch_, LV_ALIGN_TOP_RIGHT, -16, 58);
  lv_obj_add_event_cb(settingsWifiSwitch_, onWifiSwitchChanged,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  settingsWifiStatus_ = lv_label_create(wifiPanel_);
  lv_obj_set_width(settingsWifiStatus_, 208);
  lv_label_set_long_mode(settingsWifiStatus_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(settingsWifiStatus_, lv_color_make(71, 85, 105), 0);
  lv_obj_align(settingsWifiStatus_, LV_ALIGN_TOP_LEFT, 16, 104);

  refreshWifiStatus();
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
  lv_label_set_text(soundHint_, "Tap a level to set volume.\nA test tone plays at that volume.");
  lv_obj_set_width(soundHint_, 216);
  lv_label_set_long_mode(soundHint_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(soundHint_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(soundHint_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(soundHint_, LV_ALIGN_TOP_LEFT, 12, 58);

  soundVolumeLabel_ = lv_label_create(soundPanel_);
  lv_label_set_text(soundVolumeLabel_, "Volume: 70%");
  lv_obj_set_style_text_font(soundVolumeLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(soundVolumeLabel_, lv_color_make(23, 32, 51), 0);
  lv_obj_align(soundVolumeLabel_, LV_ALIGN_TOP_LEFT, 12, 112);

  // 五档音量按钮：0 / 25 / 50 / 75 / 100
  static const char *const kVolumeLabels[kVolumeOptionCount] = {
      "MUTE", "25%", "50%", "75%", "100%"};
  static const uint8_t kVolumeValues[kVolumeOptionCount] = {0, 25, 50, 75, 100};
  lv_coord_t y = 148;
  for (size_t i = 0; i < kVolumeOptionCount; ++i) {
    lv_obj_t *button = lv_btn_create(soundPanel_);
    lv_obj_set_size(button, 216, 30);
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_border_color(button, lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 12, y);
    lv_obj_add_event_cb(button, onVolumeSelected, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)kVolumeValues[i]);
    soundVolumeButtons_[i] = button;
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, kVolumeLabels[i]);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
    soundVolumeButtonLabels_[i] = label;
    y += 30 + 4;
  }

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

  sdStatusLabel_ = lv_label_create(sdPanel_);
  lv_label_set_text(sdStatusLabel_, "Loading files...");
  lv_obj_set_width(sdStatusLabel_, 216);
  lv_obj_set_style_text_font(sdStatusLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sdStatusLabel_, lv_color_make(100, 116, 139), 0);
  lv_obj_align(sdStatusLabel_, LV_ALIGN_TOP_LEFT, 12, 54);

  // 可滚动文件列表
  sdList_ = lv_obj_create(sdPanel_);
  lv_obj_set_size(sdList_, 216, 178);
  lv_obj_align(sdList_, LV_ALIGN_TOP_LEFT, 12, 76);
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
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_make(30, 41, 59), 0);
    lv_obj_center(label);
    sdRowLabels_[i] = label;
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  }

  sdNowPlayingLabel_ = lv_label_create(sdPanel_);
  lv_label_set_text(sdNowPlayingLabel_, "Stopped");
  lv_obj_set_width(sdNowPlayingLabel_, 216);
  lv_label_set_long_mode(sdNowPlayingLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(sdNowPlayingLabel_, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_text_font(sdStopLabel_, &lv_font_montserrat_14, 0);
  lv_obj_center(sdStopLabel_);

  lv_obj_add_flag(sdPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::setLanguage(bool chinese) {
  chinese_ = chinese;
  Preferences preferences;
  if (!preferences.begin("aero-ui", false)) {
    Serial.println("[UI] Could not open preferences to save language.");
  } else {
    size_t written = preferences.putBool("zh", chinese_);
    if (written == 0) {
      Serial.println("[UI] Could not save language preference.");
    }
    preferences.end();
  }
  refreshLanguage();
}

void DisplayLcd::refreshBleDeviceButtons() {
  for (size_t i = 0; i < 4; ++i) {
    lv_obj_t *button = bleDeviceButtons_[i];
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (i >= bleDeviceCount_) {
      lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const char *profile = bleDevices_[i].cyclingPower
                              ? (bleDevices_[i].fitnessMachine
                                     ? localized("CPS + FTMS", "功率计 + 骑行台")
                                     : localized("CPS", "功率计"))
                              : (bleDevices_[i].fitnessMachine
                                     ? localized("FTMS trainer", "FTMS 骑行台")
                                     : (bleDevices_[i].speedCadence
                                            ? localized("CSC cadence only", "CSC 仅踏频")
                                            : localized("BLE device", "蓝牙设备")));
    char text[64];
    snprintf(text, sizeof(text), "%s\n%s  %d dBm",
             bleDevices_[i].name, profile, bleDevices_[i].rssi);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
  }
}

void DisplayLcd::refreshLanguage() {
  Serial.printf("[LANG] begin zh=%d\n", (int)chinese_);
  const lv_font_t *font = chinese_ ? &lv_font_noto_sans_sc_14
                                   : &lv_font_montserrat_14;
  const lv_font_t *titleFont = chinese_ ? &lv_font_noto_sans_sc_14
                                        : &lv_font_montserrat_16;
  lv_obj_set_style_text_font(cdaTitleLabel_, font, 0);
  lv_label_set_text(cdaTitleLabel_, localized("ESTIMATED CdA", "估算 CdA"));
  lv_label_set_text(windLabelTitle_, localized("WIND SPEED", "风速"));
  lv_obj_set_style_text_font(windLabelTitle_, font, 0);
  lv_label_set_text(avg3Label_, localized("LIVE POWER", "实时功率"));
  lv_obj_set_style_text_font(avg3Label_, font, 0);

  const char *posture = "WAITING FOR DATA";
  const char *postureChinese = "等待数据";
  if (currentZone_ == 1) { posture = "UPRIGHT"; postureChinese = "直立"; }
  else if (currentZone_ == 2) { posture = "LOW / DROPS"; postureChinese = "低位 / 下把"; }
  else if (currentZone_ == 3) { posture = "AERO"; postureChinese = "气动"; }
  else if (currentZone_ == 4) { posture = "AGGRESSIVE"; postureChinese = "激进"; }
  else if (currentZone_ == 5) { posture = "VERY AERO"; postureChinese = "极致气动"; }
  else if (currentZone_ == 6) { posture = "RACE TT"; postureChinese = "竞赛 TT"; }
  else if (currentZone_ == 7) { posture = "ELITE TT"; postureChinese = "精英 TT"; }
  lv_label_set_text(postureLabel_, localized(posture, postureChinese));
  lv_obj_set_style_text_font(postureLabel_, font, 0);

  lv_label_set_text(powerSourceLabel_, localized("SET", "设置"));
  lv_obj_set_style_text_font(powerSourceLabel_, titleFont, 0);

  lv_label_set_text(statusLabel_,
                    localized("LOG: OFF", "日志: 关闭"));
  lv_obj_set_style_text_font(statusLabel_, font, 0);
  lv_obj_set_style_text_font(cadenceLabel_, font, 0);
  Serial.println("[LANG] 1 main done");

  lv_label_set_text(powerSourceTitle_, localized("POWER SOURCE", "功率来源"));
  lv_label_set_text(powerSourceHint_, localized("Choose a power source", "选择功率来源"));
  const char *sourceLabels[] = {
    "SCAN BLE DEVICES", "ANT+ POWER METER",
    "VIRTUAL POWER METER", "AUTO SELECT"
  };
  const char *sourceLabelsZh[] = {
    "扫描蓝牙设备", "ANT+ 功率计", "虚拟功率计", "自动选择"
  };
  for (size_t i = 0; i < 4; ++i) {
    lv_label_set_text(powerSourceOptions_[i],
                      localized(sourceLabels[i], sourceLabelsZh[i]));
    lv_obj_set_style_text_font(powerSourceOptions_[i], font, 0);
  }
  lv_obj_set_style_text_font(powerSourceTitle_, titleFont, 0);
  lv_obj_set_style_text_font(powerSourceHint_, font, 0);
  Serial.println("[LANG] 2 powersrc done");

  lv_label_set_text(bleDeviceTitle_, localized("BLE DEVICES", "蓝牙设备"));
  lv_label_set_text(bleDeviceHint_, localized("Tap a device, then confirm",
                                               "点击设备后确认连接"));
  lv_label_set_text(bleScanLabel_, localized("SCAN AGAIN", "重新扫描"));
  lv_obj_set_style_text_font(bleDeviceTitle_, titleFont, 0);
  lv_obj_set_style_text_font(bleDeviceHint_, font, 0);
  lv_obj_set_style_text_font(bleScanLabel_, font, 0);
  if (!bleScanCompleted_)
    lv_label_set_text(bleStatusLabel_, localized("No scan yet", "尚未扫描"));
  else if (bleDeviceCount_ == 0)
    lv_label_set_text(bleStatusLabel_, localized("No connectable BLE devices found",
                                                 "未找到可连接的蓝牙设备"));
  else {
    char scanStatus[40];
    snprintf(scanStatus, sizeof(scanStatus),
             chinese_ ? "找到 %u 个设备" : "%u device(s) found",
             (unsigned)bleDeviceCount_);
    lv_label_set_text(bleStatusLabel_, scanStatus);
  }
  lv_obj_set_style_text_font(bleStatusLabel_, font, 0);
  refreshBleDeviceButtons();
  Serial.println("[LANG] 3 ble done");

  lv_label_set_text(bleConfirmTitle_, localized("CONFIRM DEVICE", "确认设备"));
  lv_label_set_text(bleConfirmButtonLabel_, localized("CONNECT", "连接"));
  lv_obj_set_style_text_font(bleConfirmTitle_, titleFont, 0);
  lv_obj_set_style_text_font(bleConfirmButtonLabel_, font, 0);
  lv_obj_set_style_text_font(bleConfirmAddressLabel_, font, 0);
  lv_label_set_text(bleLoadingTitle_,
                    localized("LOADING", "正在加载"));
  lv_label_set_text(bleLoadingBackLabel_,
                    localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
  lv_obj_set_style_text_font(bleLoadingTitle_, titleFont, 0);
  lv_obj_set_style_text_font(bleLoadingBackLabel_, font, 0);
  lv_obj_set_style_text_font(bleLoadingLabel_, font, 0);
  for (size_t i = 0; i < 4; ++i)
    lv_obj_set_style_text_font(lv_obj_get_child(bleDeviceButtons_[i], 0), font, 0);
  Serial.println("[LANG] 4 ble2 done");

  lv_label_set_text(virtualTitle_, localized("VIRTUAL POWER", "虚拟功率计"));
  lv_label_set_text(virtualPowerTitle_, localized("POWER", "功率"));
  lv_label_set_text(virtualCadenceTitle_, localized("CADENCE", "踏频"));
  lv_label_set_text(virtualHint_,
                    localized("Drag either slider to update\nlive power and cadence.",
                              "拖动滑块设置实时功率\n和踏频。"));
  lv_obj_set_style_text_font(virtualTitle_, titleFont, 0);
  lv_obj_set_style_text_font(virtualPowerTitle_, font, 0);
  lv_obj_set_style_text_font(virtualCadenceTitle_, font, 0);
  lv_obj_set_style_text_font(virtualHint_, font, 0);
  Serial.println("[LANG] 5 virtual done");

  lv_label_set_text(settingsTitle_, localized("SETTINGS", "设置"));
  if (settingsPowerButtonLabel_)
    lv_label_set_text(settingsPowerButtonLabel_,
                      localized(LV_SYMBOL_POWER "\n" "POWER", LV_SYMBOL_POWER "\n" "功率"));
  if (settingsLanguageEntryLabel_)
    lv_label_set_text(settingsLanguageEntryLabel_,
                      localized(LV_SYMBOL_LIST "\n" "LANG", LV_SYMBOL_LIST "\n" "语言"));
  if (settingsWifiEntryLabel_)
    lv_label_set_text(settingsWifiEntryLabel_,
                      localized(LV_SYMBOL_WIFI "\n" "WI-FI", LV_SYMBOL_WIFI "\n" "WI-FI"));
  lv_label_set_text(settingsSdButtonLabel_,
                    localized(LV_SYMBOL_SD_CARD "\n" "SD/MUSIC", LV_SYMBOL_SD_CARD "\n" "SD/音乐"));
  lv_label_set_text(settingsSoundButtonLabel_,
                    localized(LV_SYMBOL_AUDIO "\n" "SOUND", LV_SYMBOL_AUDIO "\n" "声音"));
  lv_label_set_text(settingsGpsButtonLabel_,
                    localized(LV_SYMBOL_GPS "\n" "GPS", LV_SYMBOL_GPS "\n" "GPS"));
  lv_obj_set_style_text_font(settingsTitle_, titleFont, 0);
  if (settingsPowerButtonLabel_) lv_obj_set_style_text_font(settingsPowerButtonLabel_, font, 0);
  if (settingsLanguageEntryLabel_) lv_obj_set_style_text_font(settingsLanguageEntryLabel_, font, 0);
  if (settingsWifiEntryLabel_) lv_obj_set_style_text_font(settingsWifiEntryLabel_, font, 0);
  lv_obj_set_style_text_font(settingsSdButtonLabel_, font, 0);
  lv_obj_set_style_text_font(settingsSoundButtonLabel_, font, 0);
  lv_obj_set_style_text_font(settingsGpsButtonLabel_, font, 0);

  // 独立「语言」页面文字
  if (languageTitle_) {
    lv_label_set_text(languageTitle_, localized("LANGUAGE", "语言"));
    lv_obj_set_style_text_font(languageTitle_, titleFont, 0);
  }
  if (languageHint_) {
    lv_label_set_text(languageHint_, localized("Choose interface language", "选择界面语言"));
    lv_obj_set_style_text_font(languageHint_, font, 0);
  }
  // 独立「Wi-Fi」页面文字
  if (wifiTitle_) {
    lv_label_set_text(wifiTitle_, localized("WI-FI", "WI-FI"));
    lv_obj_set_style_text_font(wifiTitle_, titleFont, 0);
  }
  if (settingsWifiLabel_)
    lv_label_set_text(settingsWifiLabel_, localized("Wi-Fi", "Wi-Fi"));
  Serial.println("[LANG] 6 settings+lang+wifi pages done");

  // GPS 页面文字
  lv_label_set_text(gpsTitle_, localized("GPS", "GPS"));
  lv_label_set_text(gpsRestartLabel_,
                    localized(LV_SYMBOL_REFRESH " RESTART GPS",
                              LV_SYMBOL_REFRESH " 重启 GPS"));
  lv_obj_set_style_text_font(gpsTitle_, titleFont, 0);
  lv_obj_set_style_text_font(gpsRestartLabel_, font, 0);
  lv_obj_set_style_text_font(gpsFixLabel_, titleFont, 0);
  lv_obj_set_style_text_font(gpsUtcLabel_, font, 0);
  lv_obj_set_style_text_font(gpsLocalLabel_, font, 0);
  lv_obj_set_style_text_font(gpsCoordLabel_, font, 0);
  lv_obj_set_style_text_font(gpsSatLabel_, font, 0);
  lv_obj_set_style_text_font(gpsAltLabel_, font, 0);
  lv_obj_set_style_text_font(gpsSpeedLabel_, font, 0);
  Serial.println("[LANG] 7 gps done");
  lv_obj_set_style_text_font(settingsTitle_, titleFont, 0);
  if (settingsWifiLabel_) lv_obj_set_style_text_font(settingsWifiLabel_, font, 0);
  if (settingsPowerButtonLabel_) lv_obj_set_style_text_font(settingsPowerButtonLabel_, font, 0);
  lv_obj_set_style_text_font(settingsSdButtonLabel_, font, 0);
  lv_obj_set_style_text_font(settingsSoundButtonLabel_, font, 0);

  // 声音设置页文字
  lv_label_set_text(soundTitle_, localized("SOUND", "声音"));
  lv_label_set_text(soundHint_,
                    localized("Tap a level to set volume.\nA test tone plays at that volume.",
                              "点击档位设置音量，\n并以该音量播放试听音。"));
  lv_obj_set_style_text_font(soundTitle_, titleFont, 0);
  lv_obj_set_style_text_font(soundHint_, font, 0);
  lv_obj_set_style_text_font(soundVolumeLabel_, font, 0);
  {
    static const char *const en[kVolumeOptionCount] = {"MUTE", "25%", "50%", "75%", "100%"};
    static const char *const zh[kVolumeOptionCount] = {"静音", "25%", "50%", "75%", "100%"};
    for (size_t i = 0; i < kVolumeOptionCount; ++i) {
      lv_label_set_text(soundVolumeButtonLabels_[i],
                        localized(en[i], zh[i]));
      lv_obj_set_style_text_font(soundVolumeButtonLabels_[i],
                                 chinese_ ? &lv_font_noto_sans_sc_14
                                          : &lv_font_montserrat_14, 0);
    }
    refreshSoundVolume();
  }
  Serial.println("[LANG] 8 sound done");

  // SD 浏览页文字
  lv_label_set_text(sdTitle_, localized("SD CARD", "SD 卡"));
  lv_label_set_text(sdStopLabel_, localized(LV_SYMBOL_STOP " STOP", LV_SYMBOL_STOP " 停止"));
  lv_obj_set_style_text_font(sdTitle_, titleFont, 0);
  lv_obj_set_style_text_font(sdStatusLabel_, font, 0);
  lv_obj_set_style_text_font(sdStopLabel_, font, 0);
  lv_obj_set_style_text_font(sdNowPlayingLabel_, font, 0);
  refreshSdList();
  {
    char text[128];
    if (audioPlaying_ && audioTitle_[0])
      snprintf(text, sizeof(text), localized("Playing: %s", "播放中: %s"), audioTitle_);
    else
      snprintf(text, sizeof(text), "%s", localized("Stopped", "已停止"));
    lv_label_set_text(sdNowPlayingLabel_, text);
  }
  Serial.println("[LANG] 9 sd done");
  lv_obj_set_style_text_font(settingsWifiStatus_,
                             chinese_ ? &lv_font_noto_sans_sc_14
                                      : &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(settingsLanguageButtonLabels_[0],
                             &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_font(settingsLanguageButtonLabels_[1],
                             &lv_font_noto_sans_sc_14, 0);
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
  Serial.println("[LANG] 10 langbtn+wifi done");

  if (noticeBackLabel_) {
    lv_label_set_text(noticeBackLabel_, localized("OK", "确定"));
    lv_obj_set_style_text_font(noticeBackLabel_, font, 0);
  }

  Serial.println("[LANG] 11 panels begin");
  lv_obj_t *panels[] = {
    powerSourcePanel_, bleDevicePanel_, bleConfirmPanel_,
    bleLoadingPanel_, virtualPowerPanel_, settingsPanel_, soundPanel_, sdPanel_,
    gpsPanel_, languagePanel_, wifiPanel_
  };
  for (lv_obj_t *panel : panels) {
    lv_obj_t *backButton = lv_obj_get_child(panel, 0);
    lv_obj_t *backLabel = lv_obj_get_child(backButton, 0);
    lv_label_set_text(backLabel,
                      localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
    lv_obj_set_style_text_font(backLabel, font, 0);
  }
  Serial.println("[LANG] 12 done");
}

void DisplayLcd::begin() {
  g_display = this;

  // 暂时强制英文界面：中文渲染在某些面板会触发卡死（待修复）。
  // 先从 NVS 读一次并清掉，避免遗留的 zh=true 影响。
  chinese_ = false;
  {
    Preferences preferences;
    if (preferences.begin("aero-ui", false)) {
      if (preferences.getBool("zh", false)) {
        preferences.putBool("zh", false);
        Serial.println("[UI] Language forced to English (Chinese temporarily disabled).");
      }
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
    Serial.println("[TOUCH] CST328 ready");
  } else {
    Serial.println("[TOUCH] CST328 unavailable; touch controls disabled");
  }

  screen_ = lv_obj_create(nullptr);
  lv_obj_set_size(screen_, 240, 320);
  lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(screen_, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(screen_, lv_color_make(248, 250, 252), 0);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

  // 顶部左侧：GPS 状态指示（替代原来的固定文字 "AERO PROBE"）。
  // 用一个小旋转动画表示"搜星中"，定位后用绿色图标 + 卫星数表示。
  gpsIndicatorSpinner_ = lv_spinner_create(screen_, 1000, 60);
  lv_obj_set_size(gpsIndicatorSpinner_, 16, 16);
  lv_obj_align(gpsIndicatorSpinner_, LV_ALIGN_TOP_LEFT, 12, 8);
  lv_obj_set_style_arc_color(gpsIndicatorSpinner_, lv_color_make(219, 227, 236),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_width(gpsIndicatorSpinner_, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(gpsIndicatorSpinner_, lv_color_make(217, 119, 6),
                             LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(gpsIndicatorSpinner_, 3, LV_PART_INDICATOR);
  lv_obj_add_flag(gpsIndicatorSpinner_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏，查到状态后再显示

  gpsIndicatorLabel_ = lv_label_create(screen_);
  lv_label_set_text(gpsIndicatorLabel_, LV_SYMBOL_GPS " --");
  lv_obj_set_style_text_font(gpsIndicatorLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gpsIndicatorLabel_, lv_color_make(148, 163, 184), 0);
  lv_obj_align(gpsIndicatorLabel_, LV_ALIGN_TOP_LEFT, 34, 8);

  lv_obj_t *headerLine = lv_obj_create(screen_);
  lv_obj_set_size(headerLine, 216, 2);
  lv_obj_align(headerLine, LV_ALIGN_TOP_LEFT, 12, 36);
  lv_obj_set_style_bg_color(headerLine, lv_color_make(203, 213, 225), 0);

  lv_obj_t *cdaCard = lv_obj_create(screen_);
  lv_obj_set_size(cdaCard, 216, 74);
  lv_obj_align(cdaCard, LV_ALIGN_TOP_LEFT, 12, 43);
  lv_obj_clear_flag(cdaCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(cdaCard, 0, 0);
  lv_obj_set_style_radius(cdaCard, 8, 0);
  lv_obj_set_style_bg_color(cdaCard, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(cdaCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(cdaCard, lv_color_make(226, 232, 240), 0);
  lv_obj_set_style_border_width(cdaCard, 1, 0);

  cdaTitleLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(cdaTitleLabel_, "ESTIMATED CdA");
  lv_obj_align(cdaTitleLabel_, LV_ALIGN_TOP_LEFT, 8, 3);
  lv_obj_set_style_text_font(cdaTitleLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(cdaTitleLabel_, lv_color_make(100, 116, 139), 0);

  cdaLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(cdaLabel_, "-- m2");
  lv_obj_set_width(cdaLabel_, 132);
  lv_label_set_long_mode(cdaLabel_, LV_LABEL_LONG_CLIP);
  lv_obj_align(cdaLabel_, LV_ALIGN_TOP_LEFT, 8, 18);
  lv_obj_set_style_text_font(cdaLabel_, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(cdaLabel_, lv_color_make(23, 32, 51), 0);

  postureLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(postureLabel_, "WAITING FOR DATA");
  lv_obj_set_width(postureLabel_, 190);
  lv_label_set_long_mode(postureLabel_, LV_LABEL_LONG_CLIP);
  lv_obj_align(postureLabel_, LV_ALIGN_BOTTOM_LEFT, 8, -1);
  lv_obj_set_style_text_font(postureLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(postureLabel_, lv_color_make(100, 116, 139), 0);

  zoneLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(zoneLabel_, "--");
  lv_obj_align(zoneLabel_, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_text_font(zoneLabel_, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(zoneLabel_, lv_color_make(37, 99, 235), 0);

  lv_obj_t *cdaDivider = lv_obj_create(screen_);
  lv_obj_set_size(cdaDivider, 216, 1);
  lv_obj_align(cdaDivider, LV_ALIGN_TOP_LEFT, 12, 119);
  lv_obj_set_style_bg_color(cdaDivider, lv_color_make(203, 213, 225), 0);
  lv_obj_set_style_border_width(cdaDivider, 0, 0);

  lv_obj_t *windCard = lv_obj_create(screen_);
  lv_obj_set_size(windCard, 216, 56);
  lv_obj_align(windCard, LV_ALIGN_TOP_LEFT, 12, 123);
  lv_obj_clear_flag(windCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(windCard, 0, 0);
  lv_obj_set_style_radius(windCard, 8, 0);
  lv_obj_set_style_bg_color(windCard, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(windCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(windCard, lv_color_make(226, 232, 240), 0);
  lv_obj_set_style_border_width(windCard, 1, 0);

  windLabelTitle_ = lv_label_create(windCard);
  lv_label_set_text(windLabelTitle_, "WIND SPEED");
  lv_obj_align(windLabelTitle_, LV_ALIGN_TOP_LEFT, 8, 3);
  lv_obj_set_style_text_font(windLabelTitle_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(windLabelTitle_, lv_color_make(100, 116, 139), 0);

  windLabel_ = lv_label_create(windCard);
  lv_label_set_text(windLabel_, "0.0 km/h");
  lv_obj_align(windLabel_, LV_ALIGN_BOTTOM_LEFT, 8, -2);
  lv_obj_set_style_text_font(windLabel_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(windLabel_, lv_color_make(23, 32, 51), 0);

  lv_obj_t *windDivider = lv_obj_create(screen_);
  lv_obj_set_size(windDivider, 216, 1);
  lv_obj_align(windDivider, LV_ALIGN_TOP_LEFT, 12, 181);
  lv_obj_set_style_bg_color(windDivider, lv_color_make(203, 213, 225), 0);
  lv_obj_set_style_border_width(windDivider, 0, 0);

  lv_obj_t *powerCard = lv_obj_create(screen_);
  lv_obj_set_size(powerCard, 216, 76);
  lv_obj_align(powerCard, LV_ALIGN_TOP_LEFT, 12, 185);
  lv_obj_clear_flag(powerCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(powerCard, 0, 0);
  lv_obj_set_style_radius(powerCard, 8, 0);
  lv_obj_set_style_bg_color(powerCard, lv_color_make(239, 246, 255), 0);
  lv_obj_set_style_bg_opa(powerCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(powerCard, lv_color_make(191, 219, 254), 0);
  lv_obj_set_style_border_width(powerCard, 1, 0);

  avg3Label_ = lv_label_create(powerCard);
  lv_label_set_text(avg3Label_, "LIVE POWER");
  lv_obj_align(avg3Label_, LV_ALIGN_TOP_LEFT, 8, 5);
  lv_obj_set_style_text_font(avg3Label_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(avg3Label_, lv_color_make(37, 99, 235), 0);

  powerLabel_ = lv_label_create(powerCard);
  lv_label_set_text(powerLabel_, "0 W");
  lv_obj_set_width(powerLabel_, 190);
  lv_label_set_long_mode(powerLabel_, LV_LABEL_LONG_CLIP);
  lv_obj_align(powerLabel_, LV_ALIGN_CENTER, 0, 8);
  lv_obj_set_style_text_align(powerLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(powerLabel_, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(powerLabel_, lv_color_make(29, 78, 216), 0);

  lv_obj_t *powerDivider = lv_obj_create(screen_);
  lv_obj_set_size(powerDivider, 216, 1);
  lv_obj_align(powerDivider, LV_ALIGN_TOP_LEFT, 12, 265);
  lv_obj_set_style_bg_color(powerDivider, lv_color_make(203, 213, 225), 0);
  lv_obj_set_style_border_width(powerDivider, 0, 0);

  cadenceLabel_ = lv_label_create(screen_);
  lv_label_set_text(cadenceLabel_, "CADENCE 0 rpm");
  lv_obj_align(cadenceLabel_, LV_ALIGN_TOP_LEFT, 12, 268);
  lv_obj_set_style_text_font(cadenceLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(cadenceLabel_, lv_color_make(51, 65, 85), 0);

  timeLabel_ = lv_label_create(screen_);
  lv_label_set_text(timeLabel_, "00:00:00");
  lv_obj_align(timeLabel_, LV_ALIGN_TOP_RIGHT, -12, 268);
  lv_obj_set_style_text_font(timeLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(timeLabel_, lv_color_make(100, 116, 139), 0);

  statusLabel_ = lv_label_create(screen_);
  lv_label_set_text(statusLabel_, "LOG: OFF");
  lv_obj_set_width(statusLabel_, 68);
  lv_obj_set_style_text_align(statusLabel_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(statusLabel_, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_set_style_text_font(statusLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(statusLabel_, lv_color_make(100, 116, 139), 0);

  powerSourceButton_ = lv_btn_create(screen_);
  lv_obj_set_size(powerSourceButton_, 216, 28);
  lv_obj_align(powerSourceButton_, LV_ALIGN_TOP_LEFT, 12, 289);
  lv_obj_set_style_radius(powerSourceButton_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(powerSourceButton_, lv_color_make(37, 99, 235), 0);
  lv_obj_add_event_cb(powerSourceButton_, onSettingsButton, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *powerSourceIcon = lv_label_create(powerSourceButton_);
  lv_label_set_text(powerSourceIcon, LV_SYMBOL_SETTINGS);
  lv_obj_align(powerSourceIcon, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_set_style_text_font(powerSourceIcon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(powerSourceIcon, lv_color_white(), 0);
  powerSourceLabel_ = lv_label_create(powerSourceButton_);
  lv_label_set_text(powerSourceLabel_, "SET");
  lv_obj_set_width(powerSourceLabel_, 178);
  lv_label_set_long_mode(powerSourceLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(powerSourceLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(powerSourceLabel_, lv_color_white(), 0);
  lv_obj_align(powerSourceLabel_, LV_ALIGN_LEFT_MID, 28, 0);
  lv_obj_set_style_text_font(powerSourceLabel_, &lv_font_montserrat_16, 0);

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

bool DisplayLcd::takeBleConnectRequest(char *address, size_t capacity) {
  if (!requestedBleConnect_) return false;
  requestedBleConnect_ = false;
  if (!address || capacity == 0) return false;
  snprintf(address, capacity, "%s", requestedBleAddress_);
  requestedBleAddress_[0] = '\0';
  return true;
}

bool DisplayLcd::takeBleCancelRequest() {
  if (!requestedBleCancel_) return false;
  requestedBleCancel_ = false;
  return true;
}

void DisplayLcd::showBleLoading(const char *status) {
  bleOperationActive_ = true;
  requestedBleCancel_ = false;
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
}

void DisplayLcd::showBleConnectFailure(const char *error) {
  bleOperationActive_ = false;
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
  bleScanCompleted_ = true;
  bleDeviceCount_ = min(count, (size_t)4);
  if (devices)
    for (size_t i = 0; i < bleDeviceCount_; ++i) bleDevices_[i] = devices[i];
  (void)status;
  refreshLanguage();
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::closePowerMeterPanels() {
  bleOperationActive_ = false;
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
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
  if (pmSrc != 0 && power3sAvg_ > 0.0f &&
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

  currentZone_ = (uint32_t)zone;
  const char *posture = "WAITING FOR DATA";
  const char *postureChinese = "等待数据";
  if (zone == 1) { posture = "UPRIGHT"; postureChinese = "直立"; }
  else if (zone == 2) { posture = "LOW / DROPS"; postureChinese = "低位 / 下把"; }
  else if (zone == 3) { posture = "AERO"; postureChinese = "气动"; }
  else if (zone == 4) { posture = "AGGRESSIVE"; postureChinese = "激进"; }
  else if (zone == 5) { posture = "VERY AERO"; postureChinese = "极致气动"; }
  else if (zone == 6) { posture = "RACE TT"; postureChinese = "竞赛 TT"; }
  else if (zone == 7) { posture = "ELITE TT"; postureChinese = "精英 TT"; }
  if (cda >= 0.0f) {
    snprintf(buf, sizeof(buf), "%.2f m2", cda);
    lv_label_set_text(cdaLabel_, buf);
  } else {
    lv_label_set_text(cdaLabel_, "-- m2");
  }
  lv_label_set_text(postureLabel_, localized(posture, postureChinese));
  if (zone > 0) {
    snprintf(buf, sizeof(buf), "%d", zone);
    lv_label_set_text(zoneLabel_, buf);
  } else {
    lv_label_set_text(zoneLabel_, "--");
  }

  if (windValid)
    snprintf(buf, sizeof(buf), "%.1f km/h", w.vAir * 3.6f);
  else
    snprintf(buf, sizeof(buf), "-- km/h");
  lv_label_set_text(windLabel_, buf);

  snprintf(buf, sizeof(buf), "%.0f W", powerW);
  lv_label_set_text(powerLabel_, buf);

  snprintf(buf, sizeof(buf), localized("CADENCE %.0f rpm", "踏频 %.0f rpm"), cadRpm);
  lv_label_set_text(cadenceLabel_, buf);
  lv_obj_set_style_text_font(cadenceLabel_,
                             chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);

  lv_label_set_text(powerSourceLabel_, localized("SET", "设置"));
  lv_obj_set_style_text_color(powerSourceLabel_, lv_color_white(), 0);
  const lv_color_t meterButtonColor =
      pmSrc == 2 ? lv_color_make(21, 128, 61)
                 : (pmSrc == 3 ? lv_color_make(217, 119, 6)
                               : lv_color_make(37, 99, 235));
  lv_obj_set_style_bg_color(powerSourceButton_, meterButtonColor, LV_PART_MAIN);
  lv_obj_set_style_text_font(powerSourceLabel_,
                             chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_16, 0);

  snprintf(buf, sizeof(buf), localized("LOG %s", "日志 %s"),
           logOk ? "OK" : localized("OFF", "关闭"));
  lv_label_set_text(statusLabel_, buf);
  lv_obj_set_style_text_font(statusLabel_,
                             chinese_ ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(statusLabel_, logOk ? lv_color_make(22, 163, 74) : lv_color_make(100, 116, 139), 0);

  uint32_t now = millis();
  uint32_t sec = now / 1000;
  uint32_t h = (sec / 3600) % 24;
  uint32_t m = (sec / 60) % 60;
  uint32_t s = sec % 60;
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
  lv_label_set_text(timeLabel_, buf);

  lv_obj_invalidate(screen_);
}
