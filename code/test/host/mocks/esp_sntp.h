#pragma once

#include <string>

enum sntp_sync_status_t { SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_COMPLETED };
inline sntp_sync_status_t hostSntpStatus = SNTP_SYNC_STATUS_RESET;
inline unsigned hostNtpRequests = 0;
inline std::string hostNtpServers[3];
inline long hostNtpOffset = 0;
inline int hostNtpDaylightOffset = 0;

inline void configTime(long offset, int daylight, const char *first,
                       const char *second, const char *third) {
  ++hostNtpRequests;
  hostNtpOffset = offset;
  hostNtpDaylightOffset = daylight;
  hostNtpServers[0] = first;
  hostNtpServers[1] = second;
  hostNtpServers[2] = third;
  hostSntpStatus = SNTP_SYNC_STATUS_RESET;
}

inline sntp_sync_status_t sntp_get_sync_status() { return hostSntpStatus; }