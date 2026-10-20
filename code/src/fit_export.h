#pragma once

#include "csv_logger.h"
#include <atomic>

class FitExport {
public:
  bool start(CsvLogger &logger, bool timeValid);
  bool busy() const { return busy_.load(); }
  bool takeResult(bool &success, char *text, size_t capacity);
private:
  static void task(void *argument);
  bool convert();
  CsvLogger *logger_ = nullptr;
  std::atomic<bool> busy_{false}, ready_{false};
  bool success_ = false;
  char result_[160] = {};
  char output_[96] = {};
};

extern FitExport fitExport;
