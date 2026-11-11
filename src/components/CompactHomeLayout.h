#pragma once

#include <algorithm>

#include "components/Rect.h"

namespace compact_home {
constexpr int GAP = 6;
constexpr int SIDE_INSET = 14;
constexpr int CARD_PADDING = 8;
constexpr int MENU_ICON_SIZE = 32;

inline int menuRowHeight(int lineHeight) { return std::max(MENU_ICON_SIZE + 4, lineHeight + 2 * GAP); }

struct Layout {
  Rect header;
  Rect cover;
  Rect menu;
};

inline Layout layout(Rect area, int headerHeight, int iconMenuHeight, int menuItems, int rowHeight) {
  // Fit complete menu rows and give the remaining height to the book preview.
  // Landscape preserves room for the title and author above three rows.
  const int available = std::max(0, area.height - headerHeight - 2 * GAP);
  const int rowLimit = area.width > area.height ? 3 : 4;
  const int maxRows = std::max(1, (available * 2 / 3 + GAP) / (rowHeight + GAP));
  const int rows = std::min({menuItems, rowLimit, maxRows});
  const int listHeight = rows > 0 ? rows * (rowHeight + GAP) - GAP : 0;
  const int menuHeight = std::min(available, iconMenuHeight > 0 ? iconMenuHeight : listHeight);
  const int menuY = area.y + area.height - menuHeight;
  const int coverY = area.y + headerHeight + GAP;
  return {Rect{area.x, area.y, area.width, headerHeight}, Rect{area.x, coverY, area.width, menuY - GAP - coverY},
          Rect{area.x, menuY, area.width, menuHeight}};
}

struct MenuPage {
  int start;
  int count;
  int selection;
  int capacity;
};

inline MenuPage menuPage(int count, int selection, int height, int rowHeight) {
  const int capacity = std::max(1, (height + GAP) / (rowHeight + GAP));
  const int start = std::max(0, selection) / capacity * capacity;
  return {start, std::min(capacity, count - start), selection - start, capacity};
}

inline Rect menuRow(Rect menu, int index, int height) {
  return Rect{menu.x + SIDE_INSET, menu.y + index * (height + GAP), menu.width - 2 * SIDE_INSET, height};
}

struct BookPreview {
  Rect card;
  Rect cover;
  Rect details;
};

inline BookPreview singleBookPreview(Rect area) {
  const Rect card{area.x + SIDE_INSET, area.y, area.width - 2 * SIDE_INSET, area.height};
  // Use the available height for the cover, with enough room left for metadata.
  const int coverHeight = std::min(card.height - 2 * CARD_PADDING, card.width / 2);
  const int coverWidth = coverHeight * 3 / 5;
  const Rect cover{card.x + CARD_PADDING, card.y + (card.height - coverHeight) / 2, coverWidth, coverHeight};
  const int textX = cover.x + cover.width + 2 * GAP;
  return {
      card, cover,
      Rect{textX, card.y + CARD_PADDING, card.x + card.width - CARD_PADDING - textX, card.height - 2 * CARD_PADDING}};
}
}  // namespace compact_home
