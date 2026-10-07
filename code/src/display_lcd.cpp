#include "display_lcd.h"
#include "config.h"

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
  if (g_display && g_display->powerSourcePanel_) {
    lv_obj_add_flag(g_display->virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_display->bleConfirmPanel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_display->bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  }
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
                                     ? "Cycling Power + FTMS trainer"
                                     : "Cycling Power")
                              : (g_display->bleDevices_[index].fitnessMachine
                                     ? "FTMS smart trainer"
                                     : "CSC cadence/speed only");
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
    lv_label_set_text(buttonLabel, "WAIT...");
    lv_obj_add_state(g_display->bleLoadingBackButton_, LV_STATE_DISABLED);
    lv_label_set_text(g_display->bleLoadingLabel_,
                      "Cancelling operation...\nPlease wait for BLE to release.");
    return;
  }

  lv_obj_add_flag(g_display->bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(g_display->bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onAntSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 2;
  g_display->selectedPowerMode_ = 2;
  lv_label_set_text(g_display->powerSourceLabel_, "METER: ANT+");
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
  lv_label_set_text(g_display->powerSourceLabel_, "METER: AUTO");
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onCancelSelection(lv_event_t *) {
  if (g_display && g_display->powerSourcePanel_) {
    lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  }
}

void DisplayLcd::buildPanelHeader(lv_obj_t *panel, const char *title,
                                  lv_event_cb_t backCallback) {
  lv_obj_t *backButton = lv_btn_create(panel);
  lv_obj_set_size(backButton, 64, 30);
  lv_obj_set_style_radius(backButton, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(backButton, lv_color_make(40, 43, 50), 0);
  lv_obj_align(backButton, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_add_event_cb(backButton, backCallback, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(backButton);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " BACK");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_14, 0);
  lv_obj_center(backLabel);

  lv_obj_t *titleLabel = lv_label_create(panel);
  lv_label_set_text(titleLabel, title);
  lv_obj_set_width(titleLabel, 142);
  lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(titleLabel, lv_color_make(93, 214, 197), 0);
  lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, 84, 16);

  lv_obj_t *divider = lv_obj_create(panel);
  lv_obj_set_size(divider, 216, 1);
  lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 12, 48);
  lv_obj_set_style_bg_color(divider, lv_color_make(61, 90, 120), 0);
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
  lv_obj_set_style_bg_color(powerSourcePanel_, lv_color_make(8, 15, 25), 0);
  lv_obj_set_style_bg_opa(powerSourcePanel_, LV_OPA_COVER, 0);
  buildPanelHeader(powerSourcePanel_, "POWER SOURCE", onCancelSelection);

  lv_obj_t *hint = lv_label_create(powerSourcePanel_);
  lv_label_set_text(hint, "Choose a power source");
  lv_obj_set_width(hint, 216);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_make(164, 183, 198), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 60);

  const char *labels[] = {"SCAN BLE DEVICES", "ANT+ POWER METER",
                          "VIRTUAL POWER METER", "AUTO SELECT"};
  lv_event_cb_t callbacks[] = {onBleSelected, onAntSelected,
                               onVirtualSelected, onAutoSelected};
  const lv_color_t colors[] = {
    lv_color_make(22, 55, 62), lv_color_make(24, 47, 73),
    lv_color_make(38, 49, 61), lv_color_make(38, 49, 61)
  };
  const lv_coord_t heights[] = {38, 38, 38, 38};
  lv_coord_t y = 98;
  for (size_t i = 0; i < 4; ++i) {
    lv_obj_t *button = lv_btn_create(powerSourcePanel_);
    lv_obj_set_size(button, 208, heights[i]);
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 16, y);
    lv_obj_set_style_bg_color(button, colors[i], 0);
    lv_obj_add_event_cb(button, callbacks[i], LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, labels[i]);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_center(label);
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
  lv_obj_set_style_bg_color(bleDevicePanel_, lv_color_make(8, 15, 25), 0);
  lv_obj_set_style_bg_opa(bleDevicePanel_, LV_OPA_COVER, 0);

  buildPanelHeader(bleDevicePanel_, "BLE DEVICES", onBleBack);

  hint = lv_label_create(bleDevicePanel_);
  lv_label_set_text(hint, "Tap device, then confirm");
  lv_obj_set_width(hint, 216);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_make(164, 183, 198), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 56);

  lv_obj_t *scanButton = lv_btn_create(bleDevicePanel_);
  lv_obj_set_size(scanButton, 216, 32);
  lv_obj_set_style_radius(scanButton, 0, LV_PART_MAIN);
  lv_obj_align(scanButton, LV_ALIGN_TOP_LEFT, 12, 80);
  lv_obj_set_style_bg_color(scanButton, lv_color_make(22, 55, 62), 0);
  lv_obj_add_event_cb(scanButton, onBleScanAgain, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *scanLabel = lv_label_create(scanButton);
  lv_label_set_text(scanLabel, "SCAN AGAIN");
  lv_obj_set_style_text_font(scanLabel, &lv_font_montserrat_14, 0);
  lv_obj_center(scanLabel);

  bleStatusLabel_ = lv_label_create(bleDevicePanel_);
  lv_label_set_text(bleStatusLabel_, "No scan yet");
  lv_obj_set_width(bleStatusLabel_, 216);
  lv_label_set_long_mode(bleStatusLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(bleStatusLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bleStatusLabel_, lv_color_make(255, 209, 102), 0);
  lv_obj_align(bleStatusLabel_, LV_ALIGN_TOP_LEFT, 12, 116);

  for (size_t i = 0; i < 4; ++i) {
    bleDeviceButtons_[i] = lv_btn_create(bleDevicePanel_);
    lv_obj_set_size(bleDeviceButtons_[i], 216, 38);
    lv_obj_set_style_radius(bleDeviceButtons_[i], 0, LV_PART_MAIN);
    lv_obj_align(bleDeviceButtons_[i], LV_ALIGN_TOP_LEFT, 12,
                 (lv_coord_t)(136 + i * 40));
    lv_obj_set_style_bg_color(bleDeviceButtons_[i], lv_color_make(18, 36, 50), 0);
    lv_obj_set_style_pad_all(bleDeviceButtons_[i], 1, 0);
    lv_obj_set_style_border_width(bleDeviceButtons_[i], 0, 0);
    lv_obj_add_event_cb(bleDeviceButtons_[i], onBleDeviceSelected,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    lv_obj_t *deviceLabel = lv_label_create(bleDeviceButtons_[i]);
    lv_obj_set_width(deviceLabel, 196);
    lv_label_set_long_mode(deviceLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(deviceLabel, &lv_font_montserrat_14, 0);
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
  lv_obj_set_style_bg_color(bleConfirmPanel_, lv_color_make(8, 15, 25), 0);
  lv_obj_set_style_bg_opa(bleConfirmPanel_, LV_OPA_COVER, 0);

  buildPanelHeader(bleConfirmPanel_, "CONFIRM DEVICE", onBleConfirmBack);

  bleConfirmNameLabel_ = lv_label_create(bleConfirmPanel_);
  lv_label_set_text(bleConfirmNameLabel_, "BLE device");
  lv_obj_set_width(bleConfirmNameLabel_, 216);
  lv_label_set_long_mode(bleConfirmNameLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(bleConfirmNameLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(bleConfirmNameLabel_, lv_color_white(), 0);
  lv_obj_align(bleConfirmNameLabel_, LV_ALIGN_TOP_LEFT, 12, 76);

  bleConfirmAddressLabel_ = lv_label_create(bleConfirmPanel_);
  lv_label_set_text(bleConfirmAddressLabel_, "");
  lv_obj_set_width(bleConfirmAddressLabel_, 216);
  lv_obj_set_height(bleConfirmAddressLabel_, 44);
  lv_label_set_long_mode(bleConfirmAddressLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(bleConfirmAddressLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bleConfirmAddressLabel_, lv_color_make(164, 183, 198), 0);
  lv_obj_align(bleConfirmAddressLabel_, LV_ALIGN_TOP_LEFT, 12, 104);

  lv_obj_t *confirmButton = lv_btn_create(bleConfirmPanel_);
  lv_obj_set_size(confirmButton, 216, 42);
  lv_obj_set_style_radius(confirmButton, 0, LV_PART_MAIN);
  lv_obj_align(confirmButton, LV_ALIGN_TOP_LEFT, 12, 190);
  lv_obj_set_style_bg_color(confirmButton, lv_color_make(22, 75, 67), 0);
  lv_obj_add_event_cb(confirmButton, onBleConfirm, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *confirmLabel = lv_label_create(confirmButton);
  lv_label_set_text(confirmLabel, "CONNECT");
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
  lv_obj_set_style_bg_color(bleLoadingPanel_, lv_color_make(8, 15, 25), 0);
  lv_obj_set_style_bg_opa(bleLoadingPanel_, LV_OPA_COVER, 0);

  buildPanelHeader(bleLoadingPanel_, "LOADING", onBleLoadingBack);
  bleLoadingBackButton_ = lv_obj_get_child(bleLoadingPanel_, 0);

  bleLoadingTitle_ = lv_obj_get_child(bleLoadingPanel_, 1);
  bleLoadingSpinner_ = lv_spinner_create(bleLoadingPanel_, 900, 60);
  lv_obj_set_size(bleLoadingSpinner_, 42, 42);
  lv_obj_align(bleLoadingSpinner_, LV_ALIGN_TOP_MID, 0, 76);

  bleLoadingLabel_ = lv_label_create(bleLoadingPanel_);
  lv_label_set_text(bleLoadingLabel_, "Waiting...");
  lv_obj_set_width(bleLoadingLabel_, 216);
  lv_obj_set_height(bleLoadingLabel_, 150);
  lv_label_set_long_mode(bleLoadingLabel_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(bleLoadingLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(bleLoadingLabel_, lv_color_make(220, 231, 239), 0);
  lv_obj_align(bleLoadingLabel_, LV_ALIGN_TOP_LEFT, 12, 132);

  lv_obj_add_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
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
  lv_obj_set_style_bg_color(virtualPowerPanel_, lv_color_make(8, 15, 25), 0);
  lv_obj_set_style_bg_opa(virtualPowerPanel_, LV_OPA_COVER, 0);
  buildPanelHeader(virtualPowerPanel_, "VIRTUAL POWER", onVirtualBack);

  lv_obj_t *powerTitle = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(powerTitle, "POWER");
  lv_obj_set_style_text_font(powerTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(powerTitle, lv_color_make(164, 183, 198), 0);
  lv_obj_align(powerTitle, LV_ALIGN_TOP_LEFT, 16, 64);

  virtualPowerValueLabel_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualPowerValueLabel_, "0 W");
  lv_obj_set_style_text_font(virtualPowerValueLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(virtualPowerValueLabel_, lv_color_white(), 0);
  lv_obj_align(virtualPowerValueLabel_, LV_ALIGN_TOP_RIGHT, -16, 62);

  lv_obj_t *powerSlider = lv_slider_create(virtualPowerPanel_);
  lv_obj_set_size(powerSlider, 208, 24);
  lv_slider_set_range(powerSlider, 0, 1500);
  lv_slider_set_value(powerSlider, (int)virtualPowerW_, LV_ANIM_OFF);
  lv_obj_align(powerSlider, LV_ALIGN_TOP_LEFT, 16, 94);
  lv_obj_add_event_cb(powerSlider, onVirtualSliderChanged, LV_EVENT_VALUE_CHANGED,
                      (void *)(uintptr_t)1);

  lv_obj_t *cadenceTitle = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(cadenceTitle, "CADENCE");
  lv_obj_set_style_text_font(cadenceTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(cadenceTitle, lv_color_make(164, 183, 198), 0);
  lv_obj_align(cadenceTitle, LV_ALIGN_TOP_LEFT, 16, 146);

  virtualCadenceValueLabel_ = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(virtualCadenceValueLabel_, "0 rpm");
  lv_obj_set_style_text_font(virtualCadenceValueLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(virtualCadenceValueLabel_, lv_color_white(), 0);
  lv_obj_align(virtualCadenceValueLabel_, LV_ALIGN_TOP_RIGHT, -16, 144);

  lv_obj_t *cadenceSlider = lv_slider_create(virtualPowerPanel_);
  lv_obj_set_size(cadenceSlider, 208, 24);
  lv_slider_set_range(cadenceSlider, 0, 180);
  lv_slider_set_value(cadenceSlider, (int)virtualCadenceRpm_, LV_ANIM_OFF);
  lv_obj_align(cadenceSlider, LV_ALIGN_TOP_LEFT, 16, 176);
  lv_obj_add_event_cb(cadenceSlider, onVirtualSliderChanged, LV_EVENT_VALUE_CHANGED,
                      (void *)(uintptr_t)2);

  lv_obj_t *hint = lv_label_create(virtualPowerPanel_);
  lv_label_set_text(hint, "Drag either slider to update\nlive power and cadence.");
  lv_obj_set_width(hint, 208);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_make(164, 183, 198), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, 224);

  lv_obj_add_flag(virtualPowerPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::begin() {
  g_display = this;

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
  lv_obj_set_style_bg_color(screen_, lv_color_make(9, 15, 25), 0);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);

  titleLabel_ = lv_label_create(screen_);
  lv_label_set_text(titleLabel_, "AERO PROBE");
  lv_obj_align(titleLabel_, LV_ALIGN_TOP_LEFT, 12, 10);
  lv_obj_set_style_text_font(titleLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(titleLabel_, lv_color_make(93, 214, 197), 0);

  lv_obj_t *headerLine = lv_obj_create(screen_);
  lv_obj_set_size(headerLine, 216, 2);
  lv_obj_align(headerLine, LV_ALIGN_TOP_LEFT, 12, 36);
  lv_obj_set_style_bg_color(headerLine, lv_color_make(61, 90, 120), 0);

  lv_obj_t *cdaCard = lv_obj_create(screen_);
  lv_obj_set_size(cdaCard, 216, 74);
  lv_obj_align(cdaCard, LV_ALIGN_TOP_LEFT, 12, 43);
  lv_obj_clear_flag(cdaCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(cdaCard, 0, 0);
  lv_obj_set_style_radius(cdaCard, 0, 0);
  lv_obj_set_style_bg_opa(cdaCard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(cdaCard, 0, 0);

  lv_obj_t *cdaTitle = lv_label_create(cdaCard);
  lv_label_set_text(cdaTitle, "ESTIMATED CdA");
  lv_obj_align(cdaTitle, LV_ALIGN_TOP_LEFT, 8, 3);
  lv_obj_set_style_text_font(cdaTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(cdaTitle, lv_color_make(168, 196, 177), 0);

  cdaLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(cdaLabel_, "-- m2");
  lv_obj_set_width(cdaLabel_, 132);
  lv_label_set_long_mode(cdaLabel_, LV_LABEL_LONG_CLIP);
  lv_obj_align(cdaLabel_, LV_ALIGN_TOP_LEFT, 8, 18);
  lv_obj_set_style_text_font(cdaLabel_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(cdaLabel_, lv_color_make(233, 255, 240), 0);

  postureLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(postureLabel_, "WAITING FOR DATA");
  lv_obj_set_width(postureLabel_, 190);
  lv_label_set_long_mode(postureLabel_, LV_LABEL_LONG_CLIP);
  lv_obj_align(postureLabel_, LV_ALIGN_BOTTOM_LEFT, 8, -1);
  lv_obj_set_style_text_font(postureLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(postureLabel_, lv_color_make(168, 196, 177), 0);

  zoneLabel_ = lv_label_create(cdaCard);
  lv_label_set_text(zoneLabel_, "--");
  lv_obj_align(zoneLabel_, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_text_font(zoneLabel_, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(zoneLabel_, lv_color_make(170, 245, 160), 0);

  lv_obj_t *cdaDivider = lv_obj_create(screen_);
  lv_obj_set_size(cdaDivider, 216, 1);
  lv_obj_align(cdaDivider, LV_ALIGN_TOP_LEFT, 12, 119);
  lv_obj_set_style_bg_color(cdaDivider, lv_color_make(61, 90, 120), 0);
  lv_obj_set_style_border_width(cdaDivider, 0, 0);

  lv_obj_t *windCard = lv_obj_create(screen_);
  lv_obj_set_size(windCard, 216, 56);
  lv_obj_align(windCard, LV_ALIGN_TOP_LEFT, 12, 123);
  lv_obj_clear_flag(windCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(windCard, 0, 0);
  lv_obj_set_style_radius(windCard, 0, 0);
  lv_obj_set_style_bg_opa(windCard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(windCard, 0, 0);

  windLabelTitle_ = lv_label_create(windCard);
  lv_label_set_text(windLabelTitle_, "WIND");
  lv_obj_align(windLabelTitle_, LV_ALIGN_TOP_LEFT, 8, 3);
  lv_obj_set_style_text_font(windLabelTitle_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(windLabelTitle_, lv_color_make(180, 195, 210), 0);

  windLabel_ = lv_label_create(windCard);
  lv_label_set_text(windLabel_, "0.0 km/h");
  lv_obj_align(windLabel_, LV_ALIGN_BOTTOM_LEFT, 8, -2);
  lv_obj_set_style_text_font(windLabel_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(windLabel_, lv_color_make(255, 255, 255), 0);

  lv_obj_t *windDivider = lv_obj_create(screen_);
  lv_obj_set_size(windDivider, 216, 1);
  lv_obj_align(windDivider, LV_ALIGN_TOP_LEFT, 12, 181);
  lv_obj_set_style_bg_color(windDivider, lv_color_make(61, 90, 120), 0);
  lv_obj_set_style_border_width(windDivider, 0, 0);

  lv_obj_t *powerCard = lv_obj_create(screen_);
  lv_obj_set_size(powerCard, 216, 76);
  lv_obj_align(powerCard, LV_ALIGN_TOP_LEFT, 12, 185);
  lv_obj_clear_flag(powerCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(powerCard, 0, 0);
  lv_obj_set_style_radius(powerCard, 0, 0);
  lv_obj_set_style_bg_opa(powerCard, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(powerCard, 0, 0);

  avg3Label_ = lv_label_create(powerCard);
  lv_label_set_text(avg3Label_, "LIVE POWER");
  lv_obj_align(avg3Label_, LV_ALIGN_TOP_LEFT, 8, 5);
  lv_obj_set_style_text_font(avg3Label_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(avg3Label_, lv_color_make(255, 209, 102), 0);

  powerLabel_ = lv_label_create(powerCard);
  lv_label_set_text(powerLabel_, "0 W");
  lv_obj_set_width(powerLabel_, 190);
  lv_label_set_long_mode(powerLabel_, LV_LABEL_LONG_CLIP);
  lv_obj_align(powerLabel_, LV_ALIGN_CENTER, 0, 8);
  lv_obj_set_style_text_align(powerLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(powerLabel_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(powerLabel_, lv_color_make(130, 235, 255), 0);

  lv_obj_t *powerDivider = lv_obj_create(screen_);
  lv_obj_set_size(powerDivider, 216, 1);
  lv_obj_align(powerDivider, LV_ALIGN_TOP_LEFT, 12, 265);
  lv_obj_set_style_bg_color(powerDivider, lv_color_make(61, 90, 120), 0);
  lv_obj_set_style_border_width(powerDivider, 0, 0);

  cadenceLabel_ = lv_label_create(screen_);
  lv_label_set_text(cadenceLabel_, "CADENCE 0 rpm");
  lv_obj_align(cadenceLabel_, LV_ALIGN_TOP_LEFT, 12, 268);
  lv_obj_set_style_text_font(cadenceLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(cadenceLabel_, lv_color_make(255, 255, 255), 0);

  timeLabel_ = lv_label_create(screen_);
  lv_label_set_text(timeLabel_, "00:00:00");
  lv_obj_align(timeLabel_, LV_ALIGN_TOP_RIGHT, -12, 268);
  lv_obj_set_style_text_font(timeLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(timeLabel_, lv_color_make(200, 220, 230), 0);

  statusLabel_ = lv_label_create(screen_);
  lv_label_set_text(statusLabel_, "SD --");
  lv_obj_align(statusLabel_, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_set_style_text_font(statusLabel_, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(statusLabel_, lv_color_make(156, 214, 162), 0);

  powerSourceButton_ = lv_btn_create(screen_);
  lv_obj_set_size(powerSourceButton_, 216, 28);
  lv_obj_align(powerSourceButton_, LV_ALIGN_TOP_LEFT, 12, 289);
  lv_obj_set_style_radius(powerSourceButton_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(powerSourceButton_, lv_color_make(18, 49, 55), 0);
  lv_obj_add_event_cb(powerSourceButton_, onPowerSourceButton, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *powerSourceIcon = lv_label_create(powerSourceButton_);
  lv_label_set_text(powerSourceIcon, LV_SYMBOL_SETTINGS);
  lv_obj_align(powerSourceIcon, LV_ALIGN_LEFT_MID, 8, 0);
  lv_obj_set_style_text_font(powerSourceIcon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(powerSourceIcon, lv_color_make(131, 230, 215), 0);
  powerSourceLabel_ = lv_label_create(powerSourceButton_);
  lv_label_set_text(powerSourceLabel_, "SELECT POWER METER");
  lv_obj_set_width(powerSourceLabel_, 178);
  lv_label_set_long_mode(powerSourceLabel_, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(powerSourceLabel_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(powerSourceLabel_, LV_ALIGN_LEFT_MID, 28, 0);
  lv_obj_set_style_text_font(powerSourceLabel_, &lv_font_montserrat_16, 0);

  buildPowerSourcePanel();
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
  lv_label_set_text(lv_obj_get_child(bleLoadingBackButton_, 0),
                    LV_SYMBOL_LEFT " BACK");
  lv_label_set_text(bleLoadingTitle_, "LOADING");
  lv_label_set_text(bleLoadingLabel_, status ? status : "Waiting for BLE device...");
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
  lv_label_set_text(bleLoadingTitle_, "CONNECTION FAILED");
  lv_label_set_text(bleLoadingLabel_,
                    error ? error : "BLE connection failed without an error description.");
  lv_obj_add_flag(bleLoadingSpinner_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(bleLoadingLabel_, 66);
  lv_obj_set_height(bleLoadingLabel_, 240);
  lv_obj_clear_state(bleLoadingBackButton_, LV_STATE_DISABLED);
  lv_label_set_text(lv_obj_get_child(bleLoadingBackButton_, 0),
                    LV_SYMBOL_LEFT " BACK");
  lv_obj_add_flag(bleDevicePanel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(bleLoadingPanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::setBleDevices(const BlePowerDevice *devices, size_t count,
                               const char *status) {
  bleOperationActive_ = false;
  bleDeviceCount_ = min(count, (size_t)4);
  for (size_t i = 0; i < 4; ++i) {
    lv_obj_t *button = bleDeviceButtons_[i];
    if (i < bleDeviceCount_ && devices) {
      bleDevices_[i] = devices[i];
      char label[64];
      const char *profile = bleDevices_[i].cyclingPower
                                ? (bleDevices_[i].fitnessMachine ? "CPS + FTMS" : "CPS")
                                : (bleDevices_[i].fitnessMachine ? "FTMS trainer" :
                                   (bleDevices_[i].speedCadence ? "CSC cadence only" : "BLE device"));
      snprintf(label, sizeof(label), "%s\n%s  %d dBm",
               bleDevices_[i].name, profile, bleDevices_[i].rssi);
      lv_label_set_text(lv_obj_get_child(button, 0), label);
      lv_obj_clear_flag(button, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (status) {
    lv_label_set_text(bleStatusLabel_, status);
  } else if (bleDeviceCount_ == 0) {
    lv_label_set_text(bleStatusLabel_, "No connectable BLE devices found");
  } else {
    lv_label_set_text(bleStatusLabel_, "Select your power meter");
  }
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

  const char *posture = "WAITING FOR DATA";
  if (zone == 1) posture = "UPRIGHT";
  else if (zone == 2) posture = "LOW / DROPS";
  else if (zone == 3) posture = "AERO";
  else if (zone == 4) posture = "AGGRESSIVE";
  else if (zone == 5) posture = "VERY AERO";
  else if (zone == 6) posture = "RACE TT";
  else if (zone == 7) posture = "ELITE TT";
  if (cda >= 0.0f) {
    snprintf(buf, sizeof(buf), "%.2f m2", cda);
    lv_label_set_text(cdaLabel_, buf);
  } else {
    lv_label_set_text(cdaLabel_, "-- m2");
  }
  lv_label_set_text(postureLabel_, posture);
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

  snprintf(buf, sizeof(buf), "CADENCE %.0f rpm", cadRpm);
  lv_label_set_text(cadenceLabel_, buf);

  const char *srcName = "--";
  lv_color_t srcColor = lv_color_make(200, 220, 230);
  if (pmSrc == 2) {
    srcName = "ANT";
    srcColor = lv_color_make(126, 211, 255);
  } else if (pmSrc == 1) {
    srcName = "BLE";
    srcColor = lv_color_make(142, 235, 171);
  } else if (pmSrc == 3) {
    srcName = "VIRTUAL";
    srcColor = lv_color_make(255, 209, 102);
  }
  if (selectedPowerMode_ == 0) {
    snprintf(buf, sizeof(buf), "PM: AUTO%s", pmSrc == 2 ? " / ANT+" : (pmSrc == 1 ? " / BLE" : " / SCAN"));
  } else if (selectedPowerMode_ == 1) {
    snprintf(buf, sizeof(buf), "PM: BLE%s", pmSrc == 1 ? " / CONNECTED" : " / SCANNING");
  } else if (selectedPowerMode_ == 3) {
    snprintf(buf, sizeof(buf), "PM: VIRTUAL / ACTIVE");
  } else {
    snprintf(buf, sizeof(buf), "PM: ANT+%s", pmSrc == 2 ? " / CONNECTED" : " / SEARCHING");
  }
  lv_label_set_text(powerSourceLabel_, buf);
  lv_obj_set_style_text_color(powerSourceLabel_, srcColor, 0);

  snprintf(buf, sizeof(buf), "Log: %s", logOk ? "OK" : "OFF");
  lv_label_set_text(statusLabel_, buf);
  lv_obj_set_style_text_color(statusLabel_, logOk ? lv_color_make(156, 214, 162) : lv_color_make(255, 170, 126), 0);

  uint32_t now = millis();
  uint32_t sec = now / 1000;
  uint32_t h = (sec / 3600) % 24;
  uint32_t m = (sec / 60) % 60;
  uint32_t s = sec % 60;
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
  lv_label_set_text(timeLabel_, buf);

  lv_obj_invalidate(screen_);
}
