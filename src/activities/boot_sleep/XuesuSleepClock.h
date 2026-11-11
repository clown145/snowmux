#pragma once

#include <HalEnvironment.h>

#include <cstdint>
#include <ctime>
#include <optional>

class GfxRenderer;

namespace xuesu_sleep_clock {

// Separate IDs keep clock glyphs identical with or without a theme and SD card.
inline constexpr int fontSmall = 0x58534308;
inline constexpr int fontBody = 0x58534310;
inline constexpr int fontTitle = 0x58534312;

enum class Layout : uint8_t { MonthGrid, Newspaper, Fireworks, Sloppy, Almanac, Count };

inline constexpr uint32_t environmentIntervalSeconds = 5 * 60;

constexpr bool showsEnvironment(const Layout layout) {
  return layout == Layout::MonthGrid || layout == Layout::Newspaper || layout == Layout::Fireworks;
}

struct FaceState {
  Layout layout = Layout::Sloppy;
  uint32_t styleSeed = 1;
  int32_t pinnedDay = -1;  // Civil day ordinal, or -1 to follow today.
  bool inverse = false;
};

struct Frame {
  tm date;
  Layout layout;
  uint8_t battery;
  bool twelveHour;
  uint32_t styleSeed = 1;
  hal_environment::Reading environment{};
};

bool isSelected(uint8_t sleepMode);
std::optional<Layout> layoutForMode(uint8_t sleepMode);
uint8_t modeForLayout(Layout layout);
void clear();
std::optional<FaceState> takeStandbyResume(bool sleepWake);
bool drawBeforeSleep(GfxRenderer& renderer, const std::optional<FaceState>& standby = {});
void armFirstTimer();
void refreshAfterTimerWake(GfxRenderer& renderer);

}  // namespace xuesu_sleep_clock
