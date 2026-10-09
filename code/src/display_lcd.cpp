#include "display_lcd.h"
#include "config.h"

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
  uint16_t *buf = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
  if (!buf) {
    lv_disp_flush_ready(drv);
    return;
  }

  for (int32_t y = 0; y < h; y++) {
    for (int32_t x = 0; x < w; x++) {
      uint32_t idx = (uint32_t)y * w + x;
      buf[idx] = rgb565FromLvColor(color_p[idx]);
    }
  }

  g_display->_gfx->draw16bitRGBBitmap(area->x1, area->y1, buf, w, h);
  free(buf);
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

void DisplayLcd::onLanguageSelected(lv_event_t *event) {
  if (!g_display) return;
  const bool chinese = (uintptr_t)lv_event_get_user_data(event) == 1;
  g_display->setLanguage(chinese);
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

  settingsLanguageLabel_ = lv_label_create(settingsPanel_);
  lv_label_set_text(settingsLanguageLabel_, "Interface language");
  lv_obj_set_style_text_font(settingsLanguageLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(settingsLanguageLabel_, lv_color_make(30, 41, 59), 0);
  lv_obj_align(settingsLanguageLabel_, LV_ALIGN_TOP_LEFT, 16, 72);

  settingsPowerLabel_ = lv_label_create(settingsPanel_);
  lv_label_set_text(settingsPowerLabel_, "Power meter");
  lv_obj_set_style_text_font(settingsPowerLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(settingsPowerLabel_, lv_color_make(30, 41, 59), 0);
  lv_obj_align(settingsPowerLabel_, LV_ALIGN_TOP_LEFT, 16, 220);
  settingsPowerButton_ = lv_btn_create(settingsPanel_);
  lv_obj_set_size(settingsPowerButton_, 208, 42);
  lv_obj_set_style_radius(settingsPowerButton_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(settingsPowerButton_, lv_color_make(37, 99, 235), 0);
  lv_obj_align(settingsPowerButton_, LV_ALIGN_TOP_LEFT, 16, 252);
  lv_obj_add_event_cb(settingsPowerButton_, onPowerSourceButton,
                      LV_EVENT_CLICKED, nullptr);
  settingsPowerButtonLabel_ = lv_label_create(settingsPowerButton_);
  lv_label_set_text(settingsPowerButtonLabel_, "Select power meter");
  lv_obj_set_style_text_color(settingsPowerButtonLabel_, lv_color_white(), 0);
  lv_obj_set_style_text_font(settingsPowerButtonLabel_, &lv_font_montserrat_14, 0);
  lv_obj_center(settingsPowerButtonLabel_);

  const char *labels[] = {"English", "简体中文"};
  for (size_t i = 0; i < 2; ++i) {
    settingsLanguageButtons_[i] = lv_btn_create(settingsPanel_);
    lv_obj_set_size(settingsLanguageButtons_[i], 208, 42);
    lv_obj_set_style_radius(settingsLanguageButtons_[i], 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(settingsLanguageButtons_[i],
                              lv_color_make(239, 244, 248), 0);
    lv_obj_set_style_border_color(settingsLanguageButtons_[i],
                                  lv_color_make(203, 213, 225), 0);
    lv_obj_set_style_border_width(settingsLanguageButtons_[i], 1, 0);
    lv_obj_align(settingsLanguageButtons_[i], LV_ALIGN_TOP_LEFT, 16,
                 (lv_coord_t)(112 + i * 52));
    lv_obj_add_event_cb(settingsLanguageButtons_[i], onLanguageSelected,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)(i == 1));
    settingsLanguageButtonLabels_[i] = lv_label_create(settingsLanguageButtons_[i]);
    lv_label_set_text(settingsLanguageButtonLabels_[i], labels[i]);
    lv_obj_set_style_text_font(settingsLanguageButtonLabels_[i],
                               i == 1 ? &lv_font_noto_sans_sc_14 : &lv_font_montserrat_14, 0);
    lv_obj_center(settingsLanguageButtonLabels_[i]);
  }

  lv_obj_add_flag(settingsPanel_, LV_OBJ_FLAG_HIDDEN);
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

  lv_label_set_text(settingsTitle_, localized("SETTINGS", "设置"));
  lv_label_set_text(settingsLanguageLabel_, localized("Interface language", "界面语言"));
  lv_label_set_text(settingsPowerLabel_, localized("Power meter", "功率计"));
  lv_label_set_text(settingsPowerButtonLabel_,
                    localized("Select power meter", "选择功率计"));
  lv_obj_set_style_text_font(settingsTitle_, titleFont, 0);
  lv_obj_set_style_text_font(settingsLanguageLabel_, font, 0);
  lv_obj_set_style_text_font(settingsPowerLabel_, font, 0);
  lv_obj_set_style_text_font(settingsPowerButtonLabel_, font, 0);
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

  lv_obj_t *panels[] = {
    powerSourcePanel_, bleDevicePanel_, bleConfirmPanel_,
    bleLoadingPanel_, virtualPowerPanel_, settingsPanel_
  };
  for (lv_obj_t *panel : panels) {
    lv_obj_t *backButton = lv_obj_get_child(panel, 0);
    lv_obj_t *backLabel = lv_obj_get_child(backButton, 0);
    lv_label_set_text(backLabel,
                      localized(LV_SYMBOL_LEFT " BACK", LV_SYMBOL_LEFT " 返回"));
    lv_obj_set_style_text_font(backLabel, font, 0);
  }
}

void DisplayLcd::begin() {
  g_display = this;

  Preferences preferences;
  if (preferences.begin("aero-ui", true)) {
    chinese_ = preferences.getBool("zh", false);
    preferences.end();
  } else {
    Serial.println("[UI] Could not read saved language; using English.");
  }

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  _bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI);
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

  titleLabel_ = lv_label_create(screen_);
  lv_label_set_text(titleLabel_, "AERO PROBE");
  lv_obj_align(titleLabel_, LV_ALIGN_TOP_LEFT, 12, 10);
  lv_obj_set_style_text_font(titleLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(titleLabel_, lv_color_make(29, 41, 64), 0);

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

void DisplayLcd::update(const WindVector &w, float powerW, float cadRpm,
                        float vgMps, float postureDeg, float rho,
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

  snprintf(buf, sizeof(buf), "%.1f km/h", w.vAir * 3.6f);
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
