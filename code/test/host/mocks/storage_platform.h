#pragma once
#include "esp_err.h"
#include <filesystem>

using spi_host_device_t = int;
using gpio_num_t = int;
constexpr int SPI2_HOST = 2, SPI_DMA_CH_AUTO = 0;
struct spi_bus_config_t { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; };
struct sdmmc_host_t { int slot = 2, max_freq_khz = 0, command_timeout_ms = 0; };
struct sdspi_device_config_t { int gpio_cs = 0, host_id = 2; };
struct esp_vfs_fat_sdmmc_mount_config_t { bool format_if_mount_failed; int max_files, allocation_unit_size; };
struct sdmmc_card_t { struct { uint32_t capacity = 1024; } csd; };
#define SDSPI_HOST_DEFAULT() sdmmc_host_t{}
#define SDSPI_DEVICE_CONFIG_DEFAULT() sdspi_device_config_t{}
inline int hostSpiResult = ESP_OK, hostMountResult = ESP_OK;
inline bool hostFormatRequested = false;
inline unsigned hostSpiFrees = 0;
inline int spi_bus_initialize(int, const spi_bus_config_t *, int) { return hostSpiResult; }
inline void spi_bus_free(int) { ++hostSpiFrees; }
inline int esp_vfs_fat_sdspi_mount(const char *root, const sdmmc_host_t *, const sdspi_device_config_t *,
                                  const esp_vfs_fat_sdmmc_mount_config_t *config, sdmmc_card_t **card) {
  hostFormatRequested = config->format_if_mount_failed;
  if (hostMountResult != ESP_OK) return hostMountResult;
  std::filesystem::create_directories(root);
  static sdmmc_card_t media;
  *card = &media;
  return ESP_OK;
}