#pragma once

#include "Wire.h"

constexpr int HSPI = 1;
constexpr uint16_t BLACK = 0;
inline uint16_t hostFramebuffer[240 * 320] = {};

class Arduino_DataBus {};
class Arduino_ESP32SPI : public Arduino_DataBus {
public:
  Arduino_ESP32SPI(int, int, int, int, int, int) {}
};

class Arduino_GFX {
public:
  void begin() {}
  void fillScreen(uint16_t color) {
    std::fill(std::begin(hostFramebuffer), std::end(hostFramebuffer), color);
  }
  void draw16bitRGBBitmap(int x, int y, uint16_t *pixels, int width, int height) {
    for (int row = 0; row < height; ++row)
      for (int column = 0; column < width; ++column)
        hostFramebuffer[(y + row) * 240 + x + column] = pixels[row * width + column];
  }
};

class Arduino_ST7789 : public Arduino_GFX {
public:
  Arduino_ST7789(Arduino_DataBus *, int, int, bool) {}
};
