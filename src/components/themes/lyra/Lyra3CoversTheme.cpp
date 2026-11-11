#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/CompactHomeLayout.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;

#if FREEINK_DEVICE_XUESU_300
int compactTileWidth(Rect slot) {
  return (slot.width - 2 * compact_home::SIDE_INSET - 2 * compact_home::GAP) /
         Lyra3CoversMetrics::values.homeRecentBooksCount;
}
#endif

int coverHeightFor(const GfxRenderer& renderer, Rect slot) {
#if FREEINK_DEVICE_XUESU_300
  const int coverHeight =
      slot.height - 2 * hPaddingInSelection - compact_home::GAP - 2 * renderer.getLineHeight(SMALL_FONT_ID);
  return std::min(coverHeight, (compactTileWidth(slot) - 2 * hPaddingInSelection) * 5 / 3);
#else
  (void)renderer;
  (void)slot;
  return Lyra3CoversMetrics::values.homeCoverHeight;
#endif
}
}  // namespace

int Lyra3CoversTheme::recentBookIndexAt(int x, int screenWidth) const {
  // Three recent-book covers are laid out side by side: tile i spans
  // [contentSidePadding + tileWidth * i, contentSidePadding + tileWidth * (i + 1)).
  const int tileWidth = (screenWidth - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  if (tileWidth <= 0) {
    return 0;
  }
  const int index = (x - Lyra3CoversMetrics::values.contentSidePadding) / tileWidth;
  return std::clamp(index, 0, Lyra3CoversMetrics::values.homeRecentBooksCount - 1);
}

int Lyra3CoversTheme::homeCoverThumbHeight(const GfxRenderer& renderer, Rect slot) const {
#if FREEINK_DEVICE_XUESU_300
  return coverHeightFor(renderer, slot);
#else
  const int tileWidth = (renderer.getScreenWidth() - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  // Thumbs cover a (0.6*h, h) target box; in landscape the tile is wider than
  // 0.6 aspect, so request a taller thumb and let the draw crop vertically.
  return std::max(coverHeightFor(renderer, slot), (tileWidth - 2 * hPaddingInSelection) * 5 / 3 + 2);
#endif
}

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
#if FREEINK_DEVICE_XUESU_300
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int count = std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
  const int tileWidth = compactTileWidth(rect);
  const int tileStep = tileWidth + compact_home::GAP;
  const int coverHeight = coverHeightFor(renderer, rect);
  const int coverWidth = coverHeight * 3 / 5;
  const int titleHeight = 2 * renderer.getLineHeight(SMALL_FONT_ID);
  const int tileHeight = coverHeight + compact_home::GAP + titleHeight + 2 * hPaddingInSelection;
  const int groupWidth = count * tileStep - compact_home::GAP;
  const int startX = rect.x + (rect.width - groupWidth) / 2;
  const int tileY = rect.y + (rect.height - tileHeight) / 2;
  if (!bufferRestored) {
    for (int i = 0; i < count; ++i) {
      const Rect cover{startX + i * tileStep + (tileWidth - coverWidth) / 2, tileY + hPaddingInSelection, coverWidth,
                       coverHeight};
      bool drawn = false;
      if (!recentBooks[i].coverBmpPath.empty()) {
        HalFile file;
        const auto path = UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, coverHeight);
        if (Storage.openFileForRead("HOME", path, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) drawn = drawCoverThumbFill(renderer, bitmap, cover);
        }
      }
      if (!drawn) drawCoverPlaceholder(renderer, cover);
      renderer.drawRect(cover.x, cover.y, cover.width, cover.height);
    }
    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  for (int i = 0; i < count; ++i) {
    const int x = startX + i * tileStep;
    const bool selected = selectorIndex == i;
    if (selected) renderer.drawRoundedRect(x, tileY, tileWidth, tileHeight, 2, cornerRadius, true);
    const Rect title{x + hPaddingInSelection, tileY + hPaddingInSelection + coverHeight + compact_home::GAP,
                     tileWidth - 2 * hPaddingInSelection, titleHeight};
    drawCompactHomeText(renderer, title, SMALL_FONT_ID, recentBooks[i].title.c_str(), 2, selected, true);
  }
#else
  (void)bufferRestored;
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const int coverHeight = coverHeightFor(renderer, rect);
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount); i++) {
        const std::string& coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = false;
        int tileX = rect.x + Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        const Rect cover{tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                         coverHeight};
        if (!coverPath.empty()) {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, homeCoverThumbHeight(renderer, rect));

          // First time: load cover from SD and render
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              // Fill the fixed tile 1:1 and crop the overflow; the old crop
              // factor went negative for covers narrower than the tile.
              hasCover = drawCoverThumbFill(renderer, bitmap, cover);
            }
          }
        }
        // Draw either way
        if (hasCover)
          renderer.drawRect(cover.x, cover.y, cover.width, cover.height, true);
        else
          drawCoverPlaceholder(renderer, cover);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = rect.x + Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;

      auto titleLines = renderer.wrappedText(SMALL_FONT_ID, recentBooks[i].title.c_str(), maxLineWidth, 3);

      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int dynamicBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
      // Add a little padding below the text inside the selection box just like the top padding (5 + hPaddingSelection)
      const int dynamicTitleBoxHeight = dynamicBlockHeight + hPaddingInSelection + 5;

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + coverHeight + hPaddingInSelection, tileWidth, dynamicTitleBoxHeight,
                                 cornerRadius, false, false, true, true, Color::LightGray);
      }

      int currentY = tileY + coverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
#endif
}
