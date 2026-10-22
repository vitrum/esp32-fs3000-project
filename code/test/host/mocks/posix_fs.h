#pragma once
#include "Arduino.h"
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

constexpr int FILE_READ = 0;
class File {
public:
  File() = default;
  explicit File(const std::string &path) : descriptor_(new int(::open(path.c_str(), O_RDONLY)), [](int *value) {
    if (*value >= 0) ::close(*value);
    delete value;
  }) {}
  explicit operator bool() const { return descriptor_ && *descriptor_ >= 0; }
  bool isDirectory() const { struct stat info = {}; return *this && !fstat(*descriptor_, &info) && S_ISDIR(info.st_mode); }
  size_t size() const { struct stat info = {}; return *this && !fstat(*descriptor_, &info) ? size_t(info.st_size) : 0; }
  bool seek(size_t offset) { return *this && lseek(*descriptor_, offset, SEEK_SET) >= 0; }
  size_t read(uint8_t *data, size_t length) { const auto count = ::read(*descriptor_, data, length); return count < 0 ? 0 : size_t(count); }
  void close() { descriptor_.reset(); }
private:
  std::shared_ptr<int> descriptor_;
};
namespace fs {
class FSImpl {
public:
  void mountpoint(const char *value) { root = value; }
  std::string root;
};
using FSImplPtr = std::shared_ptr<FSImpl>;
class FS {
public:
  explicit FS(FSImplPtr implementation = std::make_shared<FSImpl>()) : _impl(implementation) {}
  File open(const String &path, int) { return File(_impl->root + path); }
  bool exists(const char *path) { struct stat info = {}; return stat((_impl->root + path).c_str(), &info) == 0; }
  bool mkdir(const char *path) { return ::mkdir((_impl->root + path).c_str(), 0777) == 0; }
protected:
  FSImplPtr _impl;
};
}