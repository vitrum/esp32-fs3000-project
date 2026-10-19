#pragma once
#include "ESPAsyncWebServer.h"

class AsyncAbstractResponse {
public:
  virtual ~AsyncAbstractResponse() = default;
  virtual bool _sourceValid() const = 0;
  virtual size_t _fillBuffer(uint8_t *, size_t) = 0;
protected:
  int _code = 0;
  String _contentType;
  size_t _contentLength = 0;
  bool _sendContentLength = false, _chunked = false;
};
