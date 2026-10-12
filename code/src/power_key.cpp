#include "power_key.h"
#include "config.h"

// 按键按下为低电平（与官方示例一致：开机时检查 !gpio_get_level(PIN)）
static const uint8_t kActiveLevel = LOW;

void PowerKey::begin() {
  // 电源保持控制脚：输出，默认拉低（不保持，避免异常上电）
  pinMode(PIN_PWR_CONTROL, OUTPUT);
  digitalWrite(PIN_PWR_CONTROL, LOW);

  // 按键输入脚：上拉输入（按下接地为低电平）
  pinMode(PIN_PWR_KEY, INPUT_PULLUP);

  delay(100);  // 等待电平稳定（与官方示例一致）

  // 上电时若检测到电池开关被按下 → 锁定供电保持开机。
  if (digitalRead(PIN_PWR_KEY) == kActiveLevel) {
    digitalWrite(PIN_PWR_CONTROL, HIGH);
    _powerHeld = true;
    Serial.println("[PWR] power key pressed at boot -> power latched ON");
  } else {
    Serial.println("[PWR] boot without key press -> power NOT latched");
  }
}

void PowerKey::tick() {
  if (_shuttingDown) return;

  const bool pressed = (digitalRead(PIN_PWR_KEY) == kActiveLevel);

  if (pressed && !_buttonActive) {
    // 按下沿：记录起始时间，同时确保供电已锁定
    _buttonActive = true;
    _pressStartMs = millis();
    if (!_powerHeld) {
      digitalWrite(PIN_PWR_CONTROL, HIGH);
      _powerHeld = true;
      Serial.println("[PWR] power latched ON (held key)");
    }
  } else if (!pressed && _buttonActive) {
    // 松开沿：仅用于结束计时，不触发关机
    _buttonActive = false;
  }

  if (_buttonActive && !_shuttingDown &&
      millis() - _pressStartMs >= PWR_LONG_PRESS_MS) {
    Serial.printf("[PWR] long press %lums -> shutting down\n",
                  (unsigned long)PWR_LONG_PRESS_MS);
    shutdown();
  }
}

void PowerKey::shutdown() {
  _shuttingDown = true;
  // 拉低电源保持控制 → 切断供电（同时尽快熄灭背光，减少残影）
  digitalWrite(PIN_PWR_CONTROL, LOW);
#if defined(PIN_LCD_BL)
  // 背光脚通常是高电平点亮；拉低熄灭（尽力而为，不保证断电前一定生效）
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);
#endif
  Serial.println("[PWR] power OFF");
  // 断电通常立即生效；若未生效则在此停留，等待硬件掉电。
}
