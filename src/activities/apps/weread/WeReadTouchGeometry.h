#pragma once

#include <algorithm>

#include "components/themes/BaseTheme.h"

struct WeReadDetailLayout {
  Rect cover;
  Rect metadata;
  Rect introduction;
  Rect actions;
  int actionStep;
  int actionGap;
};

inline WeReadDetailLayout weReadDetailLayout(const Rect& content, const ThemeMetrics& metrics,
                                             const int preferredActionStep, const int actionCount,
                                             const int lineHeight) {
  constexpr int coverWidth = 96;
  constexpr int coverHeight = 140;
  constexpr int metadataGap = 16;
  const int gap = metrics.verticalSpacing;
  const int introPreviewHeight = 2 * lineHeight + 4;
  const int fullHeight = coverHeight + 2 * gap + introPreviewHeight + actionCount * preferredActionStep;
  const bool compact = content.height < fullHeight;
  const int actionGap = compact ? 6 : 0;
  const int minimumActionStep = lineHeight + actionGap + 6;
  const int compactStep = std::max(minimumActionStep, content.height / (actionCount * 2));
  const int actionStep =
      std::min({preferredActionStep, compact ? compactStep : preferredActionStep, content.height / actionCount});
  const int actionsHeight = actionCount * actionStep;
  const int actionsY = content.y + content.height - actionsHeight;
  const int headerBudget = std::max(0, actionsY - content.y - 2 * gap);
  const int introReserve = std::min(introPreviewHeight, headerBudget / 2);
  const int innerWidth = std::max(0, content.width - 2 * metrics.contentSidePadding);
  const int left = content.x + metrics.contentSidePadding;
  const int heroHeight = std::min(coverHeight, headerBudget - introReserve);
  const int scaledWidth = std::min(innerWidth / 2, (heroHeight * coverWidth + coverHeight / 2) / coverHeight);
  const int metaX = left + (scaledWidth > 4 ? scaledWidth + metadataGap : 0);
  const int introY = std::min(actionsY, content.y + heroHeight + gap);
  return {Rect{left, content.y, scaledWidth, heroHeight},
          Rect{metaX, content.y, std::max(0, left + innerWidth - metaX), heroHeight},
          Rect{left, introY, innerWidth, std::max(0, actionsY - gap - introY)},
          Rect{content.x, actionsY, content.width, actionsHeight},
          actionStep,
          actionGap};
}

struct WeReadShelfGridLayout {
  int columns = 1;
  int rows = 1;
  int itemsPerPage = 1;
  int coverWidth = 1;
  int coverHeight = 1;
  int itemHeight = 1;
  int columnGap = 0;
  int rowGap = 0;
  int titleGap = 0;
  int availableX = 0;
  int availableWidth = 1;
  int itemWidth = 0;  // Zero keeps the legacy title and hit widths tied to the cover.
  int titleLines = 1;
};

inline WeReadShelfGridLayout weReadCompactShelfLayout(const Rect& content, const int sidePadding, const int spacing,
                                                      const int titleLineHeight, const int titleLines,
                                                      const int thumbWidth, const int thumbHeight) {
  WeReadShelfGridLayout layout;
  constexpr int columns = 2;
  const int gap = std::max(10, spacing / 2);  // Two 2 px selection margins leave at least 6 px between tiles.
  layout.columns = columns;
  layout.itemsPerPage = columns;
  layout.columnGap = gap;
  layout.titleGap = std::max(4, spacing / 2);
  layout.availableX = content.x + sidePadding;
  layout.availableWidth = std::max(1, content.width - 2 * sidePadding);
  layout.itemWidth = std::max(1, (layout.availableWidth - 3 * gap) / columns);
  layout.titleLines = titleLines;

  const int titleHeight = titleLineHeight * titleLines;
  const int maxCoverHeight = std::max(1, content.height - 2 * gap - layout.titleGap - titleHeight);
  const float scale = std::min(
      {1.0f, static_cast<float>(layout.itemWidth) / thumbWidth, static_cast<float>(maxCoverHeight) / thumbHeight});
  layout.coverWidth = std::max(1, static_cast<int>(thumbWidth * scale));
  layout.coverHeight = std::max(1, static_cast<int>(thumbHeight * scale));
  layout.itemHeight = layout.coverHeight + layout.titleGap + titleHeight;
  return layout;
}

struct WeReadShelfItemGeometry {
  Rect cover;
  Rect title;
  Rect hit;
};

inline WeReadShelfItemGeometry weReadShelfItemGeometry(const Rect& content, const WeReadShelfGridLayout& layout,
                                                       const int pageStart, const int pageEnd, const int index) {
  const int pageCount = pageEnd - pageStart;
  if (layout.columns <= 0 || pageCount <= 0 || index < pageStart || index >= pageEnd) return {Rect{}, Rect{}, Rect{}};

  const int item = index - pageStart;
  const int row = item / layout.columns;
  const int column = item % layout.columns;
  const int visibleRows = (pageCount + layout.columns - 1) / layout.columns;
  const int visibleHeight = visibleRows * layout.itemHeight + std::max(0, visibleRows - 1) * layout.rowGap;
  const int startY = content.y + std::max(0, (content.height - visibleHeight) / 2);
  const int rowItems = std::min(layout.columns, pageCount - row * layout.columns);
  const int itemWidth = layout.itemWidth > 0 ? layout.itemWidth : layout.coverWidth;
  const int rowWidth = rowItems * itemWidth + (rowItems - 1) * layout.columnGap;
  const int itemX =
      layout.availableX + std::max(0, (layout.availableWidth - rowWidth) / 2) + column * (itemWidth + layout.columnGap);
  const int coverY = startY + row * (layout.itemHeight + layout.rowGap);
  const Rect cover{itemX + (itemWidth - layout.coverWidth) / 2, coverY, layout.coverWidth, layout.coverHeight};
  const int titleY = coverY + cover.height + layout.titleGap;
  const Rect title{itemX, titleY, itemWidth, layout.itemHeight - cover.height - layout.titleGap};
  return {cover, title, Rect{itemX - 2, coverY - 2, itemWidth + 4, layout.itemHeight + 4}};
}

inline int weReadShelfIndexFromPoint(const Rect& content, const WeReadShelfGridLayout& layout, const int selectedIndex,
                                     const int itemCount, const int x, const int y) {
  if (itemCount <= 0 || layout.itemsPerPage <= 0) return -1;
  const int selected = std::clamp(selectedIndex, 0, itemCount - 1);
  const int pageStart = selected / layout.itemsPerPage * layout.itemsPerPage;
  const int pageEnd = std::min(pageStart + layout.itemsPerPage, itemCount);
  for (int index = pageStart; index < pageEnd; ++index) {
    const Rect hit = weReadShelfItemGeometry(content, layout, pageStart, pageEnd, index).hit;
    if (x >= hit.x && x < hit.x + hit.width && y >= hit.y && y < hit.y + hit.height) return index;
  }
  return -1;
}
