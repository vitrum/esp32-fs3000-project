#include "usb_sd_storage.h"
#include "csv_logger.h"
#include "config.h"
#include "runtime_log.h"
#include <USB.h>
#include <USBMSC.h>
#include <tusb.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>

#if ARDUINO_USB_MODE != 0 || !CONFIG_TINYUSB_MSC_ENABLED
#error USB SD export requires USB OTG mode and TinyUSB MSC support
#endif

namespace {
USBMSC massStorage;
UsbSdExport session;
sdmmc_card_t rawCard{};
uint8_t *dmaSector = nullptr;

bool readSector(void *, uint32_t sector, void *buffer) {
  if (!dmaSector || !lockSdCard(pdMS_TO_TICKS(2000))) return false;
  const bool success = sdmmc_read_sectors(&rawCard, dmaSector, sector, 1) == ESP_OK;
  if (success) memcpy(buffer, dmaSector, 512);
  unlockSdCard();
  return success;
}

bool writeSector(void *, uint32_t sector, const void *buffer) {
  if (!dmaSector || !lockSdCard(pdMS_TO_TICKS(2000))) return false;
  memcpy(dmaSector, buffer, 512);
  const bool success = sdmmc_write_sectors(&rawCard, dmaSector, sector, 1) == ESP_OK;
  unlockSdCard();
  return success;
}

bool beginRawCard() {
  if (!claimSdCardForUsb()) return false;
  spi_bus_config_t bus{};
  bus.mosi_io_num = PIN_SD_MOSI;
  bus.miso_io_num = PIN_SD_MISO;
  bus.sclk_io_num = PIN_SD_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 512;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  if (sdspi_host_init() != ESP_OK) {
    spi_bus_free(SPI2_HOST);
    return false;
  }
  sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
  device.host_id = SPI2_HOST;
  device.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
  sdspi_dev_handle_t handle;
  if (sdspi_host_init_device(&device, &handle) != ESP_OK) {
    spi_bus_free(SPI2_HOST);
    return false;
  }
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = handle;
  host.max_freq_khz = SD_SPI_FREQ_HZ / 1000;
  host.command_timeout_ms = 2000;
  if (sdmmc_card_init(&host, &rawCard) != ESP_OK || rawCard.csd.sector_size != 512) {
    sdspi_host_remove_device(handle);
    spi_bus_free(SPI2_HOST);
    return false;
  }
  dmaSector = static_cast<uint8_t *>(heap_caps_malloc(512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (!dmaSector || !session.attach(rawCard.csd.capacity, rawCard.csd.sector_size, readSector, writeSector, nullptr)) {
    if (dmaSector) heap_caps_free(dmaSector);
    dmaSector = nullptr;
    sdspi_host_remove_device(handle);
    spi_bus_free(SPI2_HOST);
    return false;
  }
  return true;
}

int32_t readBlocks(uint32_t sector, uint32_t offset, void *buffer, uint32_t size) {
  const int32_t result = session.read(sector, offset, buffer, size);
  if (result < 0) {
    tud_msc_set_sense(0, session.senseKey(), session.senseCode(), 0);
    AppLog.printf("[USB-SD] READ failed lba=%lu offset=%lu size=%lu sense=%02x/%02x\n",
                  (unsigned long)sector, (unsigned long)offset, (unsigned long)size,
                  session.senseKey(), session.senseCode());
  }
  return result;
}

int32_t writeBlocks(uint32_t sector, uint32_t offset, uint8_t *buffer, uint32_t size) {
  const int32_t result = session.write(sector, offset, buffer, size);
  if (result < 0) {
    tud_msc_set_sense(0, session.senseKey(), session.senseCode(), 0);
    AppLog.printf("[USB-SD] WRITE failed lba=%lu offset=%lu size=%lu sense=%02x/%02x\n",
                  (unsigned long)sector, (unsigned long)offset, (unsigned long)size,
                  session.senseKey(), session.senseCode());
  }
  return result;
}

bool startStop(uint8_t, bool start, bool loadEject) {
  if (loadEject && !start) {
    session.eject();
    massStorage.mediaPresent(false);
  }
  return !start || !loadEject || session.ready();
}

void usbEventCallback(void *, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
  if (eventBase != ARDUINO_USB_EVENTS) return;
  switch (eventId) {
    case ARDUINO_USB_STARTED_EVENT:
      AppLog.println("[USB] device plugged");
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      AppLog.println("[USB] device unplugged");
      break;
    case ARDUINO_USB_SUSPEND_EVENT: {
      auto *data = static_cast<arduino_usb_event_data_t *>(eventData);
      AppLog.printf("[USB] suspended (remote_wakeup_en=%u)\n",
                    data ? data->suspend.remote_wakeup_en : 0);
      break;
    }
    case ARDUINO_USB_RESUME_EVENT:
      AppLog.println("[USB] resumed");
      break;
    default:
      break;
  }
}
}

bool UsbSdStorage::beginAtBoot() {
  AppLog.println("[USB-SD] firmware=rw-msc-1.2 built=" __DATE__ " " __TIME__);
  massStorage.vendorID("AEROPROB");
  massStorage.productID("AEROPROBE SD");
  massStorage.productRevision("1.2");
  massStorage.onRead(readBlocks);
  massStorage.onWrite(writeBlocks);
  massStorage.onStartStop(startStop);
  massStorage.mediaPresent(false);
  bool enabled = false;
  if (!UsbSdSetting::load(enabled)) AppLog.println("[USB-SD] invalid setting; defaulting to OFF");
  session.selectAtBoot(enabled);
  if (!session.exclusive()) {
    AppLog.println("[USB-SD] manual setting OFF; riding mode (MSC has no medium)");
    return false;
  }
  const bool mediaReady = beginRawCard();
  if (mediaReady && massStorage.begin(session.sectors(), 512)) {
    massStorage.mediaPresent(true);
    AppLog.printf("[USB-SD] exclusive read-write export: %lu sectors\n", (unsigned long)session.sectors());
  } else {
    session.mediaFailed();
    AppLog.println("[USB-SD] SD unavailable; no medium exposed, no format attempted");
  }
  // 启动 TinyUSB 设备栈并挂钩事件（必须在此调用，否则主机看不到任何 USB 设备，
  // 既不会有 CDC 串口也没有 MSC 磁盘）。
  USB.onEvent(usbEventCallback);
  USB.begin();
  AppLog.println("[USB] TinyUSB device stack started");
  return true;
}

bool UsbSdStorage::exclusive() const { return session.exclusive(); }
bool UsbSdStorage::ready() const { return session.ready(); }
bool UsbSdStorage::ejected() const { return session.mode() == UsbSdExport::Mode::Ejected; }
bool UsbSdStorage::hostConnected() const { return tud_mounted(); }
void UsbSdStorage::eject() {
  session.eject();
  massStorage.mediaPresent(false);
}