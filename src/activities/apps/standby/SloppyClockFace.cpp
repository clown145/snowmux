#include "SloppyClockFace.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>
#include <time.h>

#include <cstdio>

#include "StandbyTime.h"

namespace {
void drawClock(GfxRenderer& renderer, const Rect& viewport, unsigned hh, unsigned mm, const sloppy::Style& style,
               const sloppy::Seeds& seeds) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02u\n%02u", hh, mm);
  sloppy::draw(renderer, style, seeds, buf, sloppy::Bounds{viewport.x, viewport.y, viewport.width, viewport.height});
}
}  // namespace

void SloppyClockFace::onEnter() {
  style_ = makeUniqueNoThrow<sloppy::Style>();
  seeds_ = makeUniqueNoThrow<sloppy::Seeds>();
  if (!style_ || !seeds_) {
    LOG_ERR("STANDBY", "OOM allocating sloppy clock state");
    return;  // tick()/render() will see nullptr and skip work
  }
  startMs_ = millis();
  lastMin_ = -1;
  regenerate(esp_random() ^ static_cast<uint32_t>(startMs_));
}

void SloppyClockFace::onExit() {
  seeds_.reset();
  style_.reset();
}

void SloppyClockFace::onShake(uint32_t seed) {
  if (!style_ || !seeds_) return;
  regenerate(seed);
}

void SloppyClockFace::onPagePrev() { onShake(esp_random() ^ static_cast<uint32_t>(millis())); }

void SloppyClockFace::onPageNext() { onShake(esp_random() ^ static_cast<uint32_t>(millis())); }

void SloppyClockFace::regenerate(uint32_t seed) {
#if FREEINK_DEVICE_XUESU_300
  styleSeed_ = seed;
#endif
  sloppy::rollStyle(seed, *style_);
  sloppy::prepareSeeds(seed, *style_, *seeds_);
  lastMin_ = -1;
}

StandbyFace::TickResult SloppyClockFace::tick() {
  if (!style_ || !seeds_) return TickResult::None;
  const uint32_t nowMin = standby_time::getMinuteTick(startMs_);
  if (static_cast<int32_t>(nowMin) == lastMin_) return TickResult::None;
  lastMin_ = static_cast<int32_t>(nowMin);

  const uint8_t currentBucket = standby_time::getTenMinuteBucket(startMs_);
  const bool cleanGhosting = standby_time::didTenMinuteBucketChange(lastTenMinuteBucket_, currentBucket);
  lastTenMinuteBucket_ = currentBucket;
  return cleanGhosting ? TickResult::RedrawWithGhostCleanup : TickResult::Redraw;
}

void SloppyClockFace::render(GfxRenderer& renderer, const Rect& viewport) {
  if (!style_ || !seeds_) return;
  unsigned hh = 0;
  unsigned mm = 0;
  standby_time::getNowHHMM(startMs_, hh, mm);
  drawClock(renderer, viewport, hh, mm, *style_, *seeds_);
}

#if FREEINK_DEVICE_XUESU_300
void SloppyClockFace::renderSleep(GfxRenderer& renderer, const Rect& viewport, const std::tm& time,
                                  const uint32_t styleSeed) {
  sloppy::Style style{};
  sloppy::Seeds seeds{};
  sloppy::rollStyle(styleSeed, style);
  sloppy::prepareSeeds(styleSeed, style, seeds);
  drawClock(renderer, viewport, time.tm_hour, time.tm_min, style, seeds);
}
#endif

uint32_t SloppyClockFace::secondsUntilNextWake() const {
  uint32_t sec;
  if (standby_time::isSynced()) {
    const time_t now = time(nullptr);
    sec = static_cast<uint32_t>(now % 60);
  } else {
    sec = static_cast<uint32_t>(((millis() - startMs_) / 1000u) % 60u);
  }
  const uint32_t remaining = (sec == 0) ? 60u : (60u - sec);
  return remaining < 1u ? 1u : remaining;
}
