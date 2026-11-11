#include "AppsMenuActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "CrossPointSettings.h"
#include "InxItemLayout.h"
#include "OpdsServerStore.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/inx_apps.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

using appVisibility::appBit;
using appVisibility::AppId;

struct AppEntry {
  AppId id;
  StrId titleId;
  UIIcon icon;
  void (ActivityManager::*open)();
};

constexpr AppEntry kAppEntries[] = {
    {AppId::Library, StrId::STR_LIBRARY, UIIcon::Library, &ActivityManager::goToLibrary},
    {AppId::FileTransfer, StrId::STR_FILE_TRANSFER, UIIcon::Transfer, &ActivityManager::goToFileTransfer},
    {AppId::OpdsBrowser, StrId::STR_OPDS_BROWSER, UIIcon::Opds, &ActivityManager::goToBrowser},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::WeRead, StrId::STR_WEREAD_TITLE, UIIcon::WeRead, &ActivityManager::goToWeRead},
#endif
    {AppId::AirPage, StrId::STR_AIRPAGE_TITLE, UIIcon::AirPage, &ActivityManager::goToAirPage},
    {AppId::ReadingStats, StrId::STR_READING_STATS, UIIcon::ReadingStats, &ActivityManager::goToReadingStatsMenu},
#if !FREEINK_DEVICE_XUESU_300
    // These games need left/right movement or touch to reach the whole board.
    {AppId::Sudoku, StrId::STR_SUDOKU_TITLE, UIIcon::Sudoku, &ActivityManager::goToSudoku},
    {AppId::Gomoku, StrId::STR_GOMOKU_TITLE, UIIcon::Gomoku, &ActivityManager::goToGomoku},
    {AppId::Sokoban, StrId::STR_SOKOBAN_TITLE, UIIcon::Sokoban, &ActivityManager::goToSokoban},
#ifdef ENABLE_CHINESE_VERSION
    {AppId::ChineseChess, StrId::STR_CHINESE_CHESS_TITLE, UIIcon::ChineseChess, &ActivityManager::goToChineseChess},
#endif
    {AppId::Minesweeper, StrId::STR_MINESWEEPER_TITLE, UIIcon::Minesweeper, &ActivityManager::goToMinesweeper},
    {AppId::Game2048, StrId::STR_2048_TITLE, UIIcon::Game2048, &ActivityManager::goToGame2048},
#endif
    {AppId::UglyAvatar, StrId::STR_UGLY_AVATAR, UIIcon::Avatar, &ActivityManager::goToUglyAvatar},
    {AppId::Buddy, StrId::STR_BUDDY_TITLE, UIIcon::Buddy, &ActivityManager::goToBuddy},
#if !FREEINK_DEVICE_XUESU_300
    {AppId::PixelSwitch, StrId::STR_PIXEL_SWITCH_TITLE, UIIcon::PixelSwitch, &ActivityManager::goToPixelSwitch},
    {AppId::Calculator, StrId::STR_CALCULATOR_TITLE, UIIcon::Calculator, &ActivityManager::goToCalculator},
#endif
    {AppId::Woodfish, StrId::STR_WOODFISH_TITLE, UIIcon::Woodfish, &ActivityManager::goToWoodfish},
    {AppId::Standby, StrId::STR_STANDBY_TITLE, UIIcon::Standby, &ActivityManager::goToStandby},
};

constexpr int kAppCount = static_cast<int>(sizeof(kAppEntries) / sizeof(kAppEntries[0]));

constexpr int visibleAppCount(const uint32_t hiddenMask) {
  int count = 0;
  for (const auto& app : kAppEntries) {
    if ((hiddenMask & appBit(app.id)) == 0) {
      // cppcheck-suppress useStlAlgorithm
      ++count;
    }
  }
  return count;
}

constexpr int appIndexForVisibleIndex(const uint32_t hiddenMask, const int visibleIndex) {
  int visible = 0;
  for (int appIndex = 0; appIndex < kAppCount; ++appIndex) {
    if ((hiddenMask & appBit(kAppEntries[appIndex].id)) != 0) continue;
    if (visible++ == visibleIndex) return appIndex;
  }
  return -1;
}

constexpr uint32_t effectiveHiddenMask(const uint32_t hiddenMask, const bool hasOpdsServers) {
  uint32_t effective = hiddenMask;
  if (!hasOpdsServers) effective |= appBit(AppId::OpdsBrowser);
  return effective;
}

constexpr bool appIdsAreUnique() {
  for (int i = 0; i < kAppCount; ++i) {
    for (int j = i + 1; j < kAppCount; ++j) {
      if (kAppEntries[i].id == kAppEntries[j].id) return false;
    }
  }
  return true;
}

static_assert(kAppCount <= 32, "the app catalog must fit hiddenAppsMask");
static_assert(static_cast<uint8_t>(AppId::Count) <= 32, "hiddenAppsMask supports at most 32 stable app IDs");
static_assert(appIdsAreUnique(), "stable app IDs must not be reused");
static_assert(visibleAppCount(0) == kAppCount, "a zero mask must show every compiled app");
static_assert(visibleAppCount(UINT32_MAX) == 0, "a full mask must hide every compiled app");
static_assert(visibleAppCount(appBit(AppId::Woodfish)) == kAppCount - 1, "the widened mask must hide Woodfish");
static_assert(visibleAppCount(effectiveHiddenMask(0, false)) == kAppCount - 1,
              "OPDS must be hidden when no server is configured");
static_assert(visibleAppCount(effectiveHiddenMask(0, true)) == kAppCount,
              "all apps must be available regardless of language");
static_assert(appIndexForVisibleIndex(appBit(kAppEntries[1].id), 1) == 2,
              "visible indices must skip a hidden middle app");

}  // namespace

int AppsMenuActivity::getAppCount() { return kAppCount; }

StrId AppsMenuActivity::getAppTitleId(const int appIndex) {
  return appIndex >= 0 && appIndex < kAppCount ? kAppEntries[appIndex].titleId : StrId::STR_NONE_OPT;
}

bool AppsMenuActivity::isAppVisible(const int appIndex) {
  if (appIndex < 0 || appIndex >= kAppCount) return false;
  return (SETTINGS.hiddenAppsMask & appBit(kAppEntries[appIndex].id)) == 0;
}

bool AppsMenuActivity::setAppVisible(const int appIndex, const bool visible) {
  if (appIndex < 0 || appIndex >= kAppCount) return false;

  const uint32_t bit = appBit(kAppEntries[appIndex].id);
  const uint32_t updatedMask = visible ? SETTINGS.hiddenAppsMask & ~bit : SETTINGS.hiddenAppsMask | bit;
  if (updatedMask == SETTINGS.hiddenAppsMask) return false;

  SETTINGS.hiddenAppsMask = updatedMask;
  return true;
}

int AppsMenuActivity::getVisibleAppCount() {
  return visibleAppCount(effectiveHiddenMask(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers()));
}

void AppsMenuActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  moveSelectionTo(MainTabs::contentEdgeIndex(edge, getVisibleAppCount()));
}

int AppsMenuActivity::getAppIndexForVisibleIndex(const int visibleIndex) {
  return appIndexForVisibleIndex(effectiveHiddenMask(SETTINGS.hiddenAppsMask, OPDS_STORE.hasServers()), visibleIndex);
}

void AppsMenuActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRowItems();
}

void AppsMenuActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(static_cast<size_t>(getVisibleAppCount()));
  for (int visibleIndex = 0; visibleIndex < getVisibleAppCount(); ++visibleIndex) {
    const int appIndex = getAppIndexForVisibleIndex(visibleIndex);
    if (appIndex < 0) continue;
    fui::ListItem item;
    item.label = I18N.get(kAppEntries[appIndex].titleId);
    item.icon = listIconFor(kAppEntries[appIndex].icon, 32);
    item.actionValue = static_cast<int16_t>(visibleIndex);
    rowItems.push_back(item);
  }
}

bool AppsMenuActivity::usesIconLayout() const {
  return UITheme::getInstance().hasMainTabs() &&
         InxGridGeometry::layoutFrom(SETTINGS.inxAppsLayout) == InxItemLayout::Icons;
}

Rect AppsMenuActivity::appContentRect() const {
  const int spacing = UITheme::getInstance().getMetrics().verticalSpacing;
  Rect content = pageContentRect();
  content.y += spacing;
  content.height -= spacing * 2;
  return content;
}

void AppsMenuActivity::openSelected() {
  const int appIndex = getAppIndexForVisibleIndex(nav.selected);
  if (appIndex >= 0) (activityManager.*kAppEntries[appIndex].open)();
}

void AppsMenuActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  openSelected();
}

void AppsMenuActivity::drawIconGrid(UiScreen& screen, const Rect& rect, const int visibleCount,
                                   const bool showSelection) {
  const int lineHeight = screen.target().lineHeight(screen.theme().bodyText.font);
  int gridWidth = rect.width;
  int gridHeight = rect.height;
  int gridTop = rect.y;
#if FREEINK_DEVICE_XUESU_300
  constexpr int columns = 2;
  constexpr int maxRows = 5;
  constexpr int iconSize = InxAppIcons::size;
  constexpr int padding = 8;
  constexpr int labelInsetY = 2;
  constexpr auto compactFont = fui::GfxRendererTarget::FONT_LABEL;
  const int compactLineHeight = std::max<int>(1, screen.target().lineHeight(compactFont));
  const int minimumTileHeight = std::max({iconSize + padding, lineHeight + 2 * labelInsetY,
                                          2 * std::min(lineHeight, compactLineHeight) + 2 * labelInsetY});
  const int fittingRows = std::clamp(rect.height / (minimumTileHeight + 2 * InxGridGeometry::cellInset), 1, maxRows);
  const int rows = std::min(fittingRows, (visibleCount + columns - 1) / columns);
  // Center shorter grids without making a single row fill the whole display.
  gridHeight = rect.height * rows / std::max(rows, std::min(3, fittingRows));
  gridTop += (rect.height - gridHeight) / 2;
  if (visibleCount > rows * columns) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    gridWidth -= metrics.scrollBarWidth + metrics.scrollBarRightOffset;
  }
#else
  constexpr int columns = InxGridGeometry::columns;
  const int rows = InxGridGeometry::fittingRows(rect.height, lineHeight, InxAppIcons::size);
#endif
  const int capacity = rows * columns;
  syncPagedViewport(capacity);
  const int start = nav.top;

  for (int slot = 0; slot < capacity && start + slot < visibleCount; ++slot) {
    const int visibleIndex = start + slot;
    const int appIndex = getAppIndexForVisibleIndex(visibleIndex);
    if (appIndex < 0) continue;
    const auto bounds = InxGridGeometry::cellBounds(slot, gridWidth, gridHeight, rows, columns);
    const Rect cell{rect.x + bounds.x, gridTop + bounds.y, bounds.width, bounds.height};
    if (cell.width <= 0 || cell.height <= 0) continue;
    const GfxRenderer::ClipScope clip(renderer, cell.x, cell.y, cell.width, cell.height);
    const bool isSelected = showSelection && visibleIndex == nav.selected;
    screen.frame().hit(fui::Rect{static_cast<int16_t>(cell.x), static_cast<int16_t>(cell.y),
                                 static_cast<int16_t>(cell.width), static_cast<int16_t>(cell.height)},
                       ACTION_ROW, static_cast<int16_t>(visibleIndex), fui::InputTouch);

    auto text = screen.theme().bodyText;
    text.bold = false;
    const auto& entry = kAppEntries[appIndex];
    const char* label = I18N.get(entry.titleId);
#if FREEINK_DEVICE_XUESU_300
    renderer.drawRoundedRect(cell.x, cell.y, cell.width, cell.height, isSelected ? 3 : 1, 6, true);
    text.color = fui::Color::Black;
    const int iconX = cell.x + padding;
    const int iconY = cell.y + (cell.height - iconSize) / 2;
    InxAppIcons::draw(renderer, entry.icon, iconX, iconY, 1, false);
    const int labelX = iconX + iconSize + InxGridGeometry::iconLabelGap;
    const fui::Rect labelRect{static_cast<int16_t>(labelX), static_cast<int16_t>(cell.y + labelInsetY),
                              static_cast<int16_t>(cell.x + cell.width - padding - labelX),
                              static_cast<int16_t>(cell.height - 2 * labelInsetY)};
    text.align = fui::TextAlign::Left;
    if (labelRect.height >= 2 * lineHeight) {
      text.maxLines = 2;
    } else if (compactLineHeight < lineHeight && labelRect.height >= 2 * compactLineHeight &&
               screen.target().measureText(text.font, label, text).width > labelRect.width) {
      text.font = compactFont;
      text.maxLines = 2;
    }
#else
    if (isSelected) renderer.fillRect(cell.x, cell.y, cell.width, cell.height, true);
    text.color = isSelected ? fui::Color::White : fui::Color::Black;
    const int iconScale = std::min(2, InxGridGeometry::maximumIconSize(cell, lineHeight) / InxAppIcons::size);
    const int iconSize = InxAppIcons::size * iconScale;
    const int iconX = cell.x + (cell.width - iconSize) / 2;
    const int iconY = InxGridGeometry::tileTop(cell, lineHeight, iconSize);
    if (iconSize > 0) InxAppIcons::draw(renderer, entry.icon, iconX, iconY, iconScale, isSelected);
    text.align = fui::TextAlign::Center;
    const int labelY = iconY + (iconSize > 0 ? iconSize + InxGridGeometry::iconLabelGap : 0);
    const fui::Rect labelRect{static_cast<int16_t>(cell.x + 4), static_cast<int16_t>(labelY),
                              static_cast<int16_t>(std::max(1, cell.width - 8)), static_cast<int16_t>(lineHeight)};
#endif
    screen.target().text(labelRect, label, text);
  }

  GUI.drawSideScrollBar(renderer, rect, visibleCount, start, capacity);
}

void AppsMenuActivity::buildScreen(UiScreen& screen) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const Rect content = appContentRect();
  const int visibleCount = getVisibleAppCount();
  const bool showSelection = showMainTabContentSelection();

  if (visibleCount == 0) {
    UITheme::drawCenteredWrappedText(renderer, content, UI_12_FONT_ID, tr(STR_NO_APPS_ENABLED), 2);
  } else if (usesIconLayout()) {
    drawIconGrid(screen, content, visibleCount, showSelection);
  } else {
    screen.setContentMarginFromScreen(
        fui::Insets{static_cast<int16_t>(content.y), static_cast<int16_t>(sw - content.x - content.width),
                    static_cast<int16_t>(sh - content.y - content.height), static_cast<int16_t>(content.x)});
    fui::ListProps props;
    props.items = rowItems.data();
    props.count = static_cast<uint16_t>(rowItems.size());
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    syncListViewport(screen, props);
    screen.list(props);
  }
}

void AppsMenuActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_APPS_TITLE));
}

void AppsMenuActivity::drawFooter() {
  const int visibleCount = getVisibleAppCount();
  const auto labels = mainTabButtonLabels(tr(STR_BACK), tr(STR_SELECT), visibleCount > 1);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AppsMenuActivity::onBackButton() { activityManager.goHome(); }
