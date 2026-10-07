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
    lv_obj_clear_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  }
}

void DisplayLcd::onBleSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 1;
  g_display->selectedPowerMode_ = 1;
  lv_label_set_text(lv_obj_get_child(g_display->powerSourceButton_, 0), "METER: BLE");
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onAntSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 2;
  g_display->selectedPowerMode_ = 2;
  lv_label_set_text(lv_obj_get_child(g_display->powerSourceButton_, 0), "METER: ANT+");
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onAutoSelected(lv_event_t *) {
  if (!g_display) return;
  g_display->requestedPowerMode_ = 0;
  g_display->selectedPowerMode_ = 0;
  lv_label_set_text(lv_obj_get_child(g_display->powerSourceButton_, 0), "METER: AUTO");
  lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
}

void DisplayLcd::onCancelSelection(lv_event_t *) {
  if (g_display && g_display->powerSourcePanel_) {
    lv_obj_add_flag(g_display->powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
  }
}

void DisplayLcd::buildPowerSourcePanel() {
  powerSourcePanel_ = lv_obj_create(screen_);
  lv_obj_set_size(powerSourcePanel_, 240, 320);
  lv_obj_center(powerSourcePanel_);
  lv_obj_set_style_radius(powerSourcePanel_, 0, 0);
  lv_obj_set_style_border_width(powerSourcePanel_, 0, 0);
  lv_obj_set_style_bg_color(powerSourcePanel_, lv_color_make(8, 15, 25), 0);
  lv_obj_set_style_bg_opa(powerSourcePanel_, LV_OPA_90, 0);

  lv_obj_t *title = lv_label_create(powerSourcePanel_);
  lv_label_set_text(title, "SELECT POWER METER");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_make(93, 214, 197), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 20);

  lv_obj_t *hint = lv_label_create(powerSourcePanel_);
  lv_label_set_text(hint, "Select connection type");
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(hint, lv_color_make(164, 183, 198), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 14, 52);

  const char *labels[] = {"BLE POWER METER", "ANT+ POWER METER", "AUTO SELECT", "BACK"};
  lv_event_cb_t callbacks[] = {onBleSelected, onAntSelected, onAutoSelected, onCancelSelection};
  const lv_color_t colors[] = {
    lv_color_make(22, 55, 62), lv_color_make(24, 47, 73),
    lv_color_make(38, 49, 61), lv_color_make(40, 43, 50)
  };
  const lv_coord_t heights[] = {48, 48, 44, 38};
  lv_coord_t y = 92;
  for (size_t i = 0; i < 4; ++i) {
    lv_obj_t *button = lv_btn_create(powerSourcePanel_);
    lv_obj_set_size(button, 208, heights[i]);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, 16, y);
    lv_obj_set_style_bg_color(button, colors[i], 0);
    lv_obj_add_event_cb(button, callbacks[i], LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, labels[i]);
    lv_obj_center(label);
    y += heights[i] + 8;
  }
  lv_obj_add_flag(powerSourcePanel_, LV_OBJ_FLAG_HIDDEN);
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

  tempLabel_ = lv_label_create(windCard);
  lv_label_set_text(tempLabel_, "--.- °C");
  lv_obj_align(tempLabel_, LV_ALIGN_RIGHT_MID, -8, 4);
  lv_obj_set_style_text_font(tempLabel_, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(tempLabel_, lv_color_make(127, 210, 255), 0);

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
  lv_label_set_text(avg3Label_, "3 SEC AVG");
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
  lv_obj_set_style_radius(powerSourceButton_, 9, 0);
  lv_obj_set_style_bg_color(powerSourceButton_, lv_color_make(18, 49, 55), 0);
  lv_obj_add_event_cb(powerSourceButton_, onPowerSourceButton, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *powerSourceLabel = lv_label_create(powerSourceButton_);
  lv_label_set_text(powerSourceLabel, "SELECT POWER METER");
  lv_obj_center(powerSourceLabel);
  lv_obj_set_style_text_font(powerSourceLabel, &lv_font_montserrat_16, 0);

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
                        int pmSrc, bool logOk, float tempC) {
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

  snprintf(buf, sizeof(buf), "%.1f °C", tempC);
  lv_label_set_text(tempLabel_, buf);

  snprintf(buf, sizeof(buf), "%.0f W", power3sAvg_);
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
  }
  lv_obj_t *powerSourceLabel = lv_obj_get_child(powerSourceButton_, 0);
  if (selectedPowerMode_ == 0) {
    snprintf(buf, sizeof(buf), "PM: AUTO%s", pmSrc == 2 ? " / ANT+" : (pmSrc == 1 ? " / BLE" : " / SCAN"));
  } else if (selectedPowerMode_ == 1) {
    snprintf(buf, sizeof(buf), "PM: BLE%s", pmSrc == 1 ? " / CONNECTED" : " / SCANNING");
  } else {
    snprintf(buf, sizeof(buf), "PM: ANT+%s", pmSrc == 2 ? " / CONNECTED" : " / SEARCHING");
  }
  lv_label_set_text(powerSourceLabel, buf);
  lv_obj_set_style_text_color(powerSourceLabel, srcColor, 0);

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
