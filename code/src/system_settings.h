#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// 系统设置：时间、声音、语言、USB、WiFi
class SystemSettings {
public:
  // 时区设置
  enum class Timezone : uint8_t {
    UTC_0 = 0,
    UTC_P1 = 1,
    UTC_P2 = 2,
    UTC_P3 = 3,
    UTC_P4 = 4,
    UTC_P5 = 5,
    UTC_P6 = 6,
    UTC_P8_BEIJING = 8,  // 东八区 - 北京
    UTC_P9 = 9,
    UTC_P10 = 10,
    UTC_P12 = 12,
    UTC_N1 = 100,
    UTC_N2 = 101,
    UTC_N5 = 102,
    UTC_N8 = 103,
    UTC_N12 = 104
  };

  // 时区名称和偏移映射
  struct TimezoneInfo {
    Timezone id;
    const char *name;      // 显示名称
    const char *code;      // 时区代码
    int32_t offset;        // UTC 偏移（小时）
  };

  static constexpr TimezoneInfo TIMEZONE_LIST[] = {
    {Timezone::UTC_0, "UTC±0", "UTC", 0},
    {Timezone::UTC_P1, "UTC+1", "CET", 1},
    {Timezone::UTC_P2, "UTC+2", "EET", 2},
    {Timezone::UTC_P3, "UTC+3", "MSK", 3},
    {Timezone::UTC_P4, "UTC+4", "GST", 4},
    {Timezone::UTC_P5, "UTC+5", "PKT", 5},
    {Timezone::UTC_P6, "UTC+6", "BDT", 6},
    {Timezone::UTC_P8_BEIJING, "UTC+8 北京", "CST", 8},
    {Timezone::UTC_P9, "UTC+9", "JST", 9},
    {Timezone::UTC_P10, "UTC+10", "AEST", 10},
    {Timezone::UTC_P12, "UTC+12", "NZST", 12},
    {Timezone::UTC_N1, "UTC-1", "AZOST", -1},
    {Timezone::UTC_N2, "UTC-2", "GST", -2},
    {Timezone::UTC_N5, "UTC-5", "EST", -5},
    {Timezone::UTC_N8, "UTC-8", "PST", -8},
    {Timezone::UTC_N12, "UTC-12", "BIT", -12},
  };

  static constexpr std::size_t TIMEZONE_COUNT = sizeof(TIMEZONE_LIST) / sizeof(TIMEZONE_LIST[0]);

  // 根据时区 ID 获取偏移
  static int32_t getTimezoneOffset(Timezone tz) {
    for (const auto &info : TIMEZONE_LIST) {
      if (info.id == tz) return info.offset;
    }
    return 8;  // 默认北京时间
  }

  // 根据时区 ID 获取名称
  static const char *getTimezoneName(Timezone tz) {
    for (const auto &info : TIMEZONE_LIST) {
      if (info.id == tz) return info.name;
    }
    return "UTC+8 北京";
  }

  // 根据 WiFi IP 自动匹配时区
  // 简单实现：根据 IP 段落大致判断地区
  static Timezone autoDetectTimezone(uint32_t ipAddress) {
    // 提取 IP 的第一个字节作为区域提示
    uint8_t firstOctet = (ipAddress >> 24) & 0xFF;
    
    // 这是一个简化的实现，实际应该使用 GeoIP 数据库
    // 这里仅作为演示，根据 IP 返回对应的时区
    if (firstOctet >= 1 && firstOctet <= 51) {
      // 亚洲 IP 范围（粗略）
      return Timezone::UTC_P8_BEIJING;  // 默认东八区
    } else if (firstOctet >= 52 && firstOctet <= 102) {
      // 欧洲 IP 范围
      return Timezone::UTC_P1;
    } else if (firstOctet >= 103 && firstOctet <= 150) {
      // 美洲 IP 范围
      return Timezone::UTC_N5;
    } else if (firstOctet >= 151 && firstOctet <= 220) {
      // 太平洋 IP 范围
      return Timezone::UTC_P9;
    } else {
      // 默认为北京时间
      return Timezone::UTC_P8_BEIJING;
    }
  }

  // 语言设置
  enum class Language {
    English = 0,
    SimplifiedChinese = 1
  };

  // 音量设置（0-100）
  uint8_t volume = 70;

  // 语言设置
  Language language = Language::English;

  // 时区设置
  Timezone timezone = Timezone::UTC_P8_BEIJING;

  // WiFi 状态
  bool wifiEnabled = false;

  // USB 存储状态
  bool usbStorageEnabled = false;
};

