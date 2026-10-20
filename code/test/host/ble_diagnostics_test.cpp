#include "ble_diagnostics.h"
#include "runtime_log.h"
#include <NimBLEDevice.h>
#include <cassert>
#include <iostream>
#include <thread>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool NimBLEDevice::initialized = false;
std::unique_ptr<NimBLEServer> NimBLEDevice::server;
NimBLEAdvertising NimBLEDevice::advertising;

static void advance(unsigned ms = 20) {
  clockOffset += ms;
  bleDiagnostics.tick();
}

static std::string drain(NimBLECharacteristic *tx, unsigned iterations = 1000) {
  const size_t start = tx->notifications.size();
  for (unsigned i = 0; i < iterations; ++i) advance();
  std::string text;
  for (size_t i = start; i < tx->notifications.size(); ++i) text += tx->notifications[i];
  return text;
}

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

int main() {
  bufferTests();
  AppLog.println("history before BLE init");
  assert(!bleDiagnostics.begin());
  NimBLEDevice::initialized = true;
  NimBLEDevice::advertising.fail = true;
  assert(bleDiagnostics.begin());
  assert(!NimBLEDevice::advertising.active);
  NimBLEDevice::advertising.fail = false;
  advance(3000);
  assert(NimBLEDevice::advertising.active);
  auto *server = NimBLEDevice::server.get();
  auto *service = server->getServiceByUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  assert(service && server->services.size() == 1);
  auto *tx = service->getCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
  auto *rx = service->getCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
  assert(tx->properties == (NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY));
  assert(rx->properties == NIMBLE_PROPERTY::WRITE && rx->maxLength == 32);
  server->connect();
  advance();
  assert(tx->notifications.empty());
  tx->subscribe();
  const std::string replay = drain(tx);
  assert(replay.find("history before BLE init") != std::string::npos);
  assert(replay.find("RAW") != std::string::npos);
  for (const auto &chunk : tx->notifications) assert(chunk.size() <= 20);
  const auto sent = tx->notifications.size();
  AppLog.println("pacing marker");
  bleDiagnostics.tick();
  assert(tx->notifications.size() == sent);
  advance();
  assert(tx->notifications.size() == sent + 1);
  server->mtu = 185;
  drain(tx);
  for (const auto &chunk : tx->notifications) assert(chunk.size() <= 180);
  AppLog.println("retry marker");
  tx->failNotify = true;
  const auto beforeRetry = tx->notifications.size();
  advance();
  assert(tx->notifications.size() == beforeRetry);
  tx->failNotify = false;
  assert(drain(tx).find("retry marker") != std::string::npos);
  assert(tx->read().find("notify_fail=1") != std::string::npos);
  rx->command("RAW ON\r\n");
  assert(AppLog.rawEnabled());
  rx->command("RAW OFF");
  assert(!AppLog.rawEnabled());
  rx->command("INVALID");
  assert(drain(tx).find("ERROR unknown command") != std::string::npos);
  rx->command(std::string("RAW ON\0BAD", 10));
  assert(!AppLog.rawEnabled());
  assert(drain(tx).find("printable ASCII") != std::string::npos);
  rx->command(std::string(33, 'A'));
  assert(drain(tx).find("1..32 ASCII") != std::string::npos);
  rx->command("PAUSE");
  const auto paused = tx->notifications.size();
  for (unsigned i = 0; i < 300; ++i) AppLog.printf("overflow line %u %050u\n", i, i);
  advance(1000);
  assert(tx->notifications.size() == paused);
  rx->command("RESUME");
  assert(drain(tx).find("!GAP\n") != std::string::npos);
  assert(tx->read().find("lost_bytes=0") == std::string::npos);
  rx->command("LIVE");
  assert(drain(tx).find("overflow line") == std::string::npos);
  rx->command("REPLAY");
  assert(drain(tx).find("overflow line") != std::string::npos);
  server->connect(8);
  assert(server->disconnected.back() == 8);
  assert(tx->read().find("subscribed=1") != std::string::npos);
  rx->command("RAW ON");
  server->disconnect(7);
  assert(!AppLog.rawEnabled());
  advance(3000);
  assert(NimBLEDevice::advertising.active);
  server->connect();
  tx->subscribe();
  assert(drain(tx).find("phone disconnected") != std::string::npos);
  tx->subscribe(0);
  const auto unsubscribed = tx->notifications.size();
  AppLog.println("unsubscribed marker");
  advance();
  assert(tx->notifications.size() == unsubscribed);
  tx->subscribe();
  assert(drain(tx).find("unsubscribed marker") != std::string::npos);
  bleDiagnostics.suspend();
  assert(!NimBLEDevice::advertising.active);
  // Failed stack teardown: reuse existing GATT service rather than duplicate it.
  assert(bleDiagnostics.begin());
  assert(server->services.size() == 1);
  bleDiagnostics.suspend();
  NimBLEDevice::destroy();
  advance(1000);
  NimBLEDevice::initialized = true;
  assert(bleDiagnostics.begin());
  server = NimBLEDevice::server.get();
  service = server->getServiceByUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
  tx = service->getCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
  server->connect();
  tx->subscribe();
  assert(drain(tx).find("phone disconnected") != std::string::npos);
  bleDiagnostics.suspend();
  NimBLEDevice::destroy();
  std::cout << "PASS BLE diagnostics: bounded replay, raw HEX, MTU23/185, pacing, retry, commands, gaps, reconnect, stack restart\n";
}
