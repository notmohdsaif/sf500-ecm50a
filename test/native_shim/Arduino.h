#pragma once
// Minimal Arduino compatibility shim for the [env:native] host test build.
// Only what persist.cpp / journal.cpp actually use. The real <Arduino.h> is
// used on the ESP32 target.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

using String = std::string;

// BSD strlcpy — not in glibc. Bounded copy, always NUL-terminates.
#ifndef HAVE_STRLCPY_SHIM
#define HAVE_STRLCPY_SHIM
inline size_t strlcpy(char* dst, const char* src, size_t size) {
  size_t len = std::strlen(src);
  if (size) {
    size_t n = (len >= size) ? size - 1 : len;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}
#endif
