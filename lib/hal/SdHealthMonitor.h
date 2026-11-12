#pragma once

#include <cstdint>

enum class SdHealthWarning : uint8_t { None, Slow, IoError };

class SdHealthMonitor {
 public:
  void observe(const uint32_t now, const uint32_t elapsed, const bool ok) {
    if (!ok) {
      ioErrorPending = !ioErrorReported;
    } else if (!slowReported && elapsed >= kSlowCommandMs) {
      if (slowCount == 0 || now - firstSlowAt >= kWindowMs) {
        firstSlowAt = now;
        slowCount = 0;
      }
      if (slowCount < kSlowCount) ++slowCount;
      if (slowCount == kSlowCount) slowPending = true;
    }
  }

  SdHealthWarning takeWarning() {
    if (ioErrorPending) {
      ioErrorPending = false;
      ioErrorReported = true;
      slowPending = false;
      slowReported = true;
      return SdHealthWarning::IoError;
    }
    if (slowPending) {
      slowPending = false;
      slowReported = true;
      return SdHealthWarning::Slow;
    }
    return SdHealthWarning::None;
  }

 private:
  static constexpr uint32_t kSlowCommandMs = 150;
  static constexpr uint32_t kWindowMs = 15'000;
  static constexpr uint8_t kSlowCount = 3;
  uint32_t firstSlowAt = 0;
  uint8_t slowCount = 0;
  bool slowPending = false;
  bool slowReported = false;
  bool ioErrorPending = false;
  bool ioErrorReported = false;
};
