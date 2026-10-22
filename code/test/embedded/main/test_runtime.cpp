#include "ble_inbox.h"
#include "fit_activity.h"
#include "log_buffer.h"
#include "ui_navigation_guard.h"
#include "ui_touch_buffer.h"
#include "unity.h"
#include "unity_test_runner.h"
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>

TEST_CASE("BLE inbox owns packets and preserves FIFO", "[ble]") {
  BleInbox<2, 4> inbox;
  uint8_t data[] = {1, 2, 3, 4};
  TEST_ASSERT_TRUE(inbox.push(7, data, sizeof(data)));
  data[0] = 9;
  TEST_ASSERT_TRUE(inbox.push(8, data, 1));
  BleInbox<2, 4>::Packet packet;
  TEST_ASSERT_TRUE(inbox.pop(packet));
  TEST_ASSERT_EQUAL_UINT8(7, packet.kind);
  TEST_ASSERT_EQUAL_UINT32(4, packet.length);
  TEST_ASSERT_EQUAL_UINT8(1, packet.data[0]);
  TEST_ASSERT_TRUE(inbox.pop(packet));
  TEST_ASSERT_EQUAL_UINT8(8, packet.kind);
  TEST_ASSERT_EQUAL_UINT8(9, packet.data[0]);
  TEST_ASSERT_FALSE(inbox.pop(packet));
}

TEST_CASE("BLE inbox rejects invalid and overflowing packets", "[ble]") {
  BleInbox<1, 4> inbox;
  const uint8_t data[] = {1, 2, 3, 4, 5};
  TEST_ASSERT_FALSE(inbox.push(0, nullptr, 1));
  TEST_ASSERT_FALSE(inbox.push(0, data, 0));
  TEST_ASSERT_FALSE(inbox.push(0, data, sizeof(data)));
  TEST_ASSERT_TRUE(inbox.push(0, data, 4));
  TEST_ASSERT_FALSE(inbox.push(0, data, 1));
  TEST_ASSERT_EQUAL_UINT32(4, inbox.dropped());
}

TEST_CASE("BLE inbox reset invalidates old generations", "[ble]") {
  BleInbox<2, 4> inbox;
  const uint8_t data[] = {42};
  TEST_ASSERT_TRUE(inbox.push(0, data, 1));
  BleInbox<2, 4>::Packet oldPacket;
  TEST_ASSERT_TRUE(inbox.pop(oldPacket));
  TEST_ASSERT_TRUE(inbox.push(0, data, 1));
  inbox.reset();
  TEST_ASSERT_NOT_EQUAL(oldPacket.generation, inbox.generation());
  BleInbox<2, 4>::Packet packet;
  TEST_ASSERT_FALSE(inbox.pop(packet));
  TEST_ASSERT_TRUE(inbox.push(0, data, 1));
  TEST_ASSERT_TRUE(inbox.pop(packet));
  TEST_ASSERT_EQUAL_UINT32(inbox.generation(), packet.generation);
}

namespace {
struct InboxTransfer {
  BleInbox<8, sizeof(uint32_t)> inbox;
  SemaphoreHandle_t done = nullptr;
};

void producePackets(void *argument) {
  auto &transfer = *static_cast<InboxTransfer *>(argument);
  const TickType_t started = xTaskGetTickCount();
  for (uint32_t sequence = 0; sequence < 64;) {
    if (transfer.inbox.push(1, reinterpret_cast<const uint8_t *>(&sequence), sizeof(sequence)))
      ++sequence;
    else
      vTaskDelay(1);
    if (xTaskGetTickCount() - started > pdMS_TO_TICKS(4000)) break;
  }
  xSemaphoreGive(transfer.done);
  vTaskDelete(nullptr);
}

bool appendFit(void *context, const uint8_t *data, size_t length) {
  auto &bytes = *static_cast<std::vector<uint8_t> *>(context);
  bytes.insert(bytes.end(), data, data + length);
  return true;
}

uint16_t fitCrc(const uint8_t *data, size_t length) {
  uint16_t value = 0;
  for (size_t index = 0; index < length; ++index) {
    value ^= data[index];
    for (unsigned bit = 0; bit < 8; ++bit)
      value = value & 1 ? (value >> 1) ^ 0xA001 : value >> 1;
  }
  return value;
}
}

TEST_CASE("BLE inbox transfers packets between FreeRTOS tasks", "[ble]") {
  static InboxTransfer transfer;
  transfer.inbox.reset();
  transfer.done = xSemaphoreCreateBinary();
  TEST_ASSERT_NOT_NULL(transfer.done);
  const BaseType_t created = xTaskCreatePinnedToCore(
      producePackets, "inboxProducer", 3072, &transfer, 5, nullptr, 0);
  if (created != pdPASS) {
    vSemaphoreDelete(transfer.done);
    TEST_FAIL_MESSAGE("Could not create producer");
  }
  uint32_t received = 0;
  bool ordered = true;
  const TickType_t started = xTaskGetTickCount();
  while (received < 64 && xTaskGetTickCount() - started < pdMS_TO_TICKS(5000)) {
    BleInbox<8, sizeof(uint32_t)>::Packet packet;
    if (transfer.inbox.pop(packet)) {
      uint32_t sequence;
      memcpy(&sequence, packet.data, sizeof(sequence));
      ordered = ordered && sequence == received++;
    } else {
      vTaskDelay(1);
    }
  }
  const bool finished = xSemaphoreTake(transfer.done, pdMS_TO_TICKS(1000)) == pdTRUE;
  if (finished) vSemaphoreDelete(transfer.done);
  TEST_ASSERT_TRUE(finished);
  TEST_ASSERT_TRUE(ordered);
  TEST_ASSERT_EQUAL_UINT32(64, received);
}

TEST_CASE("Log ring reports overwritten bytes and bounded reads", "[log]") {
  LogBuffer<8> ring;
  const uint8_t data[] = "abcdefghijkl";
  ring.append(data, 12);
  uint64_t cursor = 0, lost = 0;
  uint8_t output[8] = {};
  TEST_ASSERT_EQUAL_UINT32(3, ring.read(cursor, output, 3, lost));
  TEST_ASSERT_EQUAL_UINT32(4, lost);
  TEST_ASSERT_EQUAL_MEMORY("efg", output, 3);
  TEST_ASSERT_EQUAL_UINT32(5, ring.read(cursor, output, 8, lost));
  TEST_ASSERT_EQUAL_UINT32(0, lost);
  TEST_ASSERT_EQUAL_MEMORY("hijkl", output, 5);
}

TEST_CASE("Log ring clamps future cursors", "[log]") {
  LogBuffer<8> ring;
  uint64_t cursor = 99, lost = 0;
  uint8_t output[1] = {};
  TEST_ASSERT_EQUAL_UINT32(0, ring.read(cursor, output, 1, lost));
  TEST_ASSERT_EQUAL_UINT32(0, cursor);
}

TEST_CASE("Touch buffer handles timer wrap", "[ui]") {
  TouchBuffer guard(450);
  guard.reset(UINT32_MAX - 100);
  TEST_ASSERT_FALSE(guard.accept(348));
  TEST_ASSERT_TRUE(guard.accept(349));
  TEST_ASSERT_FALSE(guard.accept(350));
}

TEST_CASE("Navigation requires quiet release after back", "[ui]") {
  UiNavigationGuard guard;
  TEST_ASSERT_TRUE(guard.beginBack(1000));
  TEST_ASSERT_FALSE(guard.beginBack(1001));
  TEST_ASSERT_FALSE(guard.filterTouch(false, 1200));
  TEST_ASSERT_FALSE(guard.filterTouch(true, 1250));
  TEST_ASSERT_FALSE(guard.filterTouch(false, 1300));
  TEST_ASSERT_FALSE(guard.filterTouch(false, 1420));
  TEST_ASSERT_TRUE(guard.filterTouch(true, 1421));
}

TEST_CASE("FIT parser rejects malformed and nonfinite samples", "[fit]") {
  FitSample sample;
  TEST_ASSERT_TRUE(parseFitCsv("1000,250,90,10,11,0,-15,1.225,1\n", sample));
  TEST_ASSERT_FALSE(parseFitCsv("1000,250,90,nan,11,0,0,1.2,1\n", sample));
  TEST_ASSERT_FALSE(parseFitCsv("1000,250,90,10,11,0,0,1.2,2\n", sample));
  TEST_ASSERT_FALSE(parseFitCsv("1000,250,90,10,11,0,0,1.2,1,extra\n", sample));
}

TEST_CASE("FIT writer emits valid CRC and rejects incomplete records", "[fit]") {
  std::vector<uint8_t> bytes;
  FitActivity writer(appendFit, &bytes);
  constexpr uint32_t utc = 1792483200;
  FitSample sample;
  TEST_ASSERT_TRUE(parseFitCsv("1000,250,90,10,11,0,-15,1.225,1\n", sample));
  TEST_ASSERT_TRUE(writer.begin(1, utc, utc, 0, 0));
  TEST_ASSERT_TRUE(writer.record(utc, sample, 0));
  TEST_ASSERT_TRUE(writer.finish());
  TEST_ASSERT_EQUAL_MEMORY(".FIT", bytes.data() + 8, 4);
  TEST_ASSERT_EQUAL_UINT16(0, fitCrc(bytes.data(), 14));
  TEST_ASSERT_EQUAL_UINT16(0, fitCrc(bytes.data(), bytes.size()));
  FitActivity missing(appendFit, &bytes);
  TEST_ASSERT_TRUE(missing.begin(1, utc, utc, 0, 0));
  TEST_ASSERT_FALSE(missing.finish());
  FitActivity failure([](void *, const uint8_t *, size_t) { return false; }, nullptr);
  TEST_ASSERT_FALSE(failure.begin(1, utc, utc, 0, 0));
}

extern "C" void app_main() {
  unity_run_menu();
}