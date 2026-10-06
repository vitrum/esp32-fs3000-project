#include "display_lcd.h"
#include "config.h"

void DisplayLcd::begin() {
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);   // 背光

  _bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI);
  _gfx = new Arduino_ST7789(_bus, PIN_LCD_RST, 0, true);
  _gfx->begin();
  _gfx->fillScreen(BLACK);

  _gfx->setTextColor(WHITE);
  _gfx->setTextSize(2);
  _gfx->setCursor(20, 16);
  _gfx->println("AERO PROBE");
  _gfx->setTextSize(1);
  _gfx->setCursor(20, 40);
  _gfx->println("FS3000 x2 + BLE PM");
}

void DisplayLcd::update(const WindVector &w, float powerW, float cadRpm,
                        float vgMps, float postureDeg, float rho,
                        int pmSrc, bool logOk) {
  char buf[48];

  // 中部数据区整体重绘（简单可靠，1Hz 无压力）
  _gfx->fillRect(0, 60, 240, 245, BLACK);
  _gfx->setTextSize(2);
  _gfx->setTextColor(WHITE, BLACK);

  _gfx->setCursor(16, 70);
  snprintf(buf, sizeof(buf), "Yaw  %+5.1f deg", w.yawDeg);
  _gfx->println(buf);

  _gfx->setCursor(16, 96);
  snprintf(buf, sizeof(buf), "Vair %5.2f m/s", w.vAir);
  _gfx->println(buf);

  _gfx->setCursor(16, 122);
  snprintf(buf, sizeof(buf), "Pwr  %5.0f W", powerW);
  _gfx->println(buf);

  _gfx->setCursor(16, 148);
  snprintf(buf, sizeof(buf), "Cad  %5.0f rpm", cadRpm);
  _gfx->println(buf);

  _gfx->setCursor(16, 174);
  snprintf(buf, sizeof(buf), "Spd  %5.2f m/s", vgMps);
  _gfx->println(buf);

  _gfx->setCursor(16, 200);
  snprintf(buf, sizeof(buf), "Pos  %+5.1f deg", postureDeg);
  _gfx->println(buf);

  _gfx->setTextSize(1);
  _gfx->setCursor(16, 250);
  snprintf(buf, sizeof(buf), "PM %s  SD %s  rho=%.3f",
           pmSrc == 2 ? "ANT" : (pmSrc == 1 ? "BLE" : "-- "),
           logOk ? "OK " : "-- ", rho);
  _gfx->println(buf);
}
