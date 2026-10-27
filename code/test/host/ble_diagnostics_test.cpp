#include "runtime_log.h"
#include "ble_manager.h"
#include "saved_connections.h"
#include "ble_inbox.h"
#include "ble_heart_rate.h"
#include <NimBLEDevice.h>
#include <cassert>
#include <iostream>
#include <thread>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;
bool NimBLEDevice::initialized = false;
std::unique_ptr<NimBLEServer> NimBLEDevice::server;
NimBLEAdvertising NimBLEDevice::advertising;

static void bufferTests() {
  LogBuffer<8> ring;
  const uint8_t bytes[] = "abcdefghijkl";
  ring.append(bytes, 6);
  uint64_t cursor = 0, lost = 0;
  uint8_t output[32] = {};
  assert(ring.read(cursor, output, 3, lost) == 3 && !lost);
  assert(std::string(reinterpret_cast<char *>(output), 3) == "abc");
  ring.append(bytes + 6, 6);
  assert(ring.oldest() == 4 && ring.end() == 12);
  assert(ring.read(cursor, output, 32, lost) == 8 && lost == 1);
  assert(std::string(reinterpret_cast<char *>(output), 8) == "efghijkl");
  assert(ring.read(cursor, output, 32, lost) == 0 && !lost);
  const auto serialBefore = Serial.messages.load();
  AppLog.println("boot replay marker");
  assert(Serial.messages == serialBefore);
  Serial.writable = 0;
  AppLog.flushSerial();
  assert(Serial.messages == serialBefore);
  Serial.writable = 128;
  AppLog.flushSerial();
  assert(Serial.messages > serialBefore);
  uint8_t packet[] = {0x00, 0xFF, 0x12, 0xAB};
  const auto before = AppLog.end();
  AppLog.packet("CPS/2A63", packet, sizeof(packet));
  assert(AppLog.end() == before);
  AppLog.setRaw(true);
  const auto rawSerialBefore = Serial.messages.load();
  AppLog.packet("CPS/2A63", packet, sizeof(packet));
  assert(AppLog.end() > before && Serial.messages == rawSerialBefore);
  cursor = before;
  uint8_t text[180] = {};
  size_t length = AppLog.read(cursor, text, sizeof(text), lost);
  assert(std::string(reinterpret_cast<char *>(text), length).find("len=4 off=0 hex=00FF12AB") != std::string::npos);
  AppLog.setRaw(false);
  std::string huge(1200, 'X');
  AppLog.printf("%s", huge.c_str());
  assert(AppLog.truncated() == 1);
  cursor = AppLog.oldest();
  uint8_t all[RuntimeLog::kCapacity];
  length = AppLog.read(cursor, all, sizeof(all), lost);
  assert(std::string(reinterpret_cast<char *>(all), length).find("[TRUNCATED]") != std::string::npos);
  std::thread a([] { for (int i = 0; i < 100; ++i) AppLog.println("producer A"); });
  std::thread b([] { for (int i = 0; i < 100; ++i) AppLog.println("producer B"); });
  a.join(); b.join();
  assert(AppLog.end() > 0);
  AppLog.flushSerial();
  assert(AppLog.serialLostBytes() > 0);
  AppLog.setRaw(true);
  cursor = AppLog.end();
  std::vector<uint8_t> largePacket(513, 0xAB);
  AppLog.packet("POSTURE/FFE1", largePacket.data(), largePacket.size());
  length = AppLog.read(cursor, all, sizeof(all), lost);
  const std::string raw(reinterpret_cast<char *>(all), length);
  assert(raw.find("len=513 off=480 hex=") != std::string::npos);
  assert(raw.find("off=512") == std::string::npos);
  assert(raw.find("TRUNCATED at 512 bytes") != std::string::npos);
  assert(AppLog.truncated() == 2);
  cursor = AppLog.end();
  AppLog.packet("CSC/2A5B", nullptr, 0);
  length = AppLog.read(cursor, all, sizeof(all), lost);
  assert(std::string(reinterpret_cast<char *>(all), length).find("len=0 EMPTY") != std::string::npos);
  AppLog.setRaw(false);
}

static void scannerTests() {
  auto &scan = NimBLEDevice::scan;
  assert(!bleScanner.run(1, true));
  assert(bleScanner.state() == BleScanState::Failed);
  NimBLEDevice::initialized = true;
  scan.failStart = true;
  assert(!bleScanner.run(1, true));
  assert(std::string(bleScanner.error()).find("start rejected") != std::string::npos);
  scan.failStart = false;
  bleScanner.setConnected(false);
  assert(bleScanner.run(1, true));
  assert(bleScanner.state() == BleScanState::Complete && scan.results.getCount() == 0);
  assert(scan.activeScan && scan.window == 60 && scan.interval == 100);
  bleScanner.setConnected(true);
  assert(bleScanner.run(1, false));
  assert(!scan.activeScan && scan.window == 30);
  bleScanner.setConnected(false);
  assert(bleScanner.run(1, false));
  assert(scan.window == 20);
  scan.manualCompletion = true;
  scan.startHook = [&] { scan.complete(6); };
  assert(!bleScanner.run(1, true));
  assert(std::string(bleScanner.error()).find("error 6") != std::string::npos);
  scan.startHook = [] { clockOffset += 2100; };
  assert(!bleScanner.run(1, true));
  assert(std::string(bleScanner.error()).find("timed out") != std::string::npos);
  assert(!scan.isScanning());
  std::atomic<uint32_t> generation{0};
  BleCancellation cancellation(generation, 0);
  scan.startHook = [&] { ++generation; };
  assert(!bleScanner.run(1, true, &cancellation));
  assert(bleScanner.state() == BleScanState::Cancelled && !scan.isScanning());
  const auto starts = scan.starts.load();
  assert(!bleScanner.run(1, true, &cancellation));
  assert(scan.starts == starts);
  BleCancellation nextCancellation(generation, generation.load());
  scan.failStop = true;
  assert(!bleScanner.run(1, true, &nextCancellation));
  assert(bleScanner.state() == BleScanState::Failed && scan.isScanning());
  assert(!bleScanner.run(1, true));
  scan.failStop = false;
  scan.stop();
  const auto offset = clockOffset.load();
  clockOffset += UINT32_MAX - millis() - 10;
  scan.startHook = [&] { clockOffset += 50; scan.complete(BLE_HS_EDONE); };
  assert(bleScanner.run(100, true));
  assert(bleScanner.elapsedMs() >= 50 && bleScanner.elapsedMs() < 1000);
  clockOffset = offset;
  scan.startHook = {};
  scan.manualCompletion = false;
  NimBLEDevice::initialized = false;
  std::cout << "PASS BLE scanner: start failure, empty success, end error, deadline, cancellation, stop failure, timer wrap, adaptive windows\n";
}

static void inboxTests() {
  BleInbox<2, 4> inbox;
  uint8_t bytes[] = {1, 2, 3, 4, 5};
  assert(!inbox.push(0, nullptr, 1));
  assert(!inbox.push(0, bytes, 5));
  assert(inbox.push(1, bytes, 4));
  bytes[0] = 9;
  assert(inbox.push(2, bytes, 1));
  assert(!inbox.push(3, bytes, 1));
  BleInbox<2, 4>::Packet packet;
  assert(inbox.pop(packet) && packet.kind == 1 && packet.data[0] == 1);
  assert(inbox.pop(packet) && packet.kind == 2 && packet.data[0] == 9);
  assert(!inbox.pop(packet));
  assert(inbox.push(0, bytes, 1));
  inbox.reset();
  assert(!inbox.pop(packet) && inbox.generation() == 1 && inbox.dropped() == 3);
  std::atomic<bool> finished{false};
  std::thread producer([&] {
    for (unsigned index = 0; index < 10000; ++index) inbox.push(1, bytes, 4);
    finished = true;
  });
  unsigned received = 0;
  while (!finished.load()) {
    if (inbox.pop(packet)) { assert(packet.data[0] == 9); ++received; }
    else std::this_thread::yield();
  }
  producer.join();
  while (inbox.pop(packet)) ++received;
  assert(received + inbox.dropped() - 3 == 10000);
  std::cout << "PASS BLE inbox: owned copies, FIFO, bounds, overflow, generation reset, concurrent producer/consumer\n";
}

static BleOutcome outcome(BleManager &manager) {
  BleOutcome result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!manager.takeOutcome(result)) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
  return result;
}

static void controlTests() {
  BlePowerMeter power;
  BlePosture posture;
  BleHeartRate heartSensor;
  BleManager manager(power, posture, &heartSensor);
  assert(!manager.enqueue(BleOpKind::Scan));
  failTaskCreation = true;
  assert(!manager.begin());
  failTaskCreation = false;
  assert(manager.begin() && manager.available());
  NimBLEDevice::scan.failStart = true;
  assert(manager.enqueue(BleOpKind::Scan));
  assert(!manager.enqueue(BleOpKind::Posture));
  const auto failed = outcome(manager);
  assert(!failed.success && !failed.cancelled && failed.error[0]);
  NimBLEDevice::scan.failStart = false;
  assert(manager.enqueue(BleOpKind::Scan));
  const auto empty = outcome(manager);
  assert(empty.success && empty.deviceCount == 0 && !manager.busy());
  NimBLEDevice::nextService = "0x180d";
  NimBLEDevice::nextCharacteristic = "0x2a37";
  assert(manager.enqueue(BleOpKind::Heart, "44:55:66:77:88:99", 1));
  assert(outcome(manager).success && heartSensor.connected());
  auto *heartClient = NimBLEDevice::clients.back();
  heartClient->service.measurement.emit({0, 165});
  assert(!heartSensor.valid(millis()));
  manager.tick();
  assert(heartSensor.valid(millis()) && heartSensor.bpm() == 165);
  assert(!heartSensor.valid(millis() + 5001));
  heartClient->service.measurement.emit({1, 44});
  manager.tick();
  assert(heartSensor.bpm() == 165);
  heartClient->service.measurement.emit({4, 165});
  manager.tick();
  assert(!heartSensor.valid(millis()));
  heartClient->service.measurement.emit({0, 150});
  heartClient->disconnect();
  manager.tick();
  assert(!heartSensor.valid(millis()));
  heartSensor.disconnect();
  NimBLEDevice::nextService = "0x1818";
  NimBLEDevice::nextCharacteristic = "0x2a63";
  assert(manager.enqueue(BleOpKind::Connect, "11:22:33:44:55:66", 1));
  assert(outcome(manager).success && power.connected());
  SavedConnections saved;
  assert(saved.load() && saved.bleCount() == 2);
  assert(!strcmp(saved.ble(0)->address, "11:22:33:44:55:66") && saved.ble(0)->addressType == 1);
  manager.tick();
  auto *measurement = &NimBLEDevice::clients.front()->service.measurement;
  assert(!power.measurementValid(millis()));
  const auto notifications = power.notificationCount();
  AppLog.setRaw(true);
  const auto logEnd = AppLog.end();
  measurement->emit({0, 0, 250, 0});
  assert(power.notificationCount() == notifications && AppLog.end() == logEnd);
  manager.tick();
  assert(power.power() == 250 && power.notificationCount() == notifications + 1);
  assert(power.measurementValid(millis()));
  assert(!power.measurementValid(millis() + 3001));
  assert(AppLog.end() > logEnd);
  AppLog.setRaw(false);
  for (int16_t watts : {int16_t(0), int16_t(1), int16_t(255), int16_t(256),
                        int16_t(2000), int16_t(32767), int16_t(-25), int16_t(-32768)}) {
    const auto before = power.notificationCount();
    measurement->emit({0, 0, uint8_t(watts), uint8_t(uint16_t(watts) >> 8)});
    manager.tick();
    assert(power.power() == watts && power.notificationCount() == before + 1);
    for (size_t length = 1; length < 4; ++length) {
      measurement->emit(std::vector<uint8_t>(length, 0xFF));
      manager.tick();
      assert(power.power() == watts && power.notificationCount() == before + 1);
    }
  }
  for (unsigned index = 0; index < 20; ++index) measurement->emit({0, 0, 200, 0});
  assert(power.droppedPackets() == 8);
  manager.tick();
  measurement->emit({0, 0, 111, 0});
  NimBLEDevice::clients.front()->disconnect();
  manager.tick();
  assert(!power.connected() && power.power() == 0 && power.notificationCount() == 0);
  NimBLEDevice::nextService = "0x1826";
  NimBLEDevice::nextCharacteristic = "0x2ad2";
  assert(manager.enqueue(BleOpKind::Connect, "22:33:44:55:66:77", 0));
  assert(outcome(manager).success);
  manager.tick();
  NimBLEDevice::clients.front()->service.measurement.emit({0x44, 0, 0x10, 0x0e, 180, 0, 0x2c, 1});
  manager.tick();
  assert(power.power() == 300 && power.cadence() == 90 && power.speed() == 10);
  measurement = &NimBLEDevice::clients.front()->service.measurement;
  const auto previousPower = power.power();
  measurement->emit({0xc1, 0, 11, 0});
  manager.tick();
  assert(power.power() == previousPower);
  for (int16_t watts : {int16_t(0), int16_t(256), int16_t(2000), int16_t(-25)}) {
    const auto before = power.notificationCount();
    measurement->emit({0x41, 0, uint8_t(watts), uint8_t(uint16_t(watts) >> 8)});
    manager.tick();
    assert(power.power() == watts && power.notificationCount() == before + 1);
    measurement->emit({0x41, 0, 0xFF});
    manager.tick();
    assert(power.power() == watts && power.notificationCount() == before + 1);
  }
  NimBLEDevice::nextService = "0x1816";
  NimBLEDevice::nextCharacteristic = "0x2a5b";
  assert(manager.enqueue(BleOpKind::Connect, "22:33:44:55:66:78", 0));
  assert(outcome(manager).success);
  manager.tick();
  measurement = &NimBLEDevice::clients.front()->service.measurement;
  measurement->emit({2, 1, 0, 0, 4});
  measurement->emit({2, 2, 0, 0, 8});
  manager.tick();
  assert(power.cadence() == 60 && power.power() == 0);
  NimBLEDevice::nextService = "0xFFE0";
  NimBLEDevice::nextCharacteristic = "0xFFE1";
  NimBLEAdvertisedDevice sensor;
  sensor.name = "WT901";
  sensor.services = {"0xFFE0"};
  sensor.address = NimBLEAddress("33:44:55:66:77:88", 1);
  NimBLEDevice::scan.results.devices = {sensor};
  assert(manager.enqueue(BleOpKind::Posture));
  assert(outcome(manager).success && posture.connected());
  auto *postureClient = NimBLEDevice::clients.back();
  assert(postureClient->peer.getType() == 1);
  std::vector<uint8_t> angle{0x55, 0x53, 0, 0, 0x2e, 0xfb, 0, 0, 0, 0, 0};
  for (size_t index = 0; index < 10; ++index) angle[10] += angle[index];
  postureClient->service.measurement.emit(angle);
  const auto postureCount = posture.notificationCount();
  manager.tick();
  assert(posture.notificationCount() == postureCount + 1 && std::abs(posture.pitchDeg() + 12.34f) < 0.001f);
  postureClient->disconnect();
  assert(!posture.connected());
  const auto scansBeforeDirect = NimBLEDevice::scan.starts.load();
  assert(manager.enqueue(BleOpKind::Posture, "33:44:55:66:77:88", 1));
  assert(outcome(manager).success && posture.connected());
  assert(NimBLEDevice::scan.starts.load() == scansBeforeDirect);
  assert(saved.load() && saved.ble(0)->role == SavedConnections::Role::Posture);
  posture.disconnect();
  NimBLEDevice::scan.manualCompletion = true;
  NimBLEDevice::scan.startHook = {};
  assert(manager.enqueue(BleOpKind::Scan));
  assert(manager.enqueue(BleOpKind::Scan));
  assert(manager.enqueue(BleOpKind::Scan));
  assert(manager.enqueue(BleOpKind::Scan));
  assert(!manager.enqueue(BleOpKind::Scan));
  assert(!manager.suspend());
  manager.cancel();
  for (unsigned index = 0; index < 4; ++index) assert(outcome(manager).cancelled);
  assert(!manager.busy());
  NimBLEDevice::scan.manualCompletion = false;
  NimBLEDevice::scan.results.devices.clear();
  const auto started = std::chrono::steady_clock::now();
  assert(manager.enqueue(BleOpKind::Scan));
  assert(outcome(manager).success);
  assert(manager.enqueue(BleOpKind::Scan));
  assert(outcome(manager).success);
  assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1000));
  assert(manager.enqueue(BleOpKind::Posture));
  assert(!outcome(manager).success);
  assert(!manager.enqueue(BleOpKind::Posture));
  assert(manager.enqueue(BleOpKind::Posture, "33:44:55:66:77:88", 1));
  assert(outcome(manager).success && posture.connected());
  assert(manager.enqueue(BleOpKind::Scan));
  assert(outcome(manager).success);
  assert(manager.suspend() && !manager.available() && !NimBLEDevice::isInitialized());
  assert(manager.begin());
  assert(manager.suspend());
  NimBLEDevice::nextService = "0x1818";
  NimBLEDevice::nextCharacteristic = "0x2a63";
  std::cout << "PASS BLE manager/data: initialization recovery, queued cancel, queue bounds, sensor data during diagnostics pause, CPS/FTMS/CSC/posture parsing, random address, disconnect, backoff, suspend/restart, no fixed gap\n";
}

static void favoriteTests() {
  SavedConnections saved;
  assert(saved.loadFavorites() && saved.favoriteCount() == 0);
  assert(!saved.setFavorite("invalid", 0, "bad", true));
  assert(saved.setFavorite("AA:22:33:44:55:66", 1, "My Bike Power", true));
  SavedConnections reboot;
  assert(reboot.loadFavorites() && reboot.isFavorite("aa:22:33:44:55:66", 1));
  assert(!reboot.isFavorite("aa:22:33:44:55:66", 0));
  Preferences::failWrite = true;
  assert(!reboot.setFavorite("aa:22:33:44:55:66", 1, "My Bike Power", false));
  assert(reboot.favoriteCount() == 1);
  Preferences::failWrite = false;
  BlePowerMeter power;
  power.begin("favorites-test", 2.1f);
  auto &devices = NimBLEDevice::scan.results.devices;
  for (unsigned index = 0; index < 48; ++index) {
    NimBLEAdvertisedDevice device;
    device.name = index % 2 ? "Shared bIkE" : "Sensor";
    char address[18];
    snprintf(address, sizeof(address), "11:22:33:44:55:%02x", index);
    device.address = NimBLEAddress(address, 0);
    device.services = {"0x1818"};
    devices.push_back(device);
  }
  NimBLEAdvertisedDevice favorite;
  favorite.name = "My BIKE Power";
  favorite.address = NimBLEAddress("AA:22:33:44:55:66", 1);
  devices.push_back(favorite);
  BlePowerDevice output[kBleDeviceCapacity]{};
  size_t count = 0;
  assert(power.scanDevices(output, kBleDeviceCapacity, count, 1));
  assert(count == kBleDeviceCapacity && output[0].favorite && output[0].addressType == 1);
  for (size_t index = 1; index < count; ++index) assert(!strcmp(output[index].name, "Sensor"));
  auto &bytes = Preferences::bytes["aero-links/favorites"];
  bytes.back() ^= 1;
  assert(!saved.loadFavorites());
  bytes.back() ^= 1;
  assert(reboot.setFavorite("aa:22:33:44:55:66", 1, "My Bike Power", false));
  assert(saved.loadFavorites() && saved.favoriteCount() == 0);
  assert(saved.setFavorite("11:22:33:44:55:01", 0, "One", true));
  assert(saved.setFavorite("11:22:33:44:55:02", 0, "Two", true));
  assert(saved.setFavorite("11:22:33:44:55:03", 0, "Three", true));
  assert(saved.setFavorite("11:22:33:44:55:04", 0, "Four", true));
  assert(!saved.setFavorite("11:22:33:44:55:05", 0, "Five", true));
  Preferences::bytes.erase("aero-links/favorites");
  NimBLEDevice::initialized = false;
  std::cout << "PASS BLE favorites: reboot, CRC, write failure, capacity, typed address, bike filtering, favorite priority beyond scan capacity\n";
}

int main() {
  HeartRateMeasurement heart;
  const uint8_t bpm8[] = {0, 180};
  const uint8_t bpm16[] = {1, 44, 1};
  const uint8_t noContact[] = {4, 100};
  const uint8_t optional[] = {0x1e, 125, 0, 0, 0, 4};
  assert(heart.accept(bpm8, sizeof(bpm8), UINT32_MAX - 100));
  assert(heart.valid(100) && heart.bpm() == 180 && !heart.valid(5001));
  assert(heart.accept(bpm16, sizeof(bpm16), 10) && heart.bpm() == 300);
  assert(!heart.accept(bpm16, 2, 20) && !heart.valid(5011));
  assert(heart.accept(noContact, sizeof(noContact), 20) && !heart.valid(20));
  assert(heart.accept(optional, sizeof(optional), 30) && heart.valid(30) && heart.bpm() == 125);
  assert(!heart.accept(optional, sizeof(optional) - 1, 40));
  heart.invalidate();
  assert(!heart.valid(40));
  bufferTests();
  inboxTests();
  scannerTests();
  favoriteTests();
  controlTests();
  assert(!NimBLEDevice::server && !NimBLEDevice::advertising.active);
  std::cout << "PASS BLE runtime: central-only sensors, no phone advertising or GATT service\n";
}
