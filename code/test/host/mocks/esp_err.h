#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
inline const char *esp_err_to_name(esp_err_t) { return "host UART error"; }
