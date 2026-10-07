#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "lv_conf.h"
#include <lvgl.h>

#include "ble_power.h"
#include "wind_probe.h"

// ST7789 屏（240x320）状态显示：风速 / 温度 / 功率 / 踏频 / 车速 / 姿态 / 状态
class DisplayLcd {
public:
  void begin();
  void tick();
  void update(const WindVector &w, float powerW, float cadRpm, float vgMps,
              float postureDeg, float rho, int pmSrc, bool logOk, float tempC);
  bool takePowerSourceRequest(int &mode);
  bool takeBleScanRequest();
  bool takeBleConnectRequest(char *address, size_t capacity);
  bool takeBleCancelRequest();
  void setBleDevices(const BlePowerDevice *devices, size_t count, const char *status);
  void setBleScanStatus(const char *status);
  void closePowerMeterPanels();

private:
  Arduino_DataBus *_bus = nullptr;
  Arduino_GFX     *_gfx = nullptr;

  lv_obj_t *screen_ = nullptr;
  lv_obj_t *titleLabel_ = nullptr;
  lv_obj_t *windLabel_ = nullptr;
  lv_obj_t *tempLabel_ = nullptr;
  lv_obj_t *powerLabel_ = nullptr;
  lv_obj_t *avg3Label_ = nullptr;
  lv_obj_t *cadenceLabel_ = nullptr;
  lv_obj_t *timeLabel_ = nullptr;
  lv_obj_t *statusLabel_ = nullptr;
  lv_obj_t *powerSourceButton_ = nullptr;
  lv_obj_t *powerSourcePanel_ = nullptr;
  lv_obj_t *bleDevicePanel_ = nullptr;
  lv_obj_t *bleConfirmPanel_ = nullptr;
  lv_obj_t *bleStatusLabel_ = nullptr;
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
  int requestedPowerMode_ = -1;
  int selectedPowerMode_ = 0;
  bool requestedBleScan_ = false;
  bool requestedBleConnect_ = false;
  bool requestedBleCancel_ = false;
  char requestedBleAddress_[18] = {};
  BlePowerDevice bleDevices_[4] = {};
  size_t bleDeviceCount_ = 0;
  size_t selectedBleDevice_ = 0;
  bool touchReady_ = false;

  static void lvglFlushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
  static void lvglTouchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data);
  static void onPowerSourceButton(lv_event_t *event);
  static void onBleSelected(lv_event_t *event);
  static void onBleScanAgain(lv_event_t *event);
  static void onBleDeviceSelected(lv_event_t *event);
  static void onBleConfirm(lv_event_t *event);
  static void onBleBack(lv_event_t *event);
  static void onBleConfirmCancel(lv_event_t *event);
  static void onAntSelected(lv_event_t *event);
  static void onAutoSelected(lv_event_t *event);
  static void onCancelSelection(lv_event_t *event);
  static uint16_t rgb565FromLvColor(lv_color_t c);
  bool readTouch(uint16_t &x, uint16_t &y);
  bool initTouch();
  bool touchCommand(uint16_t reg);
  bool touchReadRegister(uint16_t reg, uint8_t *data, size_t len);
  bool touchWriteRegister(uint16_t reg, uint8_t value);
  void buildPowerSourcePanel();
};
