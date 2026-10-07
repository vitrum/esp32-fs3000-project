#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "lv_conf.h"
#include <lvgl.h>

#include "ble_power.h"
#include "wind_probe.h"

// ST7789 240x320 display for wind, CdA, live power, cadence, and status.
class DisplayLcd {
public:
  void begin();
  void tick();
  void update(const WindVector &w, float powerW, float cadRpm, float vgMps,
              float postureDeg, float rho, int pmSrc, bool logOk);
  bool takePowerSourceRequest(int &mode);
  float virtualPowerW() const { return virtualPowerW_; }
  float virtualCadenceRpm() const { return virtualCadenceRpm_; }
  bool takeBleScanRequest();
  bool takeBleConnectRequest(char *address, size_t capacity);
  bool takeBleCancelRequest();
  void showBleLoading(const char *status);
  void finishBleCancelled();
  void showBleConnectFailure(const char *error);
  void setBleDevices(const BlePowerDevice *devices, size_t count, const char *status);
  void closePowerMeterPanels();

private:
  Arduino_DataBus *_bus = nullptr;
  Arduino_GFX     *_gfx = nullptr;

  lv_obj_t *screen_ = nullptr;
  lv_obj_t *titleLabel_ = nullptr;
  lv_obj_t *windLabel_ = nullptr;
  lv_obj_t *powerLabel_ = nullptr;
  lv_obj_t *avg3Label_ = nullptr;
  lv_obj_t *cadenceLabel_ = nullptr;
  lv_obj_t *timeLabel_ = nullptr;
  lv_obj_t *statusLabel_ = nullptr;
  lv_obj_t *powerSourceButton_ = nullptr;
  lv_obj_t *powerSourceLabel_ = nullptr;
  lv_obj_t *powerSourcePanel_ = nullptr;
  lv_obj_t *virtualPowerPanel_ = nullptr;
  lv_obj_t *virtualPowerValueLabel_ = nullptr;
  lv_obj_t *virtualCadenceValueLabel_ = nullptr;
  lv_obj_t *bleDevicePanel_ = nullptr;
  lv_obj_t *bleConfirmPanel_ = nullptr;
  lv_obj_t *bleLoadingPanel_ = nullptr;
  lv_obj_t *bleLoadingTitle_ = nullptr;
  lv_obj_t *bleLoadingLabel_ = nullptr;
  lv_obj_t *bleLoadingSpinner_ = nullptr;
  lv_obj_t *bleLoadingBackButton_ = nullptr;
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
  float virtualPowerW_ = 0.0f;
  float virtualCadenceRpm_ = 0.0f;
  int requestedPowerMode_ = -1;
  int selectedPowerMode_ = 0;
  bool requestedBleScan_ = false;
  bool requestedBleConnect_ = false;
  bool requestedBleCancel_ = false;
  bool bleOperationActive_ = false;
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
  static void onBleConfirmBack(lv_event_t *event);
  static void onBleLoadingBack(lv_event_t *event);
  static void onAntSelected(lv_event_t *event);
  static void onAutoSelected(lv_event_t *event);
  static void onVirtualSelected(lv_event_t *event);
  static void onVirtualBack(lv_event_t *event);
  static void onVirtualSliderChanged(lv_event_t *event);
  static void onCancelSelection(lv_event_t *event);
  static uint16_t rgb565FromLvColor(lv_color_t c);
  bool readTouch(uint16_t &x, uint16_t &y);
  bool initTouch();
  bool touchCommand(uint16_t reg);
  bool touchReadRegister(uint16_t reg, uint8_t *data, size_t len);
  bool touchWriteRegister(uint16_t reg, uint8_t value);
  void buildPowerSourcePanel();
  void buildVirtualPowerPanel();
  void buildPanelHeader(lv_obj_t *panel, const char *title, lv_event_cb_t backCallback);
};
