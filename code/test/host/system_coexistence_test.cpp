#include "ble_manager.h"
#include "ble_diagnostics.h"
#include "csv_logger.h"
#include "fit_export.h"
#include "fs3000.h"
#include "gps_time.h"
#include "sd_download.h"
#include "wifi_web.h"
#include "storage_platform.h"
#include <esp_heap_caps.h>
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
    web.tick();
    assert(web.connected());
    MDNS.fail = false;
    socket->clients = 1;
    web.broadcastTelemetry(1, NAN, INFINITY, NAN, NAN, false, NAN, NAN, NAN, 1, false);
    assert(socket->frames.back().find("nan") == std::string::npos);
    assert(socket->frames.back().find("inf") == std::string::npos);
    assert(socket->frames.back().find("\"windValid\":false") != std::string::npos);
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

static void storageTests() {
  assert(beginSdCardMutex());
  static CsvLogger logger;
  hostSpiResult = -1;
  assert(!logger.begin());
  hostSpiResult = ESP_OK;
  hostMountResult = -1;
  assert(!logger.begin() && hostSpiFrees == 1 && !hostFormatRequested);
  hostMountResult = ESP_OK;
  assert(logger.begin());
  failTaskCreation = true;
  assert(!logger.start());
  failTaskCreation = false;
  logger.useFallbackUtcReference(1704067200, 0);
  logger.setUtcReference(1792483200, 100);
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

static void recordingFitTests(bool measurements = false) {
  const char *artifactRoot = getenv("FIT_ARTIFACT_DIR");
  assert(artifactRoot && artifactRoot[0]);
  const std::filesystem::path artifacts(artifactRoot);
  std::filesystem::create_directories(artifacts);
  assert(beginSdCardMutex());
  static CsvLogger logger;
  assert(logger.begin());
  logger.log(0, 999, 99, 9, 10, 0, -15, 1.2, 1);
  assert(logger.sampleCount() == 0);
  Preferences::values = {{"ssid", "recording-test"}, {"pass", "password"}};
  assert(wifiWeb.begin(false));
  auto *http = AsyncWebServer::instances.front();
  AsyncWebServerRequest request;
  request.arguments["cmd"] = "start";
  http->dispatch("/api/control", HTTP_POST, request);
  bool start = false;
  assert(request.status == 202 && wifiWeb.takeRecordingRequest(start) && start);
  logger.setLocalDate("2026-10-22");
  std::tm reference = {};
  reference.tm_year = 126;
  reference.tm_mon = 9;
  reference.tm_mday = 22;
  reference.tm_hour = 23;
  reference.tm_min = 59;
  logger.setUtcReference(uint32_t(timegm(&reference)), 10000);
  assert(logger.start() && logger.ok());
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
    if (index == 60) logger.setLocalDate("2026-10-23");
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
    logger.log(10000 + index * 1000, watts, cadence,
               8 + index * 0.1f, windMps,
               measurements ? 0 : -12 + index * 0.2f, -20 + index * 0.1f,
               1.2f + index * 0.0001f, source);
  }
  if (measurements) { inputs.close(); assert(power.droppedPackets() == 0); }
  assert(logger.sampleCount() == 65 && logger.droppedCount() == 0);
  assert(logger.sessionFileCount() == 2);
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
  assert(wifiWeb.setEnabled(false));
  std::cout << "PASS recording FIT: HTTP start, real CSV writer, midnight rollover, async export, retained artifacts, overwrite protection, malformed CSV rejection\n";
}

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "coexistence";
  if (mode == "wifi") wifiTests();
  else if (mode == "storage") storageTests();
  else if (mode == "fit") recordingFitTests();
  else if (mode == "wind_power_fit") recordingFitTests(true);
  else coexistenceTests();
}