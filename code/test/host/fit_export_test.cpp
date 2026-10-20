#include "fit_export.h"
#include "runtime_log.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

std::atomic<uint32_t> clockOffset{0};
HostSerial Serial;
bool failTaskCreation = false;
static bool cardReady = true, failStop = false, failLock = false;
static unsigned stops = 0;
static std::mutex sdMutex;
static std::string root;

bool sdCardReady() { return cardReady; }
const char *sdMountPoint() { return root.c_str(); }
bool lockSdCard(TickType_t) { if (failLock) return false; sdMutex.lock(); return true; }
void unlockSdCard() { sdMutex.unlock(); }

// Fake only the logger boundary; run the actual asynchronous exporter and FIT encoder.
bool CsvLogger::start() {
  _ok = true; _sessionFileCount = 0; _sampleCount = 0;
  _referenceUtc = _referenceMs = 0; _sessionOverflow = false;
  return true;
}
void CsvLogger::setLocalDate(const char *path) {
  if (_sessionFileCount == kSessionFiles) { _sessionOverflow = true; return; }
  snprintf(_sessionFiles[_sessionFileCount++], sizeof(_sessionFiles[0]), "%s", path);
}
void CsvLogger::setUtcReference(uint32_t utc, uint32_t ms) { _referenceUtc = utc; _referenceMs = ms; }
bool CsvLogger::utcReference(uint32_t &utc, uint32_t &ms) const {
  utc = _referenceUtc; ms = _referenceMs; return utc != 0;
}
void CsvLogger::log(uint32_t, float, float, float, float, float, float, float, uint8_t) { ++_sampleCount; }
bool CsvLogger::stop() {
  ++stops;
  if (failStop) return false;
  // This is also a lock ordering check: exporter must not hold the SD lock here.
  assert(sdMutex.try_lock()); sdMutex.unlock();
  _ok = false; return true;
}

static std::string result(bool expected) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (fitExport.busy()) {
    assert(std::chrono::steady_clock::now() < until);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  bool success;
  char text[160];
  assert(fitExport.takeResult(success, text, sizeof(text)) && success == expected);
  return text;
}
static void writeCsv(const char *name, const char *rows) {
  std::ofstream file(root + name);
  file << "t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc\n" << rows;
  assert(file.good());
}
static bool exists(const char *name) { return access((root + name).c_str(), F_OK) == 0; }
static void init(CsvLogger &logger, const char *name) {
  logger.start(); logger.setLocalDate(name);
  logger.setUtcReference(1792483200, 1000);
  logger.log(1000,250,90,10,11,0,-15,1.225,1);
}

int main() {
  char directory[] = "/tmp/aeroprobe-fit-export.XXXXXX";
  assert(mkdtemp(directory)); root = directory;
  CsvLogger logger;
  init(logger, "/ride.csv");
  writeCsv("/ride.csv", "1000,250,90,10,11,0,-15,1.225,1\n2000,260,91,11,12,0,-16,1.225,1\n");
  assert(!fitExport.start(logger,false)); result(false);
  assert(logger.ok() && stops == 0);
  cardReady = false;
  assert(!fitExport.start(logger,true)); result(false);
  assert(logger.ok() && stops == 0);
  cardReady = true;
  failTaskCreation = true;
  assert(!fitExport.start(logger,true)); result(false);
  failTaskCreation = false;
  failStop = true;
  assert(fitExport.start(logger,true)); result(false);
  assert(logger.ok() && !exists("/ride.fit"));
  failStop = false;
  logger.setLocalDate("/day2.csv");
  writeCsv("/day2.csv", "3000,270,92,12,13,0,-17,1.225,1\n");
  assert(fitExport.start(logger,true));
  assert(!fitExport.start(logger,true));
  assert(result(true).find("/ride.fit") != std::string::npos);
  assert(!logger.ok() && exists("/ride.csv") && exists("/day2.csv"));
  assert(!exists("/ride.fit.tmp"));
  std::ifstream fit(root + "/ride.fit", std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(fit)), {});
  assert(bytes.size() == 14 + 480 + 3 * 31 + 2 && bytes.substr(8,4) == ".FIT");
  assert(fitExport.start(logger,true));
  assert(result(false).find("already exists") != std::string::npos);
  assert(exists("/ride.fit"));

  init(logger,"/bad.csv"); writeCsv("/bad.csv","1000,250,90,nan,11,0,0,1.225,1\n");
  assert(fitExport.start(logger,true)); result(false);
  assert(exists("/bad.csv") && !exists("/bad.fit"));
  init(logger,"/short.csv"); writeCsv("/short.csv","1000,250,90,10,11,0,0,1.225,1");
  assert(fitExport.start(logger,true)); result(false);
  assert(!exists("/short.fit"));
  init(logger,"/missing.csv");
  assert(fitExport.start(logger,true)); result(false);
  assert(!exists("/missing.fit"));
  init(logger,"/empty.csv"); writeCsv("/empty.csv","");
  assert(fitExport.start(logger,true)); result(false);
  assert(!exists("/empty.fit"));
  init(logger,"/wrap.csv");
  logger.setUtcReference(1792483200, UINT32_MAX - 499);
  writeCsv("/wrap.csv","4294966796,250,90,10,11,0,0,1.225,1\n500,250,90,10,11,0,0,1.225,1\n");
  assert(fitExport.start(logger,true)); result(true);
  assert(exists("/wrap.fit"));
  init(logger,"/blocked.csv"); failLock = true;
  assert(fitExport.start(logger,true)); result(false);
  failLock = false;
  init(logger,"/overflow.csv");
  for (unsigned i = 0; i < CsvLogger::kSessionFiles; ++i) logger.setLocalDate("/extra.csv");
  assert(!fitExport.start(logger,true)); result(false);
  for (const char *name : {"/ride.csv","/day2.csv","/ride.fit","/bad.csv","/short.csv",
                            "/empty.csv","/wrap.csv","/wrap.fit"})
    assert(unlink((root + name).c_str()) == 0);
  assert(rmdir(root.c_str()) == 0);
  std::cout << "PASS FIT export: trusted time, task/stop/SD failure, multi-file session, no overwrite, malformed/empty CSV, timer wrap\n";
}
