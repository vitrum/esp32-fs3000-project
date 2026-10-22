#pragma once
struct HostMdns {
  bool begin(const char *) { ++starts; return !fail; }
  void addService(const char *, const char *, int) {}
  void end() { ++stops; }
  bool fail = false;
  unsigned starts = 0, stops = 0;
};
inline HostMdns MDNS;