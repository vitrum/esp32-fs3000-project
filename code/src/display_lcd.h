#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "wind_probe.h"

// ST7789 屏（240x320）状态显示：偏航角 / 视风速度 / 功率 / 踏频 / 车速 / 姿态 / 状态
class DisplayLcd {
public:
  void begin();
  void update(const WindVector &w, float powerW, float cadRpm, float vgMps,
              float postureDeg, float rho, int pmSrc, bool logOk);

private:
  Arduino_DataBus *_bus = nullptr;
  Arduino_GFX     *_gfx = nullptr;
};
