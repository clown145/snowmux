#pragma once

#include <cstdint>

namespace hal_environment {

// Store tenths so retained and current frames render the same exact values.
struct Reading {
  static constexpr uint16_t missing = 0xffff;
  int16_t temperature = 0;
  uint16_t humidity = missing;

  bool valid() const { return humidity != missing; }
};

// Single shot on the Xuesu's dedicated sensor bus. Unavailable on failure.
Reading sample();

}  // namespace hal_environment
