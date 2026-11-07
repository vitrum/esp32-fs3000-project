#pragma once

#ifdef AERO_HAS_ENV_CONFIG
#include "aero_env_config.h"
#else
#define AERO_ENV_WIFI_SSID ""
#define AERO_ENV_WIFI_PASSWORD ""
#define AERO_ENV_IOT_ENDPOINT ""
#define AERO_ENV_IOT_DEVICE_ID ""
#define AERO_ENV_IOT_USERNAME ""
#define AERO_ENV_IOT_PASSWORD ""
#define AERO_ENV_IOT_TOKEN ""
#define AERO_ENV_OSS_ENDPOINT ""
#define AERO_ENV_OSS_BUCKET ""
#define AERO_ENV_OSS_REGION ""
#define AERO_ENV_OSS_FIT_PREFIX ""
#define AERO_ENV_OSS_SIGN_URL_ENDPOINT ""
#define AERO_ENV_OSS_FIT_UPLOAD_URL ""
#define AERO_ENV_FIRMWARE_MANIFEST_URL ""
#define AERO_ENV_FIRMWARE_DOWNLOAD_URL ""
#define AERO_ENV_FIRMWARE_EXPECTED_SHA256 ""
#endif

namespace EnvSettings {
inline const char *wifiSsid() { return AERO_ENV_WIFI_SSID; }
inline const char *wifiPassword() { return AERO_ENV_WIFI_PASSWORD; }
inline const char *iotEndpoint() { return AERO_ENV_IOT_ENDPOINT; }
inline const char *iotDeviceId() { return AERO_ENV_IOT_DEVICE_ID; }
inline const char *iotUsername() { return AERO_ENV_IOT_USERNAME; }
inline const char *iotPassword() { return AERO_ENV_IOT_PASSWORD; }
inline const char *iotToken() { return AERO_ENV_IOT_TOKEN; }
inline const char *ossEndpoint() { return AERO_ENV_OSS_ENDPOINT; }
inline const char *ossBucket() { return AERO_ENV_OSS_BUCKET; }
inline const char *ossRegion() { return AERO_ENV_OSS_REGION; }
inline const char *ossFitPrefix() { return AERO_ENV_OSS_FIT_PREFIX; }
inline const char *ossSignUrlEndpoint() { return AERO_ENV_OSS_SIGN_URL_ENDPOINT; }
inline const char *ossFitUploadUrl() { return AERO_ENV_OSS_FIT_UPLOAD_URL; }
inline const char *firmwareManifestUrl() { return AERO_ENV_FIRMWARE_MANIFEST_URL; }
inline const char *firmwareDownloadUrl() { return AERO_ENV_FIRMWARE_DOWNLOAD_URL; }
inline const char *firmwareExpectedSha256() { return AERO_ENV_FIRMWARE_EXPECTED_SHA256; }
}