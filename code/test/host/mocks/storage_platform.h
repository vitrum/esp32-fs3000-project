#pragma once
#include "esp_err.h"
#include "driver/sdmmc_defs.h"
#include <filesystem>

using spi_host_device_t = int;
using gpio_num_t = int;
constexpr int SPI2_HOST = 2, SPI_DMA_CH_AUTO = 0;
struct spi_bus_config_t { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; };
constexpr int ESP_ERR_TIMEOUT = 0x107;
struct sdmmc_command_t { int opcode = 0; };
struct sdmmc_host_t {
  int slot = 2, max_freq_khz = 0, command_timeout_ms = 0;
  esp_err_t (*do_transaction)(int, sdmmc_command_t *) = nullptr;
};
struct sdspi_device_config_t { int gpio_cs = 0, host_id = 2; };
struct esp_vfs_fat_sdmmc_mount_config_t { bool format_if_mount_failed; int max_files, allocation_unit_size; };
struct sdmmc_card_t { struct { uint32_t capacity = 1024; } csd; };
#define SDSPI_HOST_DEFAULT() sdmmc_host_t{}
#define SDSPI_DEVICE_CONFIG_DEFAULT() sdspi_device_config_t{}
inline int hostSpiResult = ESP_OK, hostMountResult = ESP_OK;
inline bool hostFormatRequested = false;
inline unsigned hostSpiFrees = 0;
inline sdmmc_host_t hostMountedConfig;
inline unsigned hostTransactionCalls = 0, hostStatusTimeouts = 0;
inline int hostTransactionResult = ESP_OK;
inline esp_err_t sdspi_host_do_transaction(int, sdmmc_command_t *) {
  ++hostTransactionCalls;
  if (hostStatusTimeouts > 0) {
    --hostStatusTimeouts;
    return ESP_ERR_TIMEOUT;
  }
  return hostTransactionResult;
}
inline int spi_bus_initialize(int, const spi_bus_config_t *, int) { return hostSpiResult; }
inline void spi_bus_free(int) { ++hostSpiFrees; }
inline int esp_vfs_fat_sdspi_mount(const char *root, const sdmmc_host_t *host, const sdspi_device_config_t *,
                                  const esp_vfs_fat_sdmmc_mount_config_t *config, sdmmc_card_t **card) {
  hostFormatRequested = config->format_if_mount_failed;
  hostMountedConfig = *host;
  if (hostMountResult != ESP_OK) return hostMountResult;
  std::filesystem::create_directories(root);
  static sdmmc_card_t media;
  *card = &media;
  return ESP_OK;
}