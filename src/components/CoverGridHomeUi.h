#pragma once

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "components/bars/tab-bar.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class CoverGridHomeUi final : public UiAppHost {
 public:
  static constexpr int THUMB_HEIGHT = 400;
  static constexpr int GRID_COLUMNS = 3;
  static constexpr int GRID_ROWS = 2;
  static constexpr int MAX_BOOKS = 1 + GRID_COLUMNS * GRID_ROWS;
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);
  explicit CoverGridHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasOpds, bool hasContinueReading);
  void refreshCoverPaths();
  void setSelection(int selection) { selected = selection; }
  int selectedAction(const MappedInputManager& input);
  // Exact generation height, shared by every slot and recorded during draw.
  // Thumbs must be generated at the drawn size: rescaling a dithered 1-bit
  // image aliases badly.
  int thumbHeightFor() const;
  bool takeThumbHeightChanged();
#if FREEINK_DEVICE_XUESU_300
  // Canonical size is independent of page, book count and orientation.
  static constexpr int COMPACT_THUMB_HEIGHT = 136;
  size_t takeMissingVisibleCover();
  void setLoadedCover(size_t index, std::string path);
#endif

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawHeaderBand();
  void drawEmpty(UiScreen& screen);
  void drawGrid(UiScreen& screen);
  freeink::ui::Rect layoutGrid(freeink::ui::Rect rect, int rows);
  void drawTabs(UiScreen& screen, freeink::ui::Rect rect);
#if FREEINK_DEVICE_XUESU_300
  void drawCompactBooks(UiScreen& screen);
  void drawCompactCurrent(UiScreen& screen, freeink::ui::Rect rect, bool stackMetadata);
#else
  void drawCurrent(UiScreen& screen, freeink::ui::Rect rect, int coverRowHeight);
#endif
  bool paintFramedCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, size_t index);
  void refreshCoverPath(size_t index);
  void noteThumbHeight(int slotHeight);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> coverPaths;
  int thumbHeight = 0;
  bool thumbHeightChanged = false;
  int selected = 0;
  int pending = -1;
  int progress = -1;
  bool hasOpds = false;
  bool hasContinueReading = false;
  char progressText[12]{};
#if FREEINK_DEVICE_XUESU_300
  std::array<bool, MAX_BOOKS> coverPathChecked{};
  std::array<bool, MAX_BOOKS> coverAttempted{};
#endif
  // Component styles and interaction tables stay off the render task's stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
  freeink::ui::Rect gridBounds{};
  freeink::ui::TabBarProps tabs;
  std::array<freeink::ui::TabItem, 6> tabItems;
};
