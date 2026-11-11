#pragma once

#include <algorithm>
#include <cstdint>

#include "components/themes/BaseTheme.h"

enum class InxRecentLayout : uint8_t { Flow, Grid, List, Icons, Cover, Count };

namespace InxRecentGeometry {
#if FREEINK_DEVICE_XUESU_300
inline constexpr int footerReservedHeight = 24;
#else
inline constexpr int footerReservedHeight = UiHighDpiProfile::enabled ? UiHighDpiProfile::statusHeight : 40;
#endif

constexpr Rect flowCarouselRect(const Rect content) {
  return Rect{content.x, content.y + 5, content.width, std::max(1, content.height * 50 / 100)};
}

struct CompactFlow {
  Rect cover;
  Rect title;
  Rect progress;
};

constexpr CompactFlow compactFlow(const Rect& content, const int titleLineHeight, const int progressLineHeight) {
  constexpr int gap = 8;
  const int x = content.x + gap;
  const int width = std::max(0, content.width - 2 * gap);
  const int bottom = content.y + content.height;
  const int progressY = bottom - progressLineHeight;
  const int titleHeight = 2 * titleLineHeight;
  const int titleY = progressY - gap - titleHeight;
  return {Rect{x, content.y, width, std::max(0, titleY - gap - content.y)},
          Rect{x, titleY, width, titleHeight}, Rect{x, progressY, width, progressLineHeight}};
}

constexpr int contentHeight(const int screenHeight, const int top, const int buttonHintsHeight) {
  return std::max(0, screenHeight - top - std::max(buttonHintsHeight, footerReservedHeight));
}

inline Rect contentRect(const Rect& safeArea, const ThemeMetrics& metrics) {
  const int top = std::clamp(metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing, 0, safeArea.height);
  return Rect{safeArea.x, safeArea.y + top, safeArea.width,
              contentHeight(safeArea.height, top, metrics.buttonHintsHeight)};
}

inline Rect batteryRect(const Rect& safeArea) {
#if FREEINK_DEVICE_READPICO
  constexpr int bottomOffset = 24;
#elif FREEINK_DEVICE_XUESU_300
  constexpr int bottomOffset = footerReservedHeight;
#else
  constexpr int bottomOffset = 30;
#endif
  return Rect{safeArea.x + safeArea.width - 12 - 15, safeArea.y + safeArea.height - bottomOffset, 15, 12};
}

struct ListLayout {
  int rows;
  int rowHeight;
  int titleTop;
  int authorTop;
  int progressTop;
};

constexpr ListLayout listLayout(const Rect& content, const int titleHeight, const int authorHeight) {
  constexpr int padding = 8;
  constexpr int gap = 6;
  constexpr int progressHeight = 6;
  const int minimumRowHeight = padding * 2 + titleHeight + authorHeight + gap * 2 + progressHeight;
  const int rows = std::clamp(content.height / minimumRowHeight, 1, 5);
  const int rowHeight = content.height / rows;
  return {rows, rowHeight, padding, padding + titleHeight + gap, rowHeight - padding - progressHeight};
}

constexpr int itemsPerPage(const InxRecentLayout layout, const int listRows = 5) {
  switch (layout) {
    case InxRecentLayout::Flow:
    case InxRecentLayout::Cover:
      return 1;
    case InxRecentLayout::Grid:
      return 4;
    case InxRecentLayout::List:
      return std::max(1, listRows);
    case InxRecentLayout::Icons:
      return 9;
    case InxRecentLayout::Count:
      return 1;
  }
  return 1;
}

constexpr int pageStart(const int selected, const int count, const InxRecentLayout layout, const int listRows = 5) {
  if (count <= 0) return 0;
  const int clamped = std::clamp(selected, 0, count - 1);
  const int pageItems = itemsPerPage(layout, listRows);
  return clamped / pageItems * pageItems;
}

inline int indexFromPoint(const Rect& content, const int x, const int y, const int selected, const int count,
                          const InxRecentLayout layout, const int listRows = 5) {
  if (count <= 0 || x < content.x || x >= content.x + content.width || y < content.y || y >= content.y + content.height)
    return -1;

  int columns = 1;
  int rows = 1;
  switch (layout) {
    case InxRecentLayout::Grid:
      columns = 2;
      rows = 2;
      break;
    case InxRecentLayout::List:
      rows = std::max(1, listRows);
      break;
    case InxRecentLayout::Icons:
      columns = 3;
      rows = 3;
      break;
    case InxRecentLayout::Flow:
    case InxRecentLayout::Cover:
      return std::clamp(selected, 0, count - 1);
    case InxRecentLayout::Count:
      return -1;
  }

  const int column = std::min(columns - 1, (x - content.x) / std::max(1, content.width / columns));
  const int row = std::min(rows - 1, (y - content.y) / std::max(1, content.height / rows));
  const int index = pageStart(selected, count, layout, listRows) + row * columns + column;
  return index < count ? index : -1;
}
}  // namespace InxRecentGeometry
