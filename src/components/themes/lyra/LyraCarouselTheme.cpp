#include "LyraCarouselTheme.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/CompactHomeLayout.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

namespace {
constexpr int kCenterCoverWidth = 380;
constexpr int kCenterCoverHeight = 540;
constexpr int kCoverTopPadding = 32;
constexpr int kCornerRadius = 6;
constexpr int kActiveOutlineWidth = 3;
constexpr int kDotSize = 8;
constexpr int kDotGap = 6;
constexpr int kMenuIconSize = 32;
constexpr int kMenuIconPadding = 14;
constexpr int kMenuHighlightPadding = 7;

#if FREEINK_DEVICE_XUESU_300
struct CompactCarousel {
  Rect cover;
  Rect dots;
  Rect title;
  Rect author;
};

CompactCarousel compactCarousel(const GfxRenderer& renderer, Rect area) {
  constexpr int gap = compact_home::GAP;
  constexpr int inset = compact_home::SIDE_INSET;
  const int titleHeight = 2 * renderer.getLineHeight(UI_12_FONT_ID);
  const int authorHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int metadataHeight = titleHeight + gap + authorHeight;
  const int width = area.width - 2 * inset;
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  const int coverColumnWidth = landscape ? width * 2 / 5 : width;
  const int artHeight = area.height - gap - kDotSize - (landscape ? 0 : gap + metadataHeight);
  const int coverHeight = std::min(artHeight, coverColumnWidth * kCenterCoverHeight / kCenterCoverWidth);
  const int coverWidth = coverHeight * kCenterCoverWidth / kCenterCoverHeight;
  const int left = area.x + inset;
  const int coverX = left + (coverColumnWidth - coverWidth) / 2;
  const int coverY = area.y + (artHeight - coverHeight) / 2;
  const int dotsY = area.y + artHeight + gap;
  const int textX = landscape ? left + coverColumnWidth + 2 * gap : left;
  const int textWidth = landscape ? width - coverColumnWidth - 2 * gap : width;
  const int textY = landscape ? area.y + (area.height - metadataHeight) / 2 : dotsY + kDotSize + gap;
  return {Rect{coverX, coverY, coverWidth, coverHeight}, Rect{left, dotsY, coverColumnWidth, kDotSize},
          Rect{textX, textY, textWidth, titleHeight}, Rect{textX, textY + titleHeight + gap, textWidth, authorHeight}};
}
#endif

void drawAppsMenuIcon(const GfxRenderer& renderer, int x, int y, bool selected) {
  constexpr int kInset = 2;
  constexpr int kCellSize = 13;
  constexpr int kCellGap = 2;
  static_assert(kInset * 2 + kCellSize * 2 + kCellGap == kMenuIconSize);

  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 2; ++column) {
      renderer.drawRoundedRect(x + kInset + column * (kCellSize + kCellGap), y + kInset + row * (kCellSize + kCellGap),
                               kCellSize, kCellSize, 2, 2, !selected);
    }
  }
}

void drawCover(const GfxRenderer& renderer, const RecentBook& book, int x, int y, int width, int height,
               int thumbHeight) {
  if (!book.coverBmpPath.empty()) {
    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
    HalFile file;
    if (Storage.openFileForRead("HOME", coverPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        const float scale = std::min(
            {1.0f, static_cast<float>(width) / bitmap.getWidth(), static_cast<float>(height) / bitmap.getHeight()});
        const int drawWidth = std::max(1, static_cast<int>(bitmap.getWidth() * scale));
        const int drawHeight = std::max(1, static_cast<int>(bitmap.getHeight() * scale));
        const int drawX = x + (width - drawWidth) / 2;
        const int drawY = y + (height - drawHeight) / 2;
        if (renderer.drawBitmap(bitmap, drawX, drawY, drawWidth, drawHeight) || !FREEINK_DEVICE_XUESU_300) {
          renderer.maskRoundedRectOutsideCorners(drawX, drawY, drawWidth, drawHeight, kCornerRadius, Color::White);
          return;
        }
        // A truncated cover may fail after drawing several rows.
        renderer.fillRect(x, y, width, height, false);
      }
    }
  }

  renderer.drawRoundedRect(x, y, width, height, 1, kCornerRadius, true);
  renderer.fillRoundedRect(x, y + height / 3, width, 2 * height / 3, kCornerRadius, false, false, true, true,
                           Color::Black);
  if (width >= kMenuIconSize && height / 3 >= kMenuIconSize)
    renderer.drawIcon(CoverIcon, x + (width - kMenuIconSize) / 2, y + height / 6 - kMenuIconSize / 2, kMenuIconSize);
}
}  // namespace

int LyraCarouselTheme::homeCoverThumbHeight(const GfxRenderer& renderer, Rect slot) const {
#if FREEINK_DEVICE_XUESU_300
  return compactCarousel(renderer, slot).cover.height;
#else
  (void)renderer;
  (void)slot;
  return LyraCarouselMetrics::values.homeCoverHeight;
#endif
}

void LyraCarouselTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect,
                                            const std::vector<RecentBook>& recentBooks, int selectorIndex,
                                            bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                            std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  (void)storeCoverBuffer;
  coverBufferStored = false;
  if (recentBooks.empty()) {
    lastCenterIndex_ = -1;
    drawEmptyRecents(renderer, rect);
    coverRendered = true;
    return;
  }

  const GfxRenderer::ClipScope clip(renderer, rect.x, rect.y, rect.width, rect.height);
  const int bookCount = static_cast<int>(recentBooks.size());
  const bool coversFocused = selectorIndex >= 0 && selectorIndex < bookCount;
  int centerIndex = coversFocused ? selectorIndex : lastCenterIndex_;
  centerIndex = std::clamp(centerIndex, 0, bookCount - 1);

  if (centerIndex != lastCenterIndex_) {
    coverRendered = false;
    coverBufferStored = false;
    lastCenterIndex_ = centerIndex;
  }

#if FREEINK_DEVICE_XUESU_300
  const auto [cover, dots, title, author] = compactCarousel(renderer, rect);
  const auto& book = recentBooks[centerIndex];
  if (!coverRendered) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    drawCover(renderer, book, cover.x, cover.y, cover.width, cover.height, cover.height);

    if (bookCount > 1) {
      const int dotsWidth = bookCount * kDotSize + (bookCount - 1) * kDotGap;
      int x = dots.x + (dots.width - dotsWidth) / 2;
      for (int i = 0; i < bookCount; ++i) {
        if (i == centerIndex) {
          renderer.fillRect(x, dots.y, kDotSize, kDotSize);
        } else {
          renderer.drawRect(x, dots.y, kDotSize, kDotSize);
        }
        x += kDotSize + kDotGap;
      }
    }

    Rect titleBox = title;
    if (book.author.empty()) titleBox.height = author.y + author.height - title.y;
    drawCompactHomeText(renderer, titleBox, UI_12_FONT_ID, book.title.c_str(), 2, true, true);
    drawCompactHomeText(renderer, author, UI_10_FONT_ID, book.author.c_str(), 1, false, true);
    coverRendered = true;
  }
  if (coversFocused) {
    renderer.drawRoundedRect(cover.x, cover.y, cover.width, cover.height, kActiveOutlineWidth, kCornerRadius, true);
  }
#else
  const int coverHeight = kCenterCoverHeight;
  const int coverWidth = kCenterCoverWidth;
  const int centerX = (renderer.getScreenWidth() - coverWidth) / 2;
  const int centerY = rect.y + kCoverTopPadding;

  if (!coverRendered) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

    drawCover(renderer, recentBooks[centerIndex], centerX, centerY, coverWidth, coverHeight,
              homeCoverThumbHeight(renderer, rect));

    const int dotsY = centerY + coverHeight + 8;
    const int dotsWidth = bookCount * kDotSize + (bookCount - 1) * kDotGap;
    int dotX = centerX + (coverWidth - dotsWidth) / 2;
    for (int i = 0; i < bookCount; ++i) {
      if (i == centerIndex) {
        renderer.fillRect(dotX, dotsY, kDotSize, kDotSize, true);
      } else {
        renderer.drawRect(dotX, dotsY, kDotSize, kDotSize, true);
      }
      dotX += kDotSize + kDotGap;
    }

    int textY = dotsY + kDotSize + 6;
    const int textX = centerX;
    const int textWidth = coverWidth;
    if (!recentBooks[centerIndex].author.empty()) {
      const std::string author =
          renderer.truncatedText(UI_10_FONT_ID, recentBooks[centerIndex].author.c_str(), textWidth);
      const int authorWidth = renderer.getTextWidth(UI_10_FONT_ID, author.c_str());
      renderer.drawText(UI_10_FONT_ID, textX + (textWidth - authorWidth) / 2, textY, author.c_str(), true);
      textY += renderer.getLineHeight(UI_10_FONT_ID) + 2;
    }

    const std::string title =
        renderer.truncatedText(UI_12_FONT_ID, recentBooks[centerIndex].title.c_str(), textWidth, EpdFontFamily::BOLD);
    const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, textX + (textWidth - titleWidth) / 2, textY, title.c_str(), true,
                      EpdFontFamily::BOLD);

    coverRendered = true;
  }

  if (coversFocused) {
    renderer.drawRoundedRect(centerX, centerY, coverWidth, coverHeight, kActiveOutlineWidth, kCornerRadius, true);
  }
#endif
}

void LyraCarouselTheme::drawHomeMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                     const std::function<std::string(int index)>& buttonLabel,
                                     const std::function<UIIcon(int index)>& rowIcon) const {
  if (buttonCount <= 0) return;

  const int tileHeight = kMenuIconPadding * 2 + kMenuIconSize;
  const int tileWidth = (FREEINK_DEVICE_XUESU_300 ? rect.width : renderer.getScreenWidth()) / buttonCount;
  const int rowY = (FREEINK_DEVICE_XUESU_300 ? rect.y + rect.height : renderer.getScreenHeight()) - tileHeight;
#if FREEINK_DEVICE_XUESU_300
  if (selectedIndex >= 0 && selectedIndex < buttonCount) {
    const auto label = buttonLabel(selectedIndex);
    const Rect labelBox{rect.x + compact_home::SIDE_INSET, rect.y, rect.width - 2 * compact_home::SIDE_INSET,
                        renderer.getLineHeight(UI_10_FONT_ID)};
    drawCompactHomeText(renderer, labelBox, UI_10_FONT_ID, label.c_str(), 1, true, true);
  }
#else
  (void)buttonLabel;
#endif

  for (int i = 0; i < buttonCount; ++i) {
    const int tileX = (FREEINK_DEVICE_XUESU_300 ? rect.x : 0) + i * tileWidth;
    const int iconX = tileX + (tileWidth - kMenuIconSize) / 2;
    const int iconY = rowY + kMenuIconPadding;
    const bool selected = i == selectedIndex;
    if (selected) {
      const int highlightSize = FREEINK_DEVICE_XUESU_300
                                    ? std::min(kMenuIconSize + kMenuHighlightPadding * 2, tileWidth - 6)
                                    : kMenuIconSize + kMenuHighlightPadding * 2;
      renderer.fillRoundedRect(iconX - (highlightSize - kMenuIconSize) / 2, rowY + (tileHeight - highlightSize) / 2,
                               highlightSize, highlightSize, kCornerRadius, Color::Black);
    }

    if (rowIcon == nullptr) continue;
    const UIIcon iconName = rowIcon(i);
    if (iconName == UIIcon::Apps) {
      drawAppsMenuIcon(renderer, iconX, iconY, selected);
      continue;
    }

    const uint8_t* icon = iconForName(iconName, kMenuIconSize);
    if (icon == nullptr) continue;
    if (selected) {
      renderer.drawIconInverted(icon, iconX, iconY, kMenuIconSize);
    } else {
      renderer.drawIcon(icon, iconX, iconY, kMenuIconSize);
    }
  }
}
