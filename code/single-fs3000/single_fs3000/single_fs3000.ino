/*
 * single_fs3000.ino — 单 FS3000-1015 风速显示简化版（独立单文件）
 *
 * 用途：验证 rtrobot FS3000-1015 模块接线、模块好坏、简单风速监测。
 *       与双传感器 V 形探针版不同：不测偏航角，只测一路风速并大字显示。
 *
 * 硬件：Waveshare ESP32-S3-Touch-LCD-2.8 + FS3000-1015 × 1
 * 接线：SDA -> GPIO11（板载排针 SDA）
 *       SCL -> GPIO10（板载排针 SCL）
 *       VCC -> 3V3，GND -> GND
 *
 * 编译：
 *   Arduino IDE：安装 ESP32 板包 + "GFX Library for Arduino" 库，打开本文件上传
 *   PlatformIO ：进入 code/single-fs3000 目录，pio run -t upload
 */
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

// ---------------- 引脚 ----------------
#define PIN_I2C_SDA  11
#define PIN_I2C_SCL  10
#define PIN_LCD_MOSI 45
#define PIN_LCD_SCLK 40
#define PIN_LCD_CS   42
#define PIN_LCD_DC   41
#define PIN_LCD_RST  39
#define PIN_LCD_BL   5

#define FS3000_ADDR  0x28      // FS3000 固定 7bit 地址
#define SAMPLE_MS    125       // 数据手册响应时间 125ms

// ---------------- FS3000-1015 官方典型曲线（count -> m/s）----------------
static const float    kMps[] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15};
static const uint16_t kCnt[] = {409, 1203, 1597, 1908, 2187, 2400, 2629,
                                2801, 3006, 3178, 3309, 3563, 3686};
static const int kTableN = sizeof(kMps) / sizeof(kMps[0]);

Arduino_DataBus *bus = nullptr;
Arduino_GFX     *gfx = nullptr;

uint32_t lastSample = 0;
uint32_t errCount = 0;

// 查表 + 线性插值
static float countToMps(uint16_t count) {
  if (count <= kCnt[0]) return 0.0f;
  if (count >= kCnt[kTableN - 1]) return kMps[kTableN - 1];
  for (int i = 1; i < kTableN; i++) {
    if (count <= kCnt[i]) {
      float t = (float)(count - kCnt[i - 1]) / (float)(kCnt[i] - kCnt[i - 1]);
      return kMps[i - 1] + t * (kMps[i] - kMps[i - 1]);
    }
  }
  return 0.0f;
}

// 读 5 字节：校验和 + 12bit 数据；成功返回 true 并输出 m/s 与原始计数
static bool readFs3000(float &mps, uint16_t &raw) {
  Wire.requestFrom(FS3000_ADDR, (uint8_t)5);
  if (Wire.available() < 5) return false;

  uint8_t b[5];
  for (int i = 0; i < 5; i++) b[i] = Wire.read();

  // 5 字节和低 8 位必须为 0
  if (((b[0] + b[1] + b[2] + b[3] + b[4]) & 0xFF) != 0) return false;

  raw = ((b[1] & 0x0F) << 8) | b[2];   // 12 位数据
  mps = countToMps(raw);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI);
  gfx = new Arduino_ST7789(bus, PIN_LCD_RST, 0, true);
  gfx->begin();
  gfx->fillScreen(BLACK);

  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 20);
  gfx->println("FS3000 SINGLE");
  gfx->setTextSize(1);
  gfx->setCursor(20, 48);
  gfx->println("I2C SDA=11 SCL=10");

  // 探测传感器是否在线
  Wire.beginTransmission(FS3000_ADDR);
  bool ok = (Wire.endTransmission() == 0);
  gfx->setCursor(20, 76);
  gfx->println(ok ? "Sensor: OK" : "Sensor: FAIL");
  Serial.println(ok ? "FS3000 found" : "FS3000 NOT found");
}

void loop() {
  if (millis() - lastSample < SAMPLE_MS) return;
  lastSample = millis();

  float mps;
  uint16_t raw;
  bool ok = readFs3000(mps, raw);
  if (!ok) errCount++;

  char buf[40];

  // 大字风速
  gfx->fillRect(0, 120, 240, 60, BLACK);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(4);
  gfx->setCursor(16, 128);
  snprintf(buf, sizeof(buf), "%5.1f", mps * 3.6f);
  gfx->println(buf);
  gfx->setTextSize(1);
  gfx->setCursor(150, 148);
  gfx->println("km/h");

  // 明细行
  gfx->fillRect(0, 200, 240, 60, BLACK);
  gfx->setCursor(16, 208);
  snprintf(buf, sizeof(buf), "raw=%u  err=%lu", ok ? raw : 0, (unsigned long)errCount);
  gfx->println(buf);
  gfx->setCursor(16, 226);
  gfx->println(ok ? "checksum: OK" : "checksum: FAIL");

  // 串口同步输出
  Serial.printf("m/s=%.2f raw=%u ok=%d err=%lu\n", mps, ok ? raw : 0, ok ? 1 : 0,
                (unsigned long)errCount);
}
