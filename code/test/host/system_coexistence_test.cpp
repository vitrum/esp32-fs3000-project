#include "ble_manager.h"
#include "ble_diagnostics.h"
#include "csv_logger.h"
#include "fit_export.h"
#include "recording_journal.h"
#include "ride_recorder.h"
#include "fs3000.h"
#include "gps_time.h"
#include "sd_download.h"
#include "wifi_web.h"
#include "storage_platform.h"
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <driver/uart.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ctime>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;
std::mutex uartMutex;
std::deque<uint8_t> uartBytes;
std::deque<uart_event_t> uartEvents;
bool uartInstalled = false;
int uartCapacity = 0, failUartInit = 0;
bool NimBLEDevice::initialized = false;
std::unique_ptr<NimBLEServer> NimBLEDevice::server;
NimBLEAdvertising NimBLEDevice::advertising;
std::atomic<int> shortWriteBytes{0}, writeFailures{0}, syncFailures{0};

extern "C" ssize_t __real_write(int descriptor, const void *data, size_t length);
extern "C" int __real_fsync(int descriptor);
extern "C" ssize_t __wrap_write(int descriptor, const void *data, size_t length) {
  const int shortBytes = shortWriteBytes.exchange(0);
  if (shortBytes > 0) return __real_write(descriptor, data, std::min(length, size_t(shortBytes)));
  if (writeFailures.load() > 0) { --writeFailures; errno = EIO; return -1; }
  return __real_write(descriptor, data, length);
}
extern "C" int __wrap_fsync(int descriptor) {
  if (syncFailures.load() > 0) { --syncFailures; errno = EIO; return -1; }
  return __real_fsync(descriptor);
}

template <typename Predicate> void await(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

static std::string nmea(const std::string &body) {
  unsigned checksum = 0;
  for (char value : body) checksum ^= uint8_t(value);
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  return "$" + body + suffix;
}

static void feedGps(const std::string &data) {
  std::lock_guard<std::mutex> lock(uartMutex);
  assert(uartInstalled && uartBytes.size() + data.size() <= size_t(uartCapacity));
  uartBytes.insert(uartBytes.end(), data.begin(), data.end());
}

static std::string readFile(const std::string &path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

static void wifiTests() {
  Preferences::values.clear();
  {
    WifiWeb web;
    assert(!web.begin(false) && !web.enabled());
  }
  Preferences::values = {{"ssid", "saved"}, {"pass", "password"}};
  {
    WifiWeb web;
    Preferences::failOpen = true;
    assert(!web.begin(false));
    Preferences::failOpen = false;
  }
  {
    WifiWeb web;
    WiFi.beginResult = WL_CONNECT_FAILED;
    assert(!web.begin(false) && WiFi.modeValue == WIFI_OFF);
    WiFi.beginResult = WL_IDLE_STATUS;
  }
  {
    WifiWeb web;
    WiFi.failMode = true;
    assert(!web.begin(false) && !web.enabled());
    const auto attempts = WiFi.modeCalls;
    web.begin(false);
    assert(WiFi.modeCalls == attempts);
    WiFi.failMode = false;
    clockOffset += 30001;
    assert(web.begin(false) && web.enabled());
    assert(web.setEnabled(false));
  }
  {
    WifiWeb web;
    hostLargestHeap = 1024;
    assert(!web.begin(false) && !web.enabled() && WiFi.modeValue == WIFI_OFF);
    hostLargestHeap = 65536;
  }
  {
    WifiWeb web;
    assert(web.begin(false));
    auto *http = AsyncWebServer::instances.back();
    auto *socket = AsyncWebSocket::instances.back();
    WiFi.state = WL_CONNECTED;
    MDNS.fail = true;
    const auto requestsBeforeConnect = hostNtpRequests;
    web.tick();
    assert(web.connected());
    assert(hostNtpRequests == requestsBeforeConnect + 1 && web.unixSec() == 0);
    assert(hostNtpOffset == 0 && hostNtpDaylightOffset == 0);
    for (const auto &server : hostNtpServers) assert(!server.empty());
    web.tick();
    assert(hostNtpRequests == requestsBeforeConnect + 1 && web.unixSec() == 0);
    hostSntpStatus = SNTP_SYNC_STATUS_COMPLETED;
    web.tick();
    assert(web.unixSec() >= 1577836800);
    MDNS.fail = false;
    socket->clients = 1;
    web.broadcastTelemetry(1, NAN, INFINITY, NAN, NAN, false, NAN, NAN, NAN, 1, false);
    assert(socket->frames.back().find("nan") == std::string::npos);
    assert(socket->frames.back().find("inf") == std::string::npos);
    assert(socket->frames.back().find("\"windValid\":false") != std::string::npos);
    assert(socket->frames.back().find("\"cda\":-1.0000") != std::string::npos);
    web.broadcastTelemetry(2, 250, 90, 8, 10, true, 0, 0, 1.225, 1, true, 0.3125f, 1792483200);
    assert(socket->frames.back().find("\"cda\":0.3125") != std::string::npos);
    assert(socket->frames.back().find("\"utc\":1792483200") != std::string::npos);
    web.broadcastTelemetry(3, 0, 0, 0, 0, true, 0, 0, 1.225, 1, false, -1, 1792483200, true, false, 0, 0);
    assert(socket->frames.back().find("\"armed\":true") != std::string::npos);
    web.broadcastTelemetry(4, 250, 90, 8, 10, true, 0, 0, 1.225, 1, true, .3, 1792483201, false, false, 123000, 2);
    assert(socket->frames.back().find("\"duration\":123000") != std::string::npos);
    for (const char *cmd : {"", "bogus", "source"}) {
      AsyncWebServerRequest request;
      if (*cmd) request.arguments["cmd"] = cmd;
      http->dispatch("/api/control", HTTP_POST, request);
      assert(request.status == 400);
    }
    for (const char *source : {"2", "-1", "4", "1junk"}) {
      AsyncWebServerRequest request;
      request.arguments = {{"cmd", "source"}, {"src", source}};
      http->dispatch("/api/control", HTTP_POST, request);
      assert(request.status == 400);
    }
    for (const char *source : {"0", "1", "3"}) {
      AsyncWebServerRequest request;
      request.arguments = {{"cmd", "source"}, {"src", source}};
      http->dispatch("/api/control", HTTP_POST, request);
      int mode = -1;
      assert(request.status == 202 && web.takeSourceRequest(mode) && mode == atoi(source));
      assert(!web.takeSourceRequest(mode));
    }
    for (const char *cmd : {"start", "stop"}) {
      AsyncWebServerRequest request;
      request.arguments["cmd"] = cmd;
      http->dispatch("/api/control", HTTP_POST, request);
      bool start;
      assert(request.status == 202 && web.takeRecordingRequest(start) && start == (std::string(cmd) == "start"));
      assert(!web.takeRecordingRequest(start));
    }
    for (const char *name : {"../secret.csv", "a/../b.csv", "x\\y.csv", "bad\n.csv", "bad\".csv", "x.txt", ""}) {
      AsyncWebServerRequest request;
      request.arguments["name"] = name;
      http->dispatch("/api/download", HTTP_GET, request);
      assert(request.status == 400);
    }
    AsyncWebServerRequest missing;
    http->dispatch("/api/download", HTTP_GET, missing);
    assert(missing.status == 400);
    AsyncWebServerRequest unknown;
    http->dispatch("/unknown", HTTP_GET, unknown);
    assert(unknown.status == 404);
    WiFi.network = "quoted\"\\\n\tssid";
    AsyncWebServerRequest status;
    http->dispatch("/api/status", HTTP_GET, status);
    assert(status.status == 200 && status.content.find("\\u0009") != std::string::npos);
    const auto frames = socket->frames.size();
    WiFi.state = WL_DISCONNECTED;
    web.tick();
    assert(web.unixSec() >= 1577836800);
    WiFi.state = WL_CONNECTED;
    web.tick();
    assert(hostNtpRequests == requestsBeforeConnect + 2);
    assert(web.unixSec() >= 1577836800);
    WiFi.state = WL_DISCONNECTED;
    web.tick();
    web.broadcastTelemetry(1, 1, 1, 1, 1, true, 1, 1, 1, 1, true);
    assert(socket->frames.size() == frames);
    for (unsigned attempt = 0; attempt < 6; ++attempt) {
      clockOffset += 60001;
      web.tick();
    }
    assert(web.provisioning());
    WiFi.network = "new-network";
    WiFi.password = "new-password";
    WiFi.provisionDone = true;
    web.tick();
    assert(!web.provisioning() && Preferences::values["ssid"] == "new-network");
    WiFi.provisionDone = false;
    assert(web.setEnabled(false));
    assert(web.setEnabled(true));
    web.clearCredentials();
    assert(Preferences::values.empty());
    assert(web.setEnabled(false));
  }
  {
    WifiWeb web;
    assert(web.begin(true) && web.provisioning());
    clockOffset += WIFI_SMART_CONFIG_TIMEOUT_MS;
    web.tick();
    assert(!web.provisioning());
    clockOffset += 5001;
    web.tick();
    assert(web.provisioning());
    WiFi.network = "ephemeral";
    WiFi.password = "password";
    WiFi.provisionDone = true;
    Preferences::failWrite = true;
    web.tick();
    assert(web.ssid() == "ephemeral" && !web.provisioning());
    Preferences::failWrite = false;
    WiFi.provisionDone = false;
    assert(web.setEnabled(false));
  }
  std::cout << "PASS WiFi: lifecycle, startup cooldown, low heap, SmartConfig timeout/retry/persistence, reconnect, mDNS, HTTP validation, telemetry\n";
}

static void journalTests() {
  const auto *check = reinterpret_cast<const uint8_t *>("123456789");
  assert(esp_rom_crc32_le(0, check, 9) == 0xcbf43926);
  hostJournalPresent = false;
  RecordingJournal absent;
  assert(!absent.begin());
  hostJournalPresent = true;
  for (int cut : {0, 1, 30, 59, 60, 61, 63}) {
    std::fill(hostJournalFlash.begin(), hostJournalFlash.end(), 0xff);
    RecordingJournal creating;
    assert(creating.begin());
    hostFlashWriteBudget = cut;
    assert(!creating.start(123));
    hostFlashWriteBudget = -1;
    RecordingJournal afterStartCut;
    assert(afterStartCut.begin() && !afterStartCut.pending());
  }
  std::fill(hostJournalFlash.begin(), hostJournalFlash.end(), 0xff);
  RecordingJournal journal;
  assert(journal.begin() && !journal.pending());
  assert(journal.start(123));
  assert(!journal.start(124));
  RecordingJournal::Reference reference{1792483200, 1000, RecordingJournal::TimeSource::Gps};
  assert(journal.setReference(reference));
  FitSample sample{1000, 0, 90, 8, 0, 0, -15, 1.225f, 1};
  for (unsigned index = 0; index < 65; ++index) {
    sample.milliseconds = 1000 + index * 1000;
    sample.power = index;
    assert(journal.log(sample));
  }
  RecordingJournal reboot;
  assert(reboot.begin() && reboot.pending() && reboot.samples() == 65);
  assert(reboot.reference().utc == reference.utc && reboot.session() == 123);
  unsigned restored = 0;
  for (uint32_t index = 1; index < reboot.records(); ++index) {
    FitSample retained;
    bool isSample;
    assert(reboot.sample(index, retained, isSample));
    if (isSample) {
      assert(retained.milliseconds == 1000 + restored * 1000);
      assert(retained.power == restored && retained.wind == 0 && retained.source == 1);
      ++restored;
    }
  }
  assert(restored == 65);
  for (size_t byte : {size_t(4), size_t(20)}) {
    auto snapshot = hostJournalFlash;
    hostJournalFlash[2 * 64 + byte] ^= 1;
    RecordingJournal corrupted;
    assert(!corrupted.begin() && corrupted.pending());
    hostJournalFlash = snapshot;
  }
  for (int cut : {0, 1, 30, 59, 60, 61, 63}) {
    auto snapshot = hostJournalFlash;
    hostFlashWriteBudget = cut;
    assert(!journal.log(sample));
    hostFlashWriteBudget = -1;
    RecordingJournal interrupted;
    assert(interrupted.begin() && interrupted.samples() == 65);
    assert(interrupted.finish());
    hostJournalFlash = snapshot;
    assert(journal.begin());
  }
  hostFlashReadFailure = true;
  RecordingJournal readFailure;
  assert(!readFailure.begin());
  hostFlashReadFailure = false;
  hostFlashWriteBudget = 1;
  assert(!reboot.finish());
  hostFlashWriteBudget = -1;
  RecordingJournal interruptedAck;
  assert(interruptedAck.begin() && interruptedAck.samples() == 65);
  assert(interruptedAck.finish());
  RecordingJournal completed;
  assert(completed.begin() && !completed.pending());
  assert(completed.start(456) && completed.setReference(reference) && completed.log(sample));
  RecordingJournal reused;
  assert(reused.begin() && reused.samples() == 1 && reused.session() == 456);
  assert(reused.finish());
  {
    auto snapshot = hostJournalFlash;
    hostJournalPartition.size = 8192;
    RecordingJournal bounded;
    assert(bounded.begin() && bounded.start(999) && bounded.setReference(reference));
    for (unsigned index = 0; index < 126; ++index) assert(bounded.log(sample));
    assert(!bounded.log(sample) && bounded.samples() == 126);
    RecordingJournal full;
    assert(full.begin() && full.samples() == 126 && full.finish());
    hostJournalPartition.size = hostJournalFlash.size();
    hostJournalFlash = snapshot;
  }
  std::cout << "PASS journal: CRC, reboot replay, torn records, interrupted acknowledgement, session reuse\n";
}

static void storageTests() {
  journalTests();
  assert(beginSdCardMutex());
  SdFileEntry unavailableEntries[1];
  int unavailableError = 0;
  assert(listSdFiles("/", unavailableEntries, 1, &unavailableError) == 0);
  assert(unavailableError == ENODEV);
  static CsvLogger logger;
  hostSpiResult = -1;
  assert(!logger.begin());
  hostSpiResult = ESP_OK;
  hostMountResult = -1;
  assert(!logger.begin() && hostSpiFrees == 1 && !hostFormatRequested);
  hostMountResult = ESP_OK;
  failTaskCreation = true;
  assert(!logger.begin() && sdCardReady());
  sdmmc_command_t command;
  command.opcode = MMC_SEND_STATUS;
  hostStatusTimeouts = 2;
  assert(hostMountedConfig.do_transaction(hostMountedConfig.slot, &command) == ESP_OK);
  assert(hostTransactionCalls == 3);
  hostTransactionCalls = 0;
  hostStatusTimeouts = 4;
  assert(hostMountedConfig.do_transaction(hostMountedConfig.slot, &command) == ESP_ERR_TIMEOUT);
  assert(hostTransactionCalls == 3 && hostStatusTimeouts == 1);
  hostTransactionCalls = 0;
  command.opcode = 24;
  assert(hostMountedConfig.do_transaction(hostMountedConfig.slot, &command) == ESP_ERR_TIMEOUT);
  assert(hostTransactionCalls == 1);
  command.opcode = MMC_SEND_STATUS;
  hostTransactionCalls = 0;
  hostTransactionResult = -1;
  assert(hostMountedConfig.do_transaction(hostMountedConfig.slot, &command) == -1);
  assert(hostTransactionCalls == 1);
  hostTransactionResult = ESP_OK;
  assert(!logger.start());
  failTaskCreation = false;
  assert(logger.begin() && !logger.ok());
  {
    RecordingJournal journal;
    FitExport exporter;
    RideRecorder controller(logger, exporter, journal);
    assert(controller.begin() && controller.requestStart() && controller.waiting());
    assert(controller.requestStart());
    FitSample sample{10000, 0, 90, 0, 0, 0, -15, 1.225f, 1};
    assert(!controller.accept(sample, false, true, 789));
    assert(controller.waiting() && controller.duration(20000) == 0);
    assert(!controller.accept(sample, true, false, 789));
    controller.requestStop(20000);
    assert(!controller.waiting() && !journal.pending());
  }
  const std::string logDirectory = std::string(sdMountPoint()) + "/LOG";
  std::filesystem::create_directories(logDirectory);
  const std::string conflictPath = logDirectory + "/conflict";
  std::ofstream(conflictPath) << "not a directory";
  logger.setLocalDate("conflict");
  assert(!logger.start() && !logger.ok());
  assert(readFile(conflictPath) == "not a directory");
  std::filesystem::remove(conflictPath);
  logger.setLocalDate("");
  failTaskCreation = true;
  assert(logger.start());
  assert(logger.stop());
  failTaskCreation = false;
  logger.useFallbackUtcReference(1704067200, 0);
  logger.setNetworkUtcReference(0, 50);
  uint32_t networkUtc, networkMs;
  assert(logger.utcReference(networkUtc, networkMs) && networkUtc == 1704067200);
  logger.setNetworkUtcReference(1792483100, 75);
  assert(logger.utcReference(networkUtc, networkMs) && networkUtc == 1792483100 && networkMs == 75);
  assert(!logger.utcReferenceFromGps());
  assert(logger.start() && logger.stop());
  assert(logger.utcReference(networkUtc, networkMs) && networkUtc == 1792483100);
  logger.setNetworkUtcReference(1792483150, 90);
  assert(logger.utcReference(networkUtc, networkMs) && networkUtc == 1792483100 && networkMs == 75);
  logger.setUtcReference(1792483200, 100);
  logger.setNetworkUtcReference(1792483300, 110);
  assert(logger.start());
  uint32_t utc, milliseconds;
  assert(logger.utcReference(utc, milliseconds) && utc == 1792483200 && milliseconds == 100);
  logger.useFallbackUtcReference(1704067200, 0);
  assert(logger.utcReferenceFromGps());
  logger.pause();
  for (unsigned index = 0; index < 70; ++index)
    logger.log(index * 1000, 250, 90, 10, 11, 0, -15, 1.225, 1);
  assert(logger.sampleCount() == 60 && logger.droppedCount() == 10);
  logger.resume();
  logger.log(70000, 250, 90, 10, 11, 0, -15, 1.225, 1);
  const std::string first = logger.fileName();
  logger.setLocalDate("2026-10-22");
  assert(logger.sessionFileCount() == 2);
  logger.log(71000, 251, 91, 10, 11, 0, -15, 1.225, 1);
  assert(logger.stop() && !logger.ok());
  const auto csv = readFile(std::string(sdMountPoint()) + first);
  assert(std::count(csv.begin(), csv.end(), '\n') == 62);
  assert(logger.start());
  const std::string next = logger.fileName();
  assert(logger.stop());
  assert(logger.start() && std::string(logger.fileName()) != next);
  assert(lockSdCard(0));
  for (unsigned index = 0; index < 125; ++index)
    logger.log(index * 1000, 200, 80, 8, 9, 0, -15, 1.2, 1);
  assert(logger.sampleCount() == 120 && logger.droppedCount() == 5);
  unlockSdCard();
  assert(logger.stop());
  const auto recovered = readFile(std::string(sdMountPoint()) + logger.fileName());
  assert(std::count(recovered.begin(), recovered.end(), '\n') == 121);
  assert(logger.start());
  for (unsigned index = 1; index <= 9; ++index) {
    char date[12];
    snprintf(date, sizeof(date), "2026-11-%02u", index);
    logger.setLocalDate(date);
  }
  assert(!logger.sessionComplete() && logger.sessionFileCount() == CsvLogger::kSessionFiles);
  assert(logger.stop());
  syncFailures = 1;
  assert(!logger.start() && !logger.ok());
  assert(syncFailures == 0);
  for (bool syncFailure : {false, true}) {
    assert(logger.start());
    if (syncFailure) syncFailures = 1;
    else { shortWriteBytes = 17; writeFailures = 1; }
    for (unsigned index = 0; index < 60; ++index)
      logger.log(index * 1000, 200, 80, 8, 9, 0, -15, 1.2, 1);
    assert(logger.stop() && writeFailures == 0 && syncFailures == 0);
    const auto retained = readFile(std::string(sdMountPoint()) + logger.fileName());
    assert(std::count(retained.begin(), retained.end(), '\n') == 61);
    assert(retained.find("59000,200.0") != std::string::npos);
  }
  std::filesystem::create_directory(std::string(sdMountPoint()) + "/browse");
  std::filesystem::create_directory(std::string(sdMountPoint()) + "/browse/folder");
  std::ofstream(std::string(sdMountPoint()) + "/browse/plain.txt") << "text";
  std::ofstream(std::string(sdMountPoint()) + "/browse/song.MP3") << "audio";
  SdFileEntry entries[3];
  assert(listSdFiles("/../", entries, 3) == 0);
  assert(listSdFiles("/browse", entries, 3) == 3);
  assert(entries[0].isDirectory && entries[1].isAudio && !entries[2].isAudio);
  int readError = -1;
  assert(listSdFiles("/../", entries, 3, &readError) == 0 && readError == EINVAL);
  assert(listSdFiles("/browse", nullptr, 3, &readError) == 0 && readError == EINVAL);
  assert(listSdFiles("/browse", entries, 0, &readError) == 0 && readError == EINVAL);
  assert(lockSdCard(0));
  assert(listSdFiles("/browse", entries, 3, &readError) == 0 && readError == EBUSY);
  unlockSdCard();
  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    assert(listSdFiles("/browse/folder", entries, 3, &readError) == 0 && readError == 0);
    assert(listSdFiles("/browse", entries, 3, &readError) == 3 && readError == 0);
    assert(std::string(entries[0].name) == "folder");
    assert(std::string(entries[1].name) == "song.MP3");
    assert(std::string(entries[2].name) == "plain.txt");
  }
  std::filesystem::create_symlink("missing-target", std::string(sdMountPoint()) + "/browse/broken");
  assert(listSdFiles("/browse", entries, 3, &readError) == 3 && readError == ENOENT);
  std::filesystem::remove(std::string(sdMountPoint()) + "/browse/broken");
  assert(listSdFiles("/browse", entries, 3, &readError) == 3 && readError == 0);
  assert(listSdFiles("/missing", entries, 3, &readError) == 0 && readError == ENOENT);
  const std::string largeDirectory = std::string(sdMountPoint()) + "/large-list";
  std::filesystem::create_directory(largeDirectory);
  for (unsigned index = 0; index < 4; ++index)
    std::ofstream(largeDirectory + "/file" + std::to_string(index)) << "data";
  assert(listSdFiles("/large-list", entries, 3, &readError) == 3 && readError == EOVERFLOW);
  std::filesystem::create_symlink("missing-target", largeDirectory + "/broken");
  assert(listSdFiles("/large-list", entries, 3, &readError) == 3 && readError == ENOENT);
  const std::string limitedDirectory = std::string(sdMountPoint()) + "/scan-limit";
  std::filesystem::create_directory(limitedDirectory);
  for (unsigned index = 0; index < 398; ++index)
    std::ofstream(limitedDirectory + "/.hidden" + std::to_string(index)) << "data";
  assert(listSdFiles("/scan-limit", entries, 3, &readError) == 0 && readError == 0);
  std::ofstream(limitedDirectory + "/.overflow") << "data";
  assert(listSdFiles("/scan-limit", entries, 3, &readError) == 0 && readError == EOVERFLOW);
  std::cout << "PASS CSV storage: mount/task failures, UTC precedence, pause/overflow, rollover, unique files, writer backlog, short write/fsync recovery, session limits, file browsing\n";
}

static void coexistenceTests() {
  Preferences::values = {{"ssid", "test-network"}, {"pass", "test-password"}};
  assert(beginSdCardMutex());
  static CsvLogger logger;
  assert(logger.begin() && logger.start());
  assert(wifiWeb.begin(false));
  WiFi.state = WL_CONNECTED;
  wifiWeb.tick();
  assert(wifiWeb.connected());
  auto *http = AsyncWebServer::instances.front();
  auto *socket = AsyncWebSocket::instances.front();
  socket->clients = 1;
  GpsTime gps;
  assert(gps.begin());
  BlePowerMeter power;
  BlePosture posture;
  BleManager manager(power, posture);
  assert(manager.begin());
  assert(manager.enqueue(BleOpKind::Connect, "11:22:33:44:55:66", 1));
  BleOutcome outcome;
  await([&] { return manager.takeOutcome(outcome); });
  assert(outcome.success && power.connected());
  auto *measurement = &NimBLEDevice::clients.front()->service.measurement;
  NimBLEDevice::scan.manualCompletion = true;
  assert(manager.enqueue(BleOpKind::Scan));
  await([] { return NimBLEDevice::scan.isScanning(); });
  std::atomic<unsigned> requestCount{0};
  std::thread requests([&] {
    for (unsigned index = 0; index < 64; ++index) {
      AsyncWebServerRequest request;
      request.arguments = {{"cmd", "source"}, {"src", index & 1 ? "1" : "3"}};
      http->dispatch("/api/control", HTTP_POST, request);
      assert(request.status == 202);
      ++requestCount;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  const std::string fix = nmea("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,191026,003.1,W");
  for (unsigned index = 0; index < 64; ++index) {
    feedGps(fix);
    measurement->emit({0, 0, uint8_t(200 + index), 0});
    manager.tick();
    wifiWeb.broadcastTelemetry(millis(), power.power(), 90, 10, 11, true, 0, -15, 1.225, 1, true);
    logger.log(index * 1000, power.power(), 90, 10, 11, 0, -15, 1.225, 1);
    int requestedSource = -1;
    if (wifiWeb.takeSourceRequest(requestedSource)) assert(requestedSource == 1 || requestedSource == 3);
    AsyncWebServerRequest status;
    http->dispatch("/api/status", HTTP_GET, status);
    assert(status.status == 200 && status.content.find("\"connected\":true") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  requests.join();
  assert(requestCount == 64);
  await([&] { gps.tick(); return gps.sentencesSeen() == 64; });
  assert(gps.hasFix() && gps.overflowCount() == 0);
  assert(socket->frames.size() == 64 && power.notificationCount() == 64);
  assert(manager.busy() && NimBLEDevice::scan.isScanning());
  AsyncWebServerRequest control;
  control.arguments = {{"cmd", "stop"}};
  http->dispatch("/api/control", HTTP_POST, control);
  bool start = true;
  assert(control.status == 202 && wifiWeb.takeRecordingRequest(start) && !start);
  assert(logger.stop());
  const std::string csvPath = std::string(sdMountPoint()) + logger.fileName();
  const auto csv = readFile(csvPath);
  assert(std::count(csv.begin(), csv.end(), '\n') == 65);
  assert(logger.sampleCount() == 64 && logger.droppedCount() == 0);
  logger.setUtcReference(gps.unixSec(), millis());
  assert(logger.utcReferenceFromGps());
  std::ofstream(std::string(sdMountPoint()) + "/download.csv") << csv;
  AsyncWebServerRequest records;
  http->dispatch("/api/records", HTTP_GET, records);
  assert(records.status == 200 && records.content.find("download.csv") != std::string::npos);
  assert(logger.start());
  AsyncWebServerRequest download;
  download.arguments = {{"name", "download.csv"}};
  http->dispatch("/api/download", HTTP_GET, download);
  assert(download.status == 200 && download.response);
  auto *response = dynamic_cast<SdDownloadResponse *>(download.response.get());
  assert(response);
  assert(lockSdCard(0));
  uint8_t bytes[2048];
  assert(response->_fillBuffer(bytes, sizeof(bytes)) == RESPONSE_TRY_AGAIN);
  feedGps(fix);
  measurement->emit({0, 0, 250, 0});
  manager.tick();
  AsyncWebServerRequest source;
  source.arguments = {{"cmd", "source"}, {"src", "3"}};
  http->dispatch("/api/control", HTTP_POST, source);
  int mode = -1;
  assert(source.status == 202 && wifiWeb.takeSourceRequest(mode) && mode == 3);
  unlockSdCard();
  std::string downloaded;
  unsigned downloadSamples = 0;
  while (downloaded.size() < csv.size()) {
    const size_t length = response->_fillBuffer(bytes, sizeof(bytes));
    if (length != RESPONSE_TRY_AGAIN) {
      assert(length > 0 && length <= 1024);
      downloaded.append(reinterpret_cast<char *>(bytes), length);
    }
    clockOffset += 20;
    feedGps(fix);
    measurement->emit({0, 0, 250, 0});
    logger.log(downloadSamples++ * 1000, 250, 90, 10, 11, 0, -15, 1.225, 1);
    manager.tick();
    wifiWeb.tick();
  }
  assert(downloaded == csv);
  assert(logger.stop() && logger.sampleCount() == downloadSamples);
  const auto duringDownload = readFile(std::string(sdMountPoint()) + logger.fileName());
  assert(std::count(duringDownload.begin(), duringDownload.end(), '\n') == downloadSamples + 1);
  download.response.reset();
  AsyncWebServerRequest absent;
  absent.arguments["name"] = "missing.csv";
  http->dispatch("/api/download", HTTP_GET, absent);
  assert(absent.status == 404);
  assert(lockSdCard(0));
  AsyncWebServerRequest busy;
  busy.arguments["name"] = "download.csv";
  http->dispatch("/api/download", HTTP_GET, busy);
  assert(busy.status == 503);
  unlockSdCard();
  AsyncWebServerRequest again;
  again.arguments["name"] = "download.csv";
  http->dispatch("/api/download", HTTP_GET, again);
  assert(again.status == 200);
  AsyncWebServerRequest competing;
  competing.arguments["name"] = "download.csv";
  http->dispatch("/api/download", HTTP_GET, competing);
  assert(competing.status == 503);
  again.response.reset();
  WiFi.state = WL_DISCONNECTED;
  wifiWeb.tick();
  assert(!wifiWeb.connected() && power.connected());
  clockOffset += WIFI_RECONNECT_MS;
  wifiWeb.tick();
  assert(WiFi.reconnectCalls > 0);
  WiFi.state = WL_CONNECTED;
  wifiWeb.tick();
  assert(wifiWeb.connected());
  manager.cancel();
  await([&] { return manager.takeOutcome(outcome); });
  assert(outcome.cancelled);
  await([&] { gps.tick(); return gps.sentencesSeen() == 65 + downloadSamples; });
  assert(gps.overflowCount() == 0 && logger.droppedCount() == 0);
  assert(manager.suspend());
  gps.stop();
  assert(wifiWeb.setEnabled(false));
  std::cout << "PASS coexistence: GPS+BLE+WiFi+SD, scanning, telemetry, recording, download lock contention, reconnect, cancellation\n" << std::flush;
}

static void recordingFitTests(bool measurements = false, bool gpsInput = false,
                              bool fallbackTime = false) {
  const bool staged = gpsInput || fallbackTime;
  const char *artifactRoot = getenv("FIT_ARTIFACT_DIR");
  assert(artifactRoot && artifactRoot[0]);
  const std::filesystem::path artifacts(artifactRoot);
  std::filesystem::create_directories(artifacts);
  assert(beginSdCardMutex());
  static CsvLogger logger;
  assert(logger.begin());
  logger.log(0, 999, 99, 9, 10, 0, -15, 1.2, 1);
  assert(logger.sampleCount() == 0);
  if (!staged) {
    Preferences::values = {{"ssid", "recording-test"}, {"pass", "password"}};
    assert(wifiWeb.begin(false));
    auto *http = AsyncWebServer::instances.front();
    AsyncWebServerRequest request;
    request.arguments["cmd"] = "start";
    http->dispatch("/api/control", HTTP_POST, request);
    bool start = false;
    assert(request.status == 202 && wifiWeb.takeRecordingRequest(start) && start);
  } else {
    assert(!wifiWeb.enabled());
  }
  if (!staged) logger.setLocalDate("2026-10-22");
  std::tm reference = {};
  reference.tm_year = 126;
  reference.tm_mon = 9;
  reference.tm_mday = 22;
  reference.tm_hour = 23;
  reference.tm_min = 59;
  GpsTime gps;
  std::ofstream gpsInputs;
  if (gpsInput) {
    assert(gps.begin());
    gpsInputs.open(artifacts / "gps-input.nmea");
    const auto initial = nmea("GPRMC,235900,A,4807.038,N,01131.000,E,022.4,084.4,221026,003.1,W");
    gpsInputs << initial;
    feedGps(initial);
    await([&] { gps.tick(); return gps.hasFix(); });
    assert(gps.unixSec() == uint32_t(timegm(&reference)));
    logger.setUtcReference(gps.unixSec(), 10000);
    char local[24];
    gps.formatLocalDateTime(local, sizeof(local), 8 * 60);
    char date[12];
    snprintf(date, sizeof(date), "%.10s", local);
    logger.setLocalDate(date);
  } else if (!fallbackTime) {
    if (measurements) logger.setUtcReference(uint32_t(timegm(&reference)), 10000);
    else {
      logger.setNetworkUtcReference(uint32_t(timegm(&reference)), 10000);
      assert(!logger.utcReferenceFromGps());
    }
  }
  assert(logger.start() && logger.ok());
  if (fallbackTime) {
    assert(!gps.enabled());
    logger.useFallbackUtcReference(FIT_DEFAULT_UTC, 10000);
    assert(!logger.utcReferenceFromGps());
    assert(std::string(logger.fileName()).find("/LOG/fs_") == 0);
  }
  if (gpsInput) {
    assert(logger.utcReferenceFromGps());
    assert(std::string(logger.fileName()).find("/LOG/2026-10-23/") == 0);
  }
  assert(logger.start() && logger.sessionFileCount() == 1);
  TwoWire windBus;
  FS3000 wind;
  BlePowerMeter power;
  BlePosture posture;
  BleManager manager(power, posture);
  std::ofstream inputs;
  if (measurements) {
    assert(wind.begin(windBus));
    assert(manager.begin());
    assert(manager.enqueue(BleOpKind::Connect, "11:22:33:44:55:66", 1));
    BleOutcome outcome;
    await([&] { return manager.takeOutcome(outcome); });
    assert(outcome.success && power.connected());
    inputs.open(artifacts / "sensor-input.csv");
    inputs << "index,raw_count,packet_power_w,protocol\n";
  }
  const uint8_t sources[] = {0, 1, 3};
  const uint16_t rawCounts[] = {0, 409, 806, 1203, 1597, 1908, 2400, 2629, 3686, 4095};
  const int16_t packetPower[] = {0, 1, 255, 256, 2000, 32767, -25, 1000};
  for (unsigned index = 0; index < 65; ++index) {
    if (index == 60 && !staged) logger.setLocalDate("2026-10-23");
    if (gpsInput) {
      if (index > 0) clockOffset += 1000;
      if (index > 0 && (index < 20 || index >= 25)) {
        if (index == 12) {
          const std::string broken = "$GPRMC,broken*00\r\n";
          gpsInputs << broken;
          feedGps(broken);
        }
        char body[128];
        snprintf(body, sizeof(body),
                 "GPRMC,%02u%02u%02u,A,4807.038,N,01131.000,E,022.4,084.4,%s,003.1,W",
                 index < 60 ? 23u : 0u, index < 60 ? 59u : 0u,
                 index % 60, index < 60 ? "221026" : "231026");
        const auto sentence = nmea(body);
        gpsInputs << sentence;
        const auto previous = gps.sentencesSeen();
        feedGps(sentence);
        await([&] { gps.tick(); return gps.sentencesSeen() > previous; });
        assert(gps.unixSec() == uint32_t(timegm(&reference)) + index);
      }
      gps.tick();
      gps.pollPresence();
      assert(gps.hasFix() == !(index >= 22 && index <= 24));
      assert(gps.modulePresent() == gps.hasFix());
    }
    float windMps = 10 + index * 0.05f;
    float watts = 200 + index;
    float cadence = 80 + index % 10;
    uint8_t source = sources[index % 3];
    if (measurements) {
      const uint16_t raw = rawCounts[index % 10];
      windBus.setCount(raw);
      if (index == 16) {
        ++windBus.frame[0];
        windMps = 42;
        assert(!wind.readMps(windMps) && windMps == 42);
        windBus.setCount(raw);
      }
      assert(wind.readMps(windMps));
      if (index == 30) NimBLEDevice::clients.front()->disconnect();
      if (index == 32) {
        NimBLEDevice::nextService = "0x1826";
        NimBLEDevice::nextCharacteristic = "0x2ad2";
        assert(manager.enqueue(BleOpKind::Connect, "22:33:44:55:66:77", 0));
        BleOutcome outcome;
        await([&] { return manager.takeOutcome(outcome); });
        assert(outcome.success && power.connected());
      }
      const int16_t packet = packetPower[index % 8];
      if (power.connected()) {
        auto &measurement = NimBLEDevice::clients.front()->service.measurement;
        const uint8_t flags = index < 32 ? 0 : 0x41;
        if (index == 24 || index == 48) {
          const float before = power.power();
          const auto count = power.notificationCount();
          measurement.emit({flags, 0, 0xFF});
          manager.tick();
          assert(power.power() == before && power.notificationCount() == count);
        }
        measurement.emit({flags, 0, uint8_t(packet), uint8_t(uint16_t(packet) >> 8)});
      }
      manager.tick();
      watts = power.power();
      cadence = power.cadence();
      source = power.connected() ? 1 : 0;
      assert(watts == (source ? packet : 0));
      inputs << index << ',' << raw << ',' << packet << ',' << (index < 32 ? "CPS" : "FTMS") << '\n';
    }
    float groundSpeed = staged ? power.speed() : 8 + index * 0.1f;
    if (gpsInput && gps.hasFix()) groundSpeed = gps.speedMps();
    logger.log(10000 + index * 1000, watts, cadence,
           groundSpeed, windMps,
               measurements ? 0 : -12 + index * 0.2f, -20 + index * 0.1f,
               1.2f + index * 0.0001f, source);
  }
  if (measurements) { inputs.close(); assert(power.droppedPackets() == 0); }
  assert(logger.sampleCount() == 65 && logger.droppedCount() == 0);
  assert(logger.sessionFileCount() == (staged ? 1 : 2));
  if (gpsInput) {
    assert(gps.sentencesSeen() == 60 && gps.failedChecksum() == 1);
    assert(gps.overflowCount() == 0);
    gpsInputs.close();
    gps.stop();
  }
  FitExport exporter;
  assert(exporter.start(logger, true));
  assert(!exporter.start(logger, true));
  bool success = false;
  char result[200];
  await([&] { return exporter.takeResult(success, result, sizeof(result)); });
  assert(success && !logger.ok() && !exporter.busy());
  assert(!exporter.takeResult(success, result, sizeof(result)));
  auto fitPath = std::filesystem::path(sdMountPoint()) / (logger.sessionFile(0) + 1);
  fitPath.replace_extension(".fit");
  const std::string original = readFile(fitPath);
  assert(!original.empty() && !std::filesystem::exists(fitPath.string() + ".tmp"));
  std::filesystem::copy_file(fitPath, artifacts / "recording.fit");
  for (size_t index = 0; index < logger.sessionFileCount(); ++index) {
    const auto source = std::filesystem::path(sdMountPoint()) / (logger.sessionFile(index) + 1);
    std::filesystem::copy_file(source, artifacts / ("segment-" + std::to_string(index) + ".csv"));
  }
  assert(exporter.start(logger, true));
  await([&] { return exporter.takeResult(success, result, sizeof(result)); });
  assert(!success && std::string(result).find("already exists") != std::string::npos);
  assert(readFile(fitPath) == original);
  assert(logger.start());
  logger.log(80000, 300, 90, 10, 11, 0, -15, 1.225, 1);
  assert(logger.stop());
  auto invalidCsv = std::filesystem::path(sdMountPoint()) / (logger.fileName() + 1);
  std::ofstream(invalidCsv, std::ios::app) << "broken,record\n";
  auto invalidFit = invalidCsv;
  invalidFit.replace_extension(".fit");
  assert(exporter.start(logger, true));
  await([&] { return exporter.takeResult(success, result, sizeof(result)); });
  assert(!success && std::filesystem::exists(invalidCsv));
  assert(!std::filesystem::exists(invalidFit) && !std::filesystem::exists(invalidFit.string() + ".tmp"));
  if (measurements) assert(manager.suspend());
  if (!staged) assert(wifiWeb.setEnabled(false));
  else assert(!wifiWeb.enabled());
  std::cout << "PASS recording FIT: "
            << (gpsInput ? "stage2 NMEA+BLE+wind, GPS loss/recovery, " :
                fallbackTime ? "stage1 BLE+wind, fallback UTC, " : "HTTP start, midnight rollover, ")
            << "real CSV writer, async export, retained artifacts, overwrite protection, malformed CSV rejection\n";
}

static void recordingLifecycleTests() {
  const char *root = getenv("FIT_ARTIFACT_DIR");
  assert(root && beginSdCardMutex());
  const std::filesystem::path artifacts(root);
  static CsvLogger interruptedLogger;
  assert(interruptedLogger.begin());
  {
    RecordingJournal missing;
    FitExport unused;
    RideRecorder unavailable(interruptedLogger, unused, missing);
    hostJournalPresent = false;
    assert(!unavailable.begin());
    unavailable.requestShutdown(0);
    assert(unavailable.readyToPowerOff());
    hostJournalPresent = true;
  }
  RecordingJournal interruptedJournal;
  FitExport interruptedExporter;
  RideRecorder interrupted(interruptedLogger, interruptedExporter, interruptedJournal);
  assert(interrupted.begin() && interrupted.requestStart());
  interruptedLogger.setUtcReference(1792483200, 10000);
  FitSample sample{9000, 0, 80, 0, 0, 0, -15, 1.225f, 1};
  assert(!interrupted.accept(sample, false, true, 123));
  assert(interrupted.waiting() && interruptedLogger.sampleCount() == 0);
  const uint32_t times[] = {10000, 11000, 14000, 15000};
  const float watts[] = {0, 250, 300, 400};
  const float winds[] = {0, 10, 12, 15};
  const float speeds[] = {0, 8, 10, 11};
  for (unsigned index = 0; index < 4; ++index) {
    sample.milliseconds = times[index];
    sample.power = watts[index]; sample.wind = winds[index]; sample.speed = speeds[index];
    sample.cadence = 80 + index;
    assert(interrupted.accept(sample, true, true, 123));
    assert(interrupted.requestStart());
    if (index == 1) {
      sample.milliseconds = 12000;
      assert(!interrupted.accept(sample, false, true, 123));
      sample.milliseconds = 13000;
      assert(!interrupted.accept(sample, true, false, 123));
    }
  }
  assert(interrupted.recording() && interrupted.duration(15000) == 5000);
  assert(interrupted.skipped() == 2 && interruptedLogger.sampleCount() == 4);
  const auto originalCsv = readFile(std::string(sdMountPoint()) + interruptedLogger.fileName());
  assert(std::count(originalCsv.begin(), originalCsv.end(), '\n') == 1);
  hostFlashWriteBudget = 61;
  assert(!interruptedJournal.log(sample));
  hostFlashWriteBudget = -1;

  static CsvLogger recoveredLogger;
  assert(recoveredLogger.begin());
  RecordingJournal recoveredJournal;
  FitExport recoveredExporter;
  RideRecorder recovered(recoveredLogger, recoveredExporter, recoveredJournal);
  assert(recovered.begin() && recovered.restoring());
  assert(recoveredJournal.damagedTail() && recoveredJournal.samples() == 4);
  assert(!recovered.requestStart());
  syncFailures = 1;
  await([&] { recovered.poll(millis()); return !recovered.busy(); });
  assert(recoveredJournal.pending() && !recovered.readyToPowerOff());
  assert(recovered.requestSave(millis()));
  await([&] { recovered.poll(millis()); return !recovered.busy(); });
  assert(!recoveredJournal.pending() && recoveredLogger.sampleCount() == 4);
  assert(recoveredLogger.droppedCount() == 0 && recovered.duration(millis()) == 5000);
  auto recoveredCsv = std::filesystem::path(sdMountPoint()) / (recoveredLogger.fileName() + 1);
  auto recoveredFit = recoveredCsv; recoveredFit.replace_extension(".fit");
  assert(std::filesystem::exists(recoveredFit));
  std::filesystem::copy_file(recoveredCsv, artifacts / "recovered.csv");
  std::filesystem::copy_file(recoveredFit, artifacts / "recovered.fit");

  assert(recovered.requestStart());
  recoveredLogger.setUtcReference(1792483300, 20000);
  sample = FitSample{20000, 250, 90, 8, 10, 0, -15, 1.225f, 1};
  assert(recovered.accept(sample, true, true, 456));
  sample.milliseconds = 21000; sample.power = 300; sample.speed = 10; sample.wind = 12;
  assert(recovered.accept(sample, true, true, 456));
  auto shutdownCsv = std::filesystem::path(sdMountPoint()) / (recoveredLogger.fileName() + 1);
  auto shutdownFit = shutdownCsv; shutdownFit.replace_extension(".fit");
  recovered.requestShutdown(21000);
  assert(!recovered.readyToPowerOff() && recovered.busy());
  hostFlashWriteBudget = 0;
  await([&] { recovered.poll(millis()); return !recoveredExporter.busy() && std::filesystem::exists(shutdownFit); });
  recovered.poll(millis());
  assert(recoveredJournal.pending() && !recovered.readyToPowerOff());
  hostFlashWriteBudget = -1;
  recovered.poll(millis() + 1001);
  assert(recovered.readyToPowerOff() && !recoveredJournal.pending());
  assert(recovered.duration(99999) == 1000);
  std::filesystem::copy_file(shutdownCsv, artifacts / "shutdown.csv");
  std::filesystem::copy_file(shutdownFit, artifacts / "shutdown.fit");
  assert(!recovered.requestStart());
  static CsvLogger batchLogger;
  assert(batchLogger.begin());
  RecordingJournal batchJournal;
  FitExport batchExporter;
  RideRecorder batch(batchLogger, batchExporter, batchJournal);
  assert(batch.begin() && batch.requestStart());
  batchLogger.setUtcReference(1792483400, 60000);
  for (unsigned index = 0; index < 65; ++index) {
    sample = FitSample{60000 + index * 1000, 200.0f + index, 90, 8, 10, 0, -15, 1.225f, 1};
    assert(batch.accept(sample, true, true, 999));
  }
  static CsvLogger batchRestoredLogger;
  assert(batchRestoredLogger.begin());
  RecordingJournal batchRestoredJournal;
  FitExport batchRestoredExporter;
  RideRecorder batchRestored(batchRestoredLogger, batchRestoredExporter, batchRestoredJournal);
  assert(batchRestored.begin() && batchRestored.restoring());
  await([&] { batchRestored.poll(millis()); return !batchRestored.busy(); });
  assert(!batchRestoredJournal.pending() && batchRestoredLogger.sampleCount() == 65);
  assert(batchRestoredLogger.droppedCount() == 0);
  auto batchCsv = std::filesystem::path(sdMountPoint()) / (batchRestoredLogger.fileName() + 1);
  auto batchFit = batchCsv; batchFit.replace_extension(".fit");
  std::filesystem::copy_file(batchCsv, artifacts / "batch.csv");
  std::filesystem::copy_file(batchFit, artifacts / "batch.fit");
  std::cout << "PASS recording lifecycle: validity gate, zero values, duplicate start, duration, power-loss replay, SD retry, shutdown FIT and acknowledgement\n";
}

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "coexistence";
  if (mode == "wifi") wifiTests();
  else if (mode == "storage") storageTests();
  else if (mode == "fit") recordingFitTests();
  else if (mode == "wind_power_fit") recordingFitTests(true);
  else if (mode == "stage1_ble_wind_fit") recordingFitTests(true, false, true);
  else if (mode == "stage2_gps_ble_wind_fit") recordingFitTests(true, true);
  else if (mode == "recording_lifecycle") recordingLifecycleTests();
  else coexistenceTests();
}