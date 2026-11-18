#include "gps_time.h"
#include "sd_download.h"
#include "ui_navigation_guard.h"
#include <driver/uart.h>
#include <cassert>
#include <functional>
#include <iostream>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;
std::mutex uartMutex;
std::deque<uint8_t> uartBytes;
std::deque<uart_event_t> uartEvents;
bool uartInstalled = false;
int uartCapacity = 0, failUartInit = 0;
bool sdLocked = false, sdBusy = false, sdOpenFails = false;
bool sdSeekFails = false, sdShortRead = false;
std::string sdContents;
TickType_t lastSdTimeout = 0;

bool lockSdCard(TickType_t timeout) {
  assert(!sdLocked);
  lastSdTimeout = timeout;
  if (sdBusy) return false;
  sdLocked = true;
  return true;
}
void unlockSdCard() { assert(sdLocked); sdLocked = false; }
fs::FS &sdFs() {
  assert(sdLocked);
  static fs::FS fs;
  return fs;
}

static void await(const std::function<bool()> &condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!condition()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

static std::string nmea(const std::string &body) {
  unsigned checksum = 0;
  for (char c : body) checksum ^= (uint8_t)c;
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  return "$" + body + suffix;
}

static void feed(const std::string &data) {
  std::lock_guard<std::mutex> lock(uartMutex);
  assert(uartInstalled);
  assert(uartBytes.size() + data.size() <= (size_t)uartCapacity);
  uartBytes.insert(uartBytes.end(), data.begin(), data.end());
}

static void gpsTests() {
  GpsTime gps;
  failUartInit = -1;
  assert(!gps.begin() && !gps.enabled());
  failUartInit = 0;
  failTaskCreation = true;
  assert(!gps.begin() && !uartInstalled);
  failTaskCreation = false;
  assert(gps.begin() && uartCapacity == 4096);

  const auto fix = nmea("GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,191026,003.1,W");
  // Consume more than one ring buffer while the application never calls tick().
  for (int i = 0; i < 100; ++i) {
    feed(fix);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  await([&] { gps.tick(); return gps.sentencesSeen() == 100; });
  assert(gps.hasFix() && gps.modulePresent());
  assert(gps.charsProcessed() == fix.size() * 100);
  assert(std::abs(gps.latitude() - 48.1173) < 0.00001);
  assert(std::abs(gps.speedMps() - 11.52355) < 0.001);
  char text[24];
  gps.formatDateTime(text, sizeof(text));
  assert(std::string(text) == "2026-10-19 12:35:19");
  gps.formatLocalDateTime(text, sizeof(text), 8 * 60);
  assert(std::string(text) == "2026-10-19 20:35:19");

  gps.pollPresence();
  gps.pollPresence();
  assert(gps.presenceChecked());
  clockOffset += 3000;
  gps.tick();
  gps.pollPresence();
  assert(!gps.modulePresent() && !gps.hasFix() && !gps.timeValid());
  const auto receivedBeforeNoise = gps.charsProcessed();
  feed("noise\r\n$GPRMC,broken*00\r\n");
  await([&] { gps.tick(); return gps.charsProcessed() > receivedBeforeNoise; });
  assert(!gps.modulePresent());
  feed(fix);
  await([&] { gps.tick(); return gps.sentencesSeen() == 101; });
  assert(gps.modulePresent() && gps.hasFix());

  clockOffset += 4000;
  feed(nmea("GPVTG,084.4,T,,M,022.4,N,041.5,K"));
  await([&] { gps.tick(); return gps.sentencesSeen() == 102; });
  assert(!gps.hasFix() && gps.fixAgeMs() >= 4000);
  feed("$GPRMC,broken*00\r\n");
  {
    std::lock_guard<std::mutex> lock(uartMutex);
    uartEvents.push_back({UART_FIFO_OVF});
    uartEvents.push_back({UART_BUFFER_FULL});
  }
  await([&] { gps.tick(); return gps.failedChecksum() == 2 && gps.overflowCount() == 2; });
  assert(gps.restart() && gps.enabled());
  gps.tick();
  assert(!gps.hasFix() && !gps.modulePresent() && gps.charsProcessed() == 0);
  gps.pollPresence();
  clockOffset += 3001;
  gps.pollPresence();
  assert(gps.presenceChecked() && !gps.modulePresent());
  feed(fix);
  await([&] { gps.tick(); return gps.modulePresent(); });
  assert(gps.presenceChecked() && gps.hasFix());
  gps.stop();
  assert(!gps.enabled() && !gps.modulePresent() && !uartInstalled);
  assert(gps.restart() && gps.enabled());
  feed(fix);
  await([&] { gps.tick(); return gps.hasFix(); });
  gps.stop();
  std::cout << "PASS GPS: background RX, snapshots, freshness, errors, restart, presence\n";
}

static void downloadTests() {
  sdContents.assign(2500, 'x');
  assert(SdDownloadResponse::tryAcquire());
  {
    SdDownloadResponse response("/record.csv", sdContents.size());
    assert(!SdDownloadResponse::tryAcquire());
    uint8_t bytes[4096];
    sdBusy = true;
    assert(response._fillBuffer(bytes, sizeof(bytes)) == RESPONSE_TRY_AGAIN);
    assert(lastSdTimeout == 0 && response._sourceValid());
    sdBusy = false;
    std::string downloaded;
    for (size_t wanted : {1024u, 1024u, 452u}) {
      clockOffset += 20;
      const size_t count = response._fillBuffer(bytes, sizeof(bytes));
      assert(count == wanted && !sdLocked);
      downloaded.append((char *)bytes, count);
      if (downloaded.size() < sdContents.size())
        assert(response._fillBuffer(bytes, sizeof(bytes)) == RESPONSE_TRY_AGAIN);
    }
    assert(downloaded == sdContents);
    assert(response._fillBuffer(bytes, sizeof(bytes)) == 0);
  }
  assert(SdDownloadResponse::tryAcquire());
  {
    SdDownloadResponse response("/record.csv", sdContents.size());
    uint8_t bytes[64];
    sdBusy = true;
    assert(response._fillBuffer(bytes, sizeof(bytes)) == RESPONSE_TRY_AGAIN);
    clockOffset += 5000;
    assert(response._fillBuffer(bytes, sizeof(bytes)) == RESPONSE_TRY_AGAIN);
    assert(!response._sourceValid());
    sdBusy = false;
  }
  for (bool *failure : {&sdOpenFails, &sdSeekFails, &sdShortRead}) {
    assert(SdDownloadResponse::tryAcquire());
    SdDownloadResponse response("/record.csv", sdContents.size());
    uint8_t bytes[64];
    *failure = true;
    const auto logEnd = AppLog.end();
    assert(response._fillBuffer(bytes, sizeof(bytes)) == RESPONSE_TRY_AGAIN);
    assert(!response._sourceValid() && !sdLocked);
    assert(AppLog.end() > logEnd);
    *failure = false;
  }
  assert(SdDownloadResponse::tryAcquire());
  {
    SdDownloadResponse empty("/empty.csv", 0);
    uint8_t bytes[1];
    assert(empty._fillBuffer(bytes, sizeof(bytes)) == 0);
  }
  std::cout << "PASS SD: single download, 1KB chunks, pacing, retry, timeout, I/O failure, EOF\n";
}

static void navigationTests() {
  UiNavigationGuard guard;
  using Sample = UiNavigationGuard::Sample;
  guard.sample(Sample::Pressed,100,100,0);
  assert(guard.pressed() && guard.started() && guard.clickAllowed());
  guard.sample(Sample::Pressed,105,103,10);
  assert(!guard.started() && guard.clickAllowed());
  guard.sample(Sample::Pressed,112,100,20);
  guard.sample(Sample::Pressed,100,100,30);
  assert(!guard.clickAllowed());
  assert(guard.navigate());
  for (uint32_t t=40;t<=2000;t+=10) {
    guard.sample(Sample::Pressed,100,100,t);
    assert(!guard.pressed() && !guard.started());
  }
  guard.sample(Sample::Released,0,0,2010);
  guard.sample(Sample::Pressed,100,100,2030);
  assert(!guard.pressed());
  guard.sample(Sample::Released,0,0,2040);
  guard.sample(Sample::Released,0,0,2070);
  guard.sample(Sample::Pressed,80,80,2071);
  assert(guard.pressed() && guard.started() && guard.clickAllowed());
  assert(guard.sample(Sample::Invalid,0,0,2080));
  assert(!guard.pressed() && !guard.clickAllowed());
  guard.sample(Sample::Pressed,80,80,2090);
  assert(!guard.pressed());
  guard.sample(Sample::Released,0,0,2100);
  guard.sample(Sample::Invalid,0,0,2120);
  guard.sample(Sample::Released,0,0,2130);
  guard.sample(Sample::Released,0,0,2160);
  guard.sample(Sample::Pressed,80,80,2161);
  assert(guard.pressed() && guard.clickAllowed());
  guard.sample(Sample::Released,0,0,2170);
  assert(guard.pressed() && guard.x()==80 && guard.y()==80);
  guard.sample(Sample::Pressed,80,80,2180);
  assert(guard.pressed() && !guard.started());
  guard.sample(Sample::Released,0,0,UINT32_MAX-10);
  guard.sample(Sample::Released,0,0,19);
  assert(!guard.pressed());
  assert(!guard.navigate());
  guard.sample(Sample::Pressed,50,50,20);
  assert(guard.pressed() && guard.clickAllowed());
  std::cout << "PASS UI: shared touch session, held navigation, drag latch, invalid reads, quiet release, timer wrap\n";
}

int main() {
  navigationTests();
  gpsTests();
  downloadTests();
}
