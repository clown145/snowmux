"""Exercise the production clock sleep controller across deep-sleep resets."""

import unittest

from test_reading_ui_regressions import ROOT, run_cpp


class XuesuSleepClockTest(unittest.TestCase):
    def test_refresh_rebuilds_the_committed_frame_and_yields_to_confirm(self):
        source = (ROOT / 'src/activities/boot_sleep/XuesuSleepClock.cpp').read_text(encoding='utf-8')
        controller = source[source.index('namespace xuesu_sleep_clock {'):source.rindex('#endif')]
        run_cpp(r'''
#include <cassert>
#include <cstdlib>
#include <type_traits>
#include <vector>
#include "activities/boot_sleep/XuesuSleepClock.h"
#include "activities/boot_sleep/XuesuSleepClockTiming.h"

#define RTC_NOINIT_ATTR
#define LOG_INF(...) ((void)0)
enum class Language : uint8_t { EN, ZH, _COUNT };
struct CrossPointSettings {
  enum { COVER = 3, CLOCK_MONTH_GRID = 9, CLOCK_NEWSPAPER, CLOCK_FIREWORKS, CLOCK_SLOPPY, CLOCK_ALMANAC };
  uint8_t sleepScreen = CLOCK_MONTH_GRID;
  uint8_t clockDst = 0, clockFormat = 0;
} SETTINGS;
struct { Language language = Language::ZH;
  Language getLanguage() const { return language; }
  void setLanguage(Language next) { language = next; }
} I18N;
namespace TimeUtils {
bool getDateFromDayOrdinal(uint32_t ordinal, int& year, unsigned& month, unsigned& day) {
  const time_t timestamp = static_cast<time_t>(ordinal) * 86400;
  const tm* date = std::gmtime(&timestamp);
  if (!date) return false;
  year = date->tm_year + 1900;
  month = date->tm_mon + 1;
  day = date->tm_mday;
  return true;
}
}
namespace timezones {
uint8_t activeIndex() { return 8; }
void applyToClock(uint8_t zone, uint8_t dst) { assert(zone == 8 && dst == 0); }
}
static time_t epoch = 1704067199; // 2023-12-31 23:59:59 UTC
static int usec = 550000;
struct { time_t nowUtc() const { return epoch; } } halClock;
struct FakeTimeval { time_t tv_sec; int tv_usec; };
void fakeGettimeofday(FakeTimeval* out, void*) { *out = {epoch, usec}; }
tm* fakeLocaltime(const time_t* ts, tm* out) { *out = *std::gmtime(ts); return out; }
#define timeval FakeTimeval
#define gettimeofday fakeGettimeofday
#define localtime_r fakeLocaltime
struct HalDisplay {
  enum Refresh { HALF_REFRESH, FAST_REFRESH };
  bool committed = true;
  int begins = 0, seeds = 0, naps = 0;
  void setInverted(bool value) { assert(!value); }
  void begin(bool seamless) { assert(seamless); ++begins; }
  void seedXuesuSleepClockBaseline() { ++seeds; }
  void deepSleep() { ++naps; }
  bool displayCommitted() const { return committed; }
} display;
namespace hal_environment {
int sampleCount = 0;
Reading nextReading{231, 567};
Reading sample() { ++sampleCount; return nextReading; }
}
struct GfxRenderer {
  enum class Orientation { Portrait };
  enum RenderMode { BW };
  std::vector<HalDisplay::Refresh> updates;
  int inversions = 0;
  void begin() {}
  void setOrientation(Orientation) {}
  void setRenderMode(RenderMode) {}
  void invertScreen() { ++inversions; }
  void displayBuffer(HalDisplay::Refresh mode) { updates.push_back(mode); }
} renderer;
struct {
  bool key = false;
  int watchEnds = 0;
  bool xuesuClockWakeKeyPending() const { return key; }
  bool endXuesuClockWakeWatch() { ++watchEnds; return key; }
} gpio;
struct {
  uint64_t firstDelay = 0, laterDelay = 0;
  unsigned getBatteryPercentage() const { return 77; }
  bool armXuesuClockTimer(uint64_t delay) { firstDelay = delay; return true; }
  void continueXuesuClockSleep(decltype(gpio)& keyWatch, uint64_t delay) {
    if (!keyWatch.endXuesuClockWakeWatch()) laterDelay = delay;
  }
} powerManager;
struct UITheme {
  static inline std::vector<xuesu_sleep_clock::Frame> paints;
  static void drawXuesuSleepClock(const GfxRenderer&, const xuesu_sleep_clock::Frame& frame) {
    paints.push_back(frame);
  }
};
''' + controller + r'''
int main() {
  using namespace xuesu_sleep_clock;
#if defined(_WIN32)
  _putenv_s("TZ", "UTC0");
  _tzset();
#else
  setenv("TZ", "UTC0", 1);
  tzset();
#endif
  static_assert(nextWakeDelayUs(61, 61500000) == 58700000);
  static_assert(nextWakeDelayUs(59, 61000000) == wakeMarginUs);
  static_assert(nextWakeDelayUs(60, 60000000) == 60200000);
  SETTINGS.sleepScreen = CrossPointSettings::COVER;
  assert(!drawBeforeSleep(renderer));
  for (uint8_t i = 0; i < static_cast<uint8_t>(Layout::Count); ++i) {
    clear();
    SETTINGS.sleepScreen = CrossPointSettings::CLOCK_MONTH_GRID + i;
    assert(drawBeforeSleep(renderer));
    assert(UITheme::paints.back().layout == static_cast<Layout>(i));
    assert(UITheme::paints.back().battery == 77);
    assert(UITheme::paints.back().environment.valid() == (i < 3));
    assert(modeForLayout(UITheme::paints.back().layout) == SETTINGS.sleepScreen);
  }
  assert(hal_environment::sampleCount == 3);
  clear();
  SETTINGS.sleepScreen = CrossPointSettings::CLOCK_FIREWORKS;
  assert(drawBeforeSleep(renderer));
  armFirstTimer();
  assert(powerManager.firstDelay == 650000);
  UITheme::paints.clear();
  renderer.updates.clear();

  // An early wake in the same minute must not repaint the panel.
  refreshAfterTimerWake(renderer);
  assert(UITheme::paints.empty() && powerManager.laterDelay == 650000);
  ++epoch;
  refreshAfterTimerWake(renderer);
  assert(display.begins == 1 && display.seeds == 1 && display.naps == 1);
  assert(UITheme::paints.size() == 2 && renderer.updates.size() == 1);
  assert(UITheme::paints[0].date.tm_mday == 31 && UITheme::paints[0].date.tm_min == 59);
  assert(UITheme::paints[1].date.tm_mday == 1 && UITheme::paints[1].date.tm_min == 0);
  assert(UITheme::paints[0].environment.temperature == 231 && UITheme::paints[1].environment.temperature == 231);
  assert(powerManager.laterDelay == 59650000);
  epoch += 60;
  refreshAfterTimerWake(renderer);
  assert(renderer.updates.back() == HalDisplay::FAST_REFRESH);
  assert(UITheme::paints[2].date.tm_min == 0 && UITheme::paints[3].date.tm_min == 1);
  assert(hal_environment::sampleCount == 4);

  // A confirm held at boot returns to the calling normal-resume code.
  gpio.key = true;
  powerManager.laterDelay = 0;
  epoch += 60;
  refreshAfterTimerWake(renderer);
  assert(UITheme::paints.size() == 4 && powerManager.laterDelay == 0);

  // Refresh only when due, preserving exactly the previously displayed numbers
  // for the old plane. A failed measurement becomes missing, not a made-up value.
  gpio.key = false;
  epoch += 180;
  hal_environment::nextReading = {-1, 731};
  refreshAfterTimerWake(renderer);
  assert(hal_environment::sampleCount == 5);
  assert(UITheme::paints[4].environment.temperature == 231 && UITheme::paints[4].environment.humidity == 567);
  assert(UITheme::paints[5].environment.temperature == -1 && UITheme::paints[5].environment.humidity == 731);
  epoch += 300;
  hal_environment::nextReading = {};
  refreshAfterTimerWake(renderer);
  assert(UITheme::paints[6].environment.valid() && !UITheme::paints[7].environment.valid());

  // A failed panel transaction or invalid clock must not arm a new cycle.
  powerManager.laterDelay = 0;
  epoch += 300;
  display.committed = false;
  hal_environment::nextReading = {284, 530};
  refreshAfterTimerWake(renderer);
  assert(powerManager.laterDelay == 0);
  assert(retained.humidity == hal_environment::Reading::missing);
  clear();
  epoch = 0;
  assert(!drawBeforeSleep(renderer));
  powerManager.firstDelay = 0;
  armFirstTimer();
  assert(powerManager.firstDelay == 0);

  // Almanac schedules only the next civil midnight and ignores minute wakes.
  display.committed = true;
  SETTINGS.sleepScreen = CrossPointSettings::CLOCK_ALMANAC;
  epoch = 1704067140;
  usec = 0;
  assert(drawBeforeSleep(renderer));
  armFirstTimer();
  assert(powerManager.firstDelay == 60200000);
  UITheme::paints.clear();
  epoch += 30;
  refreshAfterTimerWake(renderer);
  assert(UITheme::paints.empty() && powerManager.laterDelay == 30200000);
  epoch += 30;
  refreshAfterTimerWake(renderer);
  assert(UITheme::paints.size() == 2);
  assert(UITheme::paints.back().date.tm_mday == 1);
  assert(powerManager.laterDelay == 86400200000ULL);
  assert(!takeStandbyResume(true));  // Sleep from reading is not an app resume.

  // Manual standby overrides the saved sleep mode for this sleep only. A
  // browsed date is pinned and needs no timer, while confirm restores the app.
  SETTINGS.sleepScreen = CrossPointSettings::COVER;
  const FaceState pinned{.layout=Layout::Almanac, .pinnedDay=19723, .inverse=true};
  ++epoch;
  epoch += 86400;
  assert(drawBeforeSleep(renderer, pinned));
  assert(UITheme::paints.back().date.tm_mday == 1 && renderer.inversions == 1);
  powerManager.firstDelay = 0;
  armFirstTimer();
  assert(powerManager.firstDelay == 0 && SETTINGS.sleepScreen == CrossPointSettings::COVER);
  const auto manual = takeStandbyResume(true);
  assert(manual && manual->layout == Layout::Almanac && manual->pinnedDay == pinned.pinnedDay && manual->inverse);
  assert(!takeStandbyResume(true));

  const FaceState sketch{.layout=Layout::Sloppy, .styleSeed=123456, .inverse=true};
  assert(drawBeforeSleep(renderer, sketch));
  const int firstInversion = renderer.inversions;
  epoch += 60;
  refreshAfterTimerWake(renderer);
  assert(renderer.inversions == firstInversion + 2);
  assert(UITheme::paints.back().styleSeed == sketch.styleSeed);
  assert(!takeStandbyResume(false));  // An ordinary boot clears retained state.

  // A face selected in the standby app stays authoritative after timer wakes,
  // including reconstruction of the old frame and a fresh sensor sample.
  SETTINGS.sleepScreen = CrossPointSettings::CLOCK_SLOPPY;
  hal_environment::nextReading = {247, 402};
  assert(drawBeforeSleep(renderer, FaceState{.layout=Layout::Newspaper}));
  UITheme::paints.clear();
  hal_environment::nextReading = {249, 399};
  for (int minute = 1; minute <= 5; ++minute) {
    epoch += 60;
    refreshAfterTimerWake(renderer);
  }
  assert(UITheme::paints.size() == 10);
  for (const auto& paint : UITheme::paints) assert(paint.layout == Layout::Newspaper);
  assert(UITheme::paints[8].environment.humidity == 402 && UITheme::paints[9].environment.humidity == 399);
  assert(SETTINGS.sleepScreen == CrossPointSettings::CLOCK_SLOPPY);
  const auto weather = takeStandbyResume(true);
  assert(weather && weather->layout == Layout::Newspaper);

  // An older firmware's retained bytes cannot serve as a differential baseline.
  assert(drawBeforeSleep(renderer, FaceState{.layout=Layout::Newspaper}));
  retained.magic = 0x58534333;
  UITheme::paints.clear();
  powerManager.laterDelay = 0;
  epoch += 60;
  refreshAfterTimerWake(renderer);
  assert(UITheme::paints.empty() && powerManager.laterDelay == 0);
}
''', include_dirs=(ROOT / 'src', ROOT / 'lib/hal'))
