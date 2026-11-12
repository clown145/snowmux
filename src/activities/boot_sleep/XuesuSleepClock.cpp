#if FREEINK_DEVICE_XUESU_300

#include "XuesuSleepClock.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Logging.h>
#include <sys/time.h>

#include <type_traits>

#include "CrossPointSettings.h"
#include "XuesuSleepClockTiming.h"
#include "components/UITheme.h"
#include "util/TimeUtils.h"
#include "util/Timezones.h"

namespace xuesu_sleep_clock {
namespace {

constexpr uint32_t kFrameMagic = 0x58534334;  // XSC4
constexpr uint8_t kFirstMode = CrossPointSettings::CLOCK_MONTH_GRID;
constexpr uint8_t kModeCount = static_cast<uint8_t>(Layout::Count);
static_assert(kFirstMode + kModeCount - 1 == CrossPointSettings::CLOCK_ALMANAC);

// ESP32-S3 RTC RAM keeps the inputs needed to reconstruct the old plane. The
// panel driver already owns that plane, so a timer wake needs no SD cache or
// additional framebuffer. Only mark a frame after the panel commits it.
// No default member initializers: C++ startup runs again on every timer wake.
struct RetainedFrame {
  time_t epoch;
  uint32_t magic;
  uint32_t styleSeed;
  int32_t pinnedDay;
  Layout layout;
  Language language;
  uint8_t timezone;
  uint8_t dstMode;
  uint8_t battery;
  bool twelveHour;
  bool inverse;
  bool fromStandby;
  time_t environmentSampledAt;
  int16_t temperature;
  uint16_t humidity;
};
static_assert(std::is_trivial_v<RetainedFrame>, "RTC no-init state must survive C++ startup without construction");
RTC_NOINIT_ATTR RetainedFrame retained;

bool hasRetainedFrame() {
  return retained.magic == kFrameMagic && retained.epoch > 0 && retained.language < Language::_COUNT &&
         retained.layout < Layout::Count;
}

bool makeFrame(const time_t epoch, const RetainedFrame& params, Frame& frame) {
  frame = {.date = {},
           .layout = params.layout,
           .battery = params.battery,
           .twelveHour = params.twelveHour,
           .styleSeed = params.styleSeed,
           .environment = {params.temperature, params.humidity}};
  if (epoch == 0 || !localtime_r(&epoch, &frame.date)) return false;
  if (params.layout == Layout::Almanac && params.pinnedDay >= 0) {
    int year;
    unsigned month, day;
    if (!TimeUtils::getDateFromDayOrdinal(params.pinnedDay, year, month, day)) return false;
    frame.date.tm_year = year - 1900;
    frame.date.tm_mon = static_cast<int>(month) - 1;
    frame.date.tm_mday = static_cast<int>(day);
    frame.date.tm_wday = (params.pinnedDay + 4) % 7;
  }
  return true;
}

void refreshEnvironment(RetainedFrame& params, Frame& frame) {
  if (!showsEnvironment(params.layout)) return;
  if (params.environmentSampledAt && params.epoch - params.environmentSampledAt < environmentIntervalSeconds) return;
  frame.environment = hal_environment::sample();
  params.temperature = frame.environment.temperature;
  params.humidity = frame.environment.humidity;
  params.environmentSampledAt = params.epoch;
}

uint64_t timerDelay() {
  timeval now{};
  gettimeofday(&now, nullptr);
  const auto nowUs = static_cast<uint64_t>(now.tv_sec) * 1'000'000 + now.tv_usec;
  if (retained.layout != Layout::Almanac) return nextWakeDelayUs(retained.epoch, nowUs);
  if (retained.pinnedDay >= 0) return 0;

  std::tm painted{};
  if (!localtime_r(&retained.epoch, &painted)) return 0;
  const time_t midnight = nextLocalMidnight(painted);
  return midnight > retained.epoch ? delayUntilUs(static_cast<uint64_t>(midnight) * 1'000'000, nowUs) : 0;
}

bool frameChanged(const time_t epoch) {
  if (retained.layout != Layout::Almanac) return epoch / 60 != retained.epoch / 60;
  if (retained.pinnedDay >= 0) return false;

  std::tm previous{}, current{};
  return localtime_r(&retained.epoch, &previous) && localtime_r(&epoch, &current) &&
         (current.tm_year != previous.tm_year || current.tm_yday != previous.tm_yday);
}

void drawFrame(GfxRenderer& renderer, const Frame& frame, const bool inverse) {
  UITheme::drawXuesuSleepClock(renderer, frame);
  if (inverse) renderer.invertScreen();
}

}  // namespace

bool isSelected(const uint8_t sleepMode) { return sleepMode >= kFirstMode && sleepMode < kFirstMode + kModeCount; }

std::optional<Layout> layoutForMode(const uint8_t sleepMode) {
  if (!isSelected(sleepMode)) return std::nullopt;
  return static_cast<Layout>(sleepMode - kFirstMode);
}

uint8_t modeForLayout(const Layout layout) { return kFirstMode + static_cast<uint8_t>(layout); }

void clear() { retained.magic = 0; }

std::optional<FaceState> takeStandbyResume(const bool sleepWake) {
  const bool resume = sleepWake && hasRetainedFrame() && retained.fromStandby;
  clear();
  if (!resume) return std::nullopt;
  return FaceState{.layout = retained.layout,
                   .styleSeed = retained.styleSeed,
                   .pinnedDay = retained.pinnedDay,
                   .inverse = retained.inverse};
}

bool drawBeforeSleep(GfxRenderer& renderer, const std::optional<FaceState>& standby) {
  const auto selected = layoutForMode(SETTINGS.sleepScreen);
  if (!standby && !selected) return false;
  const FaceState face = standby ? *standby : FaceState{.layout = *selected};
  RetainedFrame params{.epoch = halClock.nowUtc(),
                       .magic = kFrameMagic,
                       .styleSeed = face.styleSeed,
                       .pinnedDay = face.pinnedDay,
                       .layout = face.layout,
                       .language = I18N.getLanguage(),
                       .timezone = timezones::activeIndex(),
                       .dstMode = SETTINGS.clockDst,
                       .battery = static_cast<uint8_t>(powerManager.getBatteryPercentage()),
                       .twelveHour = SETTINGS.clockFormat == 1,
                       .inverse = face.inverse,
                       .fromStandby = standby.has_value(),
                       .environmentSampledAt = 0,
                       .temperature = 0,
                       .humidity = hal_environment::Reading::missing};
  Frame frame{};
  if (!makeFrame(params.epoch, params, frame)) {
    LOG_INF("SLC", "No valid clock; entering key-only sleep");
    return false;
  }
  refreshEnvironment(params, frame);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.setRenderMode(GfxRenderer::BW);
  display.setInverted(false);
  drawFrame(renderer, frame, params.inverse);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  if (display.displayCommitted()) retained = params;
  return true;
}

void armFirstTimer() {
  if (!hasRetainedFrame()) return;
  const uint64_t delay = timerDelay();
  if (delay && powerManager.armXuesuClockTimer(delay)) {
    LOG_INF("SLC", "Clock sleep started, face=%u", static_cast<unsigned>(retained.layout));
  }
}

void refreshAfterTimerWake(GfxRenderer& renderer) {
  const time_t epoch = halClock.nowUtc();
  if (!hasRetainedFrame() || epoch < retained.epoch || gpio.xuesuClockWakeKeyPending()) {
    gpio.endXuesuClockWakeWatch();
    return;
  }

  timezones::applyToClock(retained.timezone, retained.dstMode);
  if (frameChanged(epoch)) {
    I18N.setLanguage(retained.language);
    RetainedFrame next = retained;
    next.epoch = epoch;
    Frame previous{};
    Frame current{};
    if (!makeFrame(retained.epoch, retained, previous) || !makeFrame(next.epoch, next, current)) {
      gpio.endXuesuClockWakeWatch();
      return;
    }
    refreshEnvironment(next, current);
    if (gpio.xuesuClockWakeKeyPending()) {
      gpio.endXuesuClockWakeWatch();
      return;
    }
    display.begin(true);
    renderer.begin();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    drawFrame(renderer, previous, retained.inverse);
    display.seedXuesuSleepClockBaseline();
    drawFrame(renderer, current, next.inverse);
    // Clean ghosting periodically while using the original differential LUT
    // for routine minute changes. The old plane matches the visible frame.
    const bool clean = next.layout == Layout::Almanac || current.date.tm_min % 10 == 0;
    renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    display.deepSleep();
    if (!display.displayCommitted()) {
      gpio.endXuesuClockWakeWatch();
      return;
    }
    retained = next;
    LOG_INF("SLC", "Clock refreshed, face=%u at %02d:%02d", static_cast<unsigned>(retained.layout),
            current.date.tm_hour, current.date.tm_min);
  }

  const uint64_t delay = timerDelay();
  if (delay) {
    powerManager.continueXuesuClockSleep(gpio, delay);
  } else {
    gpio.endXuesuClockWakeWatch();
  }
}

}  // namespace xuesu_sleep_clock

#endif
