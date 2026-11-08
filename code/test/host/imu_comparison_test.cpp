#include <cassert>
#include <atomic>
#include <cstring>
#include <iostream>
#include "board_imu.h"
#include "imu_comparison.h"
#include "rider_posture.h"
#include "recent_sensor_reading.h"
#include "imu_comparison_logger.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <thread>

static std::timed_mutex sdMutex;
static std::atomic<bool> cardReady{true}, writeFails{false}, syncFails{false};
static std::string mountPath;
bool sdCardReady() { return cardReady; }
bool lockSdCard(TickType_t timeout) { return sdMutex.try_lock_for(std::chrono::milliseconds(timeout)); }
void unlockSdCard() { sdMutex.unlock(); }
const char *sdMountPoint() { return mountPath.c_str(); }
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
  if (writeFails) { errno = EIO; return -1; }
  return __real_write(fd, data, size);
}
extern "C" int __wrap_fsync(int fd) {
  if (syncFails) { errno = EIO; return -1; }
  return __real_fsync(fd);
}

static void await(const std::function<bool()> &condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!condition()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}

static void loggerTests(const ImuComparisonRecord &record) {
  const char *artifacts = std::getenv("IMU_ARTIFACT_DIR");
  assert(artifacts);
  mountPath = artifacts;
  ImuComparisonLogger logger;
  failTaskCreation = true;
  assert(!logger.begin());
  failTaskCreation = false;
  assert(logger.begin());
  cardReady = false;
  assert(logger.start(1));
  await([&] { return !logger.busy(); });
  assert(logger.failed());
  cardReady = true;
  assert(logger.start(2));
  for (unsigned index = 0; index < 20; ++index) assert(logger.log(record));
  logger.requestStop();
  assert(!logger.log(record));
  await([&] { return !logger.busy(); });
  assert(!logger.failed() && logger.written() == 20 && logger.dropped() == 0);
  const std::string path = mountPath + "/IMU/I00000002-0.csv";
  std::ifstream input(path);
  const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  assert(text.find(ImuComparison::header()) == 0);
  assert(std::count(text.begin(), text.end(), '\n') == 21);
  assert(logger.start(2));
  assert(logger.log(record));
  logger.requestStop();
  await([&] { return !logger.busy(); });
  assert(!logger.failed() && logger.written() == 1);
  assert(std::ifstream(mountPath + "/IMU/I00000002-1.csv").good());
  writeFails = true;
  assert(logger.start(3));
  await([&] { return !logger.busy(); });
  assert(logger.failed());
  writeFails = false;
  assert(logger.start(4));
  assert(logger.log(record));
  syncFails = true;
  logger.requestStop();
  await([&] { return !logger.busy(); });
  assert(logger.failed() && logger.written() == 0);
  syncFails = false;
  sdMutex.lock();
  assert(logger.start(5));
  for (unsigned index = 0; index < 256; ++index) logger.log(record);
  assert(logger.dropped() > 0);
  sdMutex.unlock();
  logger.requestStop();
  await([&] { return !logger.busy(); });
  assert(!logger.failed());
  assert(logger.written() + logger.dropped() == 256);
  assert(logger.end());
  std::cout << "PASS IMU storage: unavailable SD, real CSV, stop drain, unique files, write/fsync failures, bounded queue\n";
}

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;

static void postureTests() {
  RiderPosture estimator;
  assert(estimator.placement() == RiderPosture::Placement::Forearm);
  ImuSample board, wearable;
  board.valid = wearable.valid = true;
  board.accelerationG[2] = wearable.accelerationG[2] = 1;
  board.receivedMs = wearable.receivedMs = 100;
  assert(estimator.update(100, board, wearable).state == RiderPosture::State::NeedUpright);
  assert(!estimator.calibrate(true, 100, board, wearable));
  assert(estimator.calibrate(false, 100, board, wearable));
  assert(!estimator.calibrate(true, 100, board, wearable));
  wearable.accelerationG[0] = -0.5f;
  wearable.accelerationG[2] = std::sqrt(0.75f);
  assert(estimator.calibrate(true, 100, board, wearable));
  for (uint32_t now = 100; now <= 2200; now += 100) {
    board.receivedMs = wearable.receivedMs = now;
    estimator.update(now, board, wearable);
  }
  const auto estimate = estimator.update(2200, board, wearable);
  assert(estimate.state == RiderPosture::State::Forward && !estimate.indirect);
  assert(std::fabs(estimate.relativeDegrees - 30) < 0.001f);
  wearable.angularVelocityDps[0] = 60;
  assert(estimator.update(2200, board, wearable).state == RiderPosture::State::Moving);
  wearable.angularVelocityDps[0] = 0;
  assert(estimator.update(2501, board, wearable).state == RiderPosture::State::NoData);
  estimator.setPlacement(RiderPosture::Placement::Helmet);
  assert(estimator.update(2200, board, wearable).state == RiderPosture::State::NeedUpright);
  assert(estimator.update(2200, board, wearable).indirect);
  for (auto placement : {RiderPosture::Placement::Forearm, RiderPosture::Placement::UpperArm,
                         RiderPosture::Placement::BackStrap, RiderPosture::Placement::Helmet}) {
    RiderPosture mounted;
    mounted.setPlacement(placement);
    board = wearable = ImuSample{};
    board.valid = wearable.valid = true;
    board.receivedMs = wearable.receivedMs = 100;
    board.accelerationG[2] = wearable.accelerationG[2] = 1;
    assert(mounted.calibrate(false, 100, board, wearable));
    wearable.accelerationG[0] = -0.5f;
    wearable.accelerationG[2] = std::sqrt(0.75f);
    assert(mounted.calibrate(true, 100, board, wearable));
    wearable.accelerationG[0] = -std::sin(50.0f / 57.2957795f);
    wearable.accelerationG[2] = std::cos(50.0f / 57.2957795f);
    for (uint32_t now = 100; now <= 2200; now += 100) {
      board.receivedMs = wearable.receivedMs = now;
      mounted.update(now, board, wearable);
    }
    const auto classified = mounted.update(2200, board, wearable);
    assert(classified.state == (placement == RiderPosture::Placement::Helmet ||
                                placement == RiderPosture::Placement::Forearm
        ? RiderPosture::State::Forward : RiderPosture::State::Low));
    assert(classified.indirect == (placement != RiderPosture::Placement::BackStrap &&
                                   placement != RiderPosture::Placement::Forearm));
    board.accelerationG[0] = -std::sin(10.0f / 57.2957795f);
    board.accelerationG[2] = std::cos(10.0f / 57.2957795f);
    wearable.accelerationG[0] = -std::sin(40.0f / 57.2957795f);
    wearable.accelerationG[2] = std::cos(40.0f / 57.2957795f);
    assert(std::fabs(mounted.update(2200, board, wearable).relativeDegrees - 30) < 0.001f);
    board.receivedMs -= 101;
    assert(mounted.update(2200, board, wearable).state == RiderPosture::State::NoData);
    board.receivedMs += 101;
    wearable.accelerationG[0] = NAN;
    assert(mounted.update(2200, board, wearable).state == RiderPosture::State::NoData);
  }
  RecentSensorReading recent;
  char text[160];
  recent.format(0, text, sizeof(text));
  assert(std::string(text) == "--");
  recent.capture("A +0.00 +0.00 +1.00 g", 100);
  recent.format(1100, text, sizeof(text));
  assert(std::string(text) == "A +0.00 +0.00 +1.00 g  1000ms");
  recent.capture("42 bpm", 0xFFFFFFF0u);
  recent.format(20, text, sizeof(text));
  assert(std::string(text) == "42 bpm  36ms");
  recent.clear();
  recent.format(100, text, sizeof(text));
  assert(std::string(text) == "--");
}

int main() {
  postureTests();
  TwoWire wire;
  BoardImu imu;
  SensorQMI8658::available = false;
  assert(!imu.begin(wire));
  SensorQMI8658::available = true;
  SensorQMI8658::configError = -1;
  assert(!imu.begin(wire));
  SensorQMI8658::configError = 0;
  assert(imu.begin(wire));
  clockOffset += 100;
  imu.poll(millis());
  const auto board = imu.sample(millis());
  assert(board.valid && board.accelerationG[2] == 1);
  assert(ImuComparison::norm(board.angularVelocityDps) == 5);
  assert(!imu.sample(board.receivedMs + 251).valid);
  ImuSample wt = board;
  ImuComparison comparison;
  auto record = comparison.capture(board.receivedMs, board, wt, 1, 2, 3);
  assert(record.paired && record.skewMs == 0);
  loggerTests(record);
  char text[768];
  assert(ImuComparison::format(record, text, sizeof(text)) > 0);
  assert(strstr(text, "nan") == nullptr);
  assert(!comparison.capture(board.receivedMs, board, wt, 1, 2, 3).paired);
  ++wt.sequence;
  wt.receivedMs -= 101;
  ++wt.sequence;
  assert(!comparison.capture(board.receivedMs, board, wt, 1, 2, 3).paired);
  wt.receivedMs += 1;
  auto aligned = board;
  ++aligned.sequence;
  assert(comparison.capture(board.receivedMs, aligned, wt, 1, 2, 3).paired);
  wt.valid = false;
  record = comparison.capture(board.receivedMs, board, wt, 1, 2, 3);
  assert(!record.paired && !record.wt.valid);
  assert(ImuComparison::format(record, text, sizeof(text)) > 0 && strstr(text, "nan"));
  assert(ImuComparison::format(record, text, 8) == 0);
  wt = board;
  wt.accelerationG[0] = NAN;
  assert(!comparison.capture(board.receivedMs, board, wt, 1, 2, 3).wt.valid);
  wt = board;
  wt.receivedMs = 0xFFFFFFF0u;
  aligned = wt;
  ImuComparison wrapped;
  assert(wrapped.capture(20, aligned, wt, 0, 0, 0).paired);
  SensorQMI8658::readOk = false;
  clockOffset += 20;
  imu.poll(millis());
  assert(!imu.sample(millis()).valid);
  std::cout << "PASS IMU comparison: driver errors, units, freshness, skew, deduplication, wrap, invalid CSV, bounds\n";
}