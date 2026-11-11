#pragma once

#include <cstdint>
#include <ctime>

namespace xuesu_sleep_clock {

constexpr uint64_t minuteUs = 60'000'000;
constexpr uint64_t wakeMarginUs = 200'000;  // Avoid landing just before the boundary due to RTC granularity.

constexpr uint64_t delayUntilUs(const uint64_t targetUs, const uint64_t nowUs) {
  return (nowUs < targetUs ? targetUs - nowUs : 0) + wakeMarginUs;
}

constexpr uint64_t nextWakeDelayUs(const uint64_t paintedEpochSeconds, const uint64_t nowUs) {
  return delayUntilUs((paintedEpochSeconds / 60 + 1) * minuteUs, nowUs);
}

inline time_t nextLocalMidnight(std::tm local) {
  local.tm_hour = local.tm_min = local.tm_sec = 0;
  ++local.tm_mday;
  local.tm_isdst = -1;
  return std::mktime(&local);
}

}  // namespace xuesu_sleep_clock
