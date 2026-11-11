#if FREEINK_DEVICE_XUESU_300

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Rect.h"
#include "UITheme.h"
#ifdef ENABLE_CHINESE_VERSION
#include "activities/apps/standby/ChineseCalendarFace.h"
#endif
#include "activities/apps/standby/SloppyClockFace.h"
#include "activities/boot_sleep/XuesuSleepClock.h"

namespace {

using xuesu_sleep_clock::Frame;
using xuesu_sleep_clock::Layout;

constexpr StrId kWeekday[]{StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE,
                           StrId::STR_CAL_WEEKDAY_WED, StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI,
                           StrId::STR_CAL_WEEKDAY_SAT};

constexpr bool leapYear(const int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

Rect band(const Rect& r, const int topPercent, const int bottomPercent) {
  const int y = r.height * topPercent / 100;
  return Rect(r.x, r.y + y, r.width, r.height * bottomPercent / 100 - y);
}

void centered(const GfxRenderer& r, const Rect& box, const char* text, const int font, const bool black = true,
              const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int fit = r.getTextWidth(font, text, style) <= box.width ? font : xuesu_sleep_clock::fontSmall;
  const int y = box.y + std::max(0, (box.height - r.getLineHeight(fit)) / 2);
  UITheme::drawCenteredText(r, box, fit, y, text, black, style);
}

void line(const GfxRenderer& r, const Rect& box, const int width = 1, const bool black = true) {
  r.fillRect(box.x, box.y, box.width, width, black);
}

// Scalable B/W time digits. This lives in Flash, with no font rasterization or
// scratch bitmap on the minute-wake path.
constexpr uint8_t kDigits[10][7] = {
    {14, 17, 17, 17, 17, 17, 14}, {4, 12, 4, 4, 4, 4, 14},    {14, 17, 1, 2, 4, 8, 31},   {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
};

void drawTime(const GfxRenderer& r, const Rect& box, const Frame& frame, const bool black = true) {
  constexpr int kWidth = 27;  // Four 5-column glyphs, a 3-column colon, four gaps.
  const int scale = std::min(box.width / kWidth, box.height / 7);
  if (scale <= 0) return;
  const int hour = frame.twelveHour ? (frame.date.tm_hour + 11) % 12 + 1 : frame.date.tm_hour;
  const int values[]{hour / 10, hour % 10, frame.date.tm_min / 10, frame.date.tm_min % 10};
  int x = box.x + (box.width - kWidth * scale) / 2;
  const int y = box.y + (box.height - 7 * scale) / 2;
  for (int digit = 0; digit < 4; ++digit) {
    if (digit == 2) {
      r.fillRect(x + scale, y + 2 * scale, scale, scale, black);
      r.fillRect(x + scale, y + 4 * scale, scale, scale, black);
      x += 4 * scale;
    }
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 5; ++col) {
        if (kDigits[values[digit]][row] & (1 << (4 - col))) {
          r.fillRect(x + col * scale, y + row * scale, scale, scale, black);
        }
      }
    }
    x += 6 * scale;
  }
}

void dateLine(const GfxRenderer& r, const Rect& box, const Frame& frame, const bool black = true) {
  char text[56];
  const char* meridiem = !frame.twelveHour ? "" : frame.date.tm_hour < 12 ? "  AM" : "  PM";
  std::snprintf(text, sizeof(text), "%04d-%02d-%02d  %s%s", frame.date.tm_year + 1900, frame.date.tm_mon + 1,
                frame.date.tm_mday, I18N.get(kWeekday[frame.date.tm_wday]), meridiem);
  centered(r, box, text, xuesu_sleep_clock::fontSmall, black);
}

void stats(const GfxRenderer& r, const Rect& box, const Frame& frame, const bool black = true) {
  const int column = box.width / 3;
  const StrId labels[]{StrId::STR_SLEEP_CLOCK_TEMPERATURE, StrId::STR_SLEEP_CLOCK_HUMIDITY,
                       StrId::STR_SLEEP_CLOCK_BATTERY};
  char charge[8];
  std::snprintf(charge, sizeof(charge), "%u%%", static_cast<unsigned>(frame.battery));
  char temperature[12] = "--";
  char humidity[8] = "--";
  if (frame.environment.valid()) {
    const int temp = frame.environment.temperature;
    const int magnitude = temp < 0 ? -temp : temp;
    const unsigned rh = frame.environment.humidity;
    std::snprintf(temperature, sizeof(temperature), "%s%d.%d°C", temp < 0 ? "-" : "", magnitude / 10, magnitude % 10);
    std::snprintf(humidity, sizeof(humidity), "%u.%u%%", rh / 10, rh % 10);
  }
  const char* values[]{temperature, humidity, charge};
  for (int i = 0; i < 3; ++i) {
    const Rect cell(box.x + i * column, box.y, column, box.height);
    if (i) r.fillRect(cell.x, cell.y + 4, 1, std::max(1, cell.height - 8), black);
    centered(r, band(cell, 0, 50), I18N.get(labels[i]), xuesu_sleep_clock::fontSmall, black);
    centered(r, band(cell, 50, 100), values[i], xuesu_sleep_clock::fontSmall, black, EpdFontFamily::BOLD);
  }
}

void yearCountdown(const GfxRenderer& r, const Rect& box, const Frame& frame) {
  const int year = frame.date.tm_year + 1900;
  char text[64];
  std::snprintf(text, sizeof(text), tr(STR_SLEEP_CLOCK_YEAR_REMAINING), year + 1,
                (leapYear(year) ? 366 : 365) - frame.date.tm_yday);
  centered(r, box, text, xuesu_sleep_clock::fontSmall);
}

void monthGrid(const GfxRenderer& r, const Rect& page, const Frame& frame) {
  drawTime(r, band(page, 1, 18), frame);
  dateLine(r, band(page, 19, 25), frame);
  const Rect calendar = band(page, 27, 79);
  const int rowHeight = calendar.height / 7;
  const int colWidth = calendar.width / 7;
  const int first = (frame.date.tm_wday - (frame.date.tm_mday - 1) % 7 + 7) % 7;
  constexpr int kDays[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int total = kDays[frame.date.tm_mon] + (frame.date.tm_mon == 1 && leapYear(frame.date.tm_year + 1900));
  r.drawRect(calendar.x, calendar.y, colWidth * 7, rowHeight * 7);
  for (int col = 0; col < 7; ++col) {
    const int x = calendar.x + col * colWidth;
    if (col) r.fillRect(x, calendar.y, 1, rowHeight * 7);
    const char* day = I18N.get(kWeekday[col]);
    constexpr char kChineseWeekPrefix[] = "星期";
    if (std::strncmp(day, kChineseWeekPrefix, sizeof(kChineseWeekPrefix) - 1) == 0) {
      day += sizeof(kChineseWeekPrefix) - 1;
    }
    centered(r, Rect(x, calendar.y, colWidth, rowHeight), day, xuesu_sleep_clock::fontSmall, true, EpdFontFamily::BOLD);
  }
  for (int week = 0; week < 6; ++week) {
    const int y = calendar.y + (week + 1) * rowHeight;
    line(r, Rect(calendar.x, y, colWidth * 7, 1));
    for (int col = 0; col < 7; ++col) {
      const int day = week * 7 + col - first + 1;
      if (day < 1 || day > total) continue;
      const Rect cell(calendar.x + col * colWidth, y, colWidth, rowHeight);
      const bool today = day == frame.date.tm_mday;
      if (today) r.fillRect(cell.x + 2, cell.y + 2, cell.width - 3, cell.height - 3);
      char value[4];
      std::snprintf(value, sizeof(value), "%d", day);
      centered(r, cell, value, xuesu_sleep_clock::fontSmall, !today);
    }
  }
  stats(r, band(page, 80, 92), frame);
  yearCountdown(r, band(page, 93, 100), frame);
}

void newspaper(const GfxRenderer& r, const Rect& page, const Frame& frame) {
  line(r, band(page, 2, 3), 3);
  centered(r, band(page, 6, 19), tr(STR_SLEEP_CLOCK_INDOOR), xuesu_sleep_clock::fontTitle, true, EpdFontFamily::BOLD);
  line(r, band(page, 20, 21));
  dateLine(r, band(page, 22, 31), frame);
  line(r, band(page, 32, 33));
  drawTime(r, band(page, 34, 60), frame);
  line(r, band(page, 62, 63), 3);
  line(r, band(page, 64, 65));
  const Rect data = band(page, 68, 84);
  r.drawRect(data.x, data.y, data.width, data.height);
  stats(r, data, frame);
  yearCountdown(r, band(page, 88, 98), frame);
  line(r, band(page, 99, 100));
}

void burst(const GfxRenderer& r, const int x, const int y, const int radius) {
  constexpr int rays[][2]{{0, -6}, {4, -4}, {6, 0}, {4, 4}, {0, 6}, {-4, 4}, {-6, 0}, {-4, -4}};
  for (const auto& ray : rays) {
    const int dx = ray[0] * radius / 6;
    const int dy = ray[1] * radius / 6;
    r.drawLine(x + dx / 3, y + dy / 3, x + dx, y + dy, false);
    r.fillRect(x + dx + dx / 4 - 1, y + dy + dy / 4 - 1, 2, 2, false);
  }
}

void fireworks(const GfxRenderer& r, const Rect& page, const Frame& frame) {
  r.fillRect(page.x, page.y, page.width, page.height);
  const Rect title = band(page, 6, 20);
  centered(r, title, tr(STR_SLEEP_CLOCK_NATIONAL_DAY), xuesu_sleep_clock::fontTitle, false, EpdFontFamily::BOLD);
  burst(r, page.x + page.width / 7, title.y + title.height / 2, page.width / 14);
  burst(r, page.x + page.width * 6 / 7, title.y + title.height / 2, page.width / 17);
  char years[24];
  const int year = frame.date.tm_year + 1900;
  std::snprintf(years, sizeof(years), "1949   /   %d", year);
  centered(r, band(page, 22, 30), years, xuesu_sleep_clock::fontSmall, false);
  line(r, band(page, 31, 32), 1, false);
  drawTime(r, band(page, 34, 57), frame, false);
  dateLine(r, band(page, 59, 67), frame, false);
  line(r, band(page, 69, 70), 1, false);
  stats(r, band(page, 72, 86), frame, false);
  char anniversary[84];
  std::snprintf(anniversary, sizeof(anniversary), tr(STR_SLEEP_CLOCK_ANNIVERSARY), year - 1949);
  centered(r, band(page, 89, 98), anniversary, xuesu_sleep_clock::fontSmall, false);
}

}  // namespace

void UITheme::drawXuesuSleepClock(GfxRenderer& renderer, const Frame& frame) {
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  renderer.clearScreen();
  drawXuesuSleepClock(
      renderer, frame,
      Rect(left, top, renderer.getScreenWidth() - left - right, renderer.getScreenHeight() - top - bottom));
}

void UITheme::drawXuesuSleepClock(GfxRenderer& renderer, const Frame& frame, const Rect& viewport) {
  constexpr int pad = 10;
  const Rect page(viewport.x + pad, viewport.y + pad, viewport.width - 2 * pad, viewport.height - 2 * pad);
  switch (frame.layout) {
    case Layout::MonthGrid:
      monthGrid(renderer, page, frame);
      break;
    case Layout::Newspaper:
      newspaper(renderer, page, frame);
      break;
    case Layout::Fireworks:
      fireworks(renderer, page, frame);
      break;
    case Layout::Sloppy:
      SloppyClockFace::renderSleep(renderer, viewport, frame.date, frame.styleSeed);
      break;
    case Layout::Almanac:
#ifdef ENABLE_CHINESE_VERSION
      ChineseCalendarFace::renderSleep(renderer, viewport, frame.date);
#endif
      break;
    case Layout::Count:
      break;
  }
}

#endif
