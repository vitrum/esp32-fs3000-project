#pragma once
#include "esp_err.h"
using i2s_port_t = int;
using i2s_mode_t = int;
constexpr int I2S_NUM_0 = 0, I2S_MODE_MASTER = 1, I2S_MODE_TX = 2;
constexpr int I2S_BITS_PER_SAMPLE_16BIT = 16, I2S_CHANNEL_FMT_RIGHT_LEFT = 0;
constexpr int I2S_COMM_FORMAT_STAND_I2S = 0, ESP_INTR_FLAG_LEVEL1 = 1;
struct i2s_config_t {
  int mode, sample_rate, bits_per_sample, channel_format, communication_format;
  int intr_alloc_flags, dma_buf_count, dma_buf_len;
  bool use_apll, tx_desc_auto_clear;
};
inline unsigned hostI2sInstalls = 0;
inline esp_err_t i2s_driver_install(int, const i2s_config_t *, int, void *) { ++hostI2sInstalls; return ESP_OK; }
inline void i2s_driver_uninstall(int) {}