#include "SettingsListActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "SettingsActivity.h"
#include "XuesuSettingsLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
// Row icons, indexed by SettingsSection.
#if FREEINK_DEVICE_XUESU_300
constexpr int16_t COMPACT_ICON_SIZE = 24;
constexpr const freeink::Icon* COMPACT_SECTION_ICONS[SETTINGS_SECTION_COUNT] = {
    &icon_settings_24, &icon_smartphone_24, &icon_book_open_24, &icon_pointer_24, &icon_library_24, &icon_wifi_24,
};
#endif
constexpr int16_t ICON_SIZE = 32;
constexpr const freeink::Icon* SECTION_ICONS[SETTINGS_SECTION_COUNT] = {
    &icon_settings_32, &icon_smartphone_32, &icon_book_open_32, &icon_pointer_32, &icon_library_32, &icon_wifi_32,
};
}  // namespace

SettingsListActivity::SettingsListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SettingsSection openSection)
    : UiListActivity("SettingsList", renderer, mappedInput), pendingSection(openSection) {}

void SettingsListActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

void SettingsListActivity::onExit() {
  UiListActivity::onExit();
  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsListActivity::loop() {
  if (pendingSection != SettingsSection::End) {
    const SettingsSection section = pendingSection;
    pendingSection = SettingsSection::End;
    openSection(section);
    return;
  }
  UiListActivity::loop();
}

// Rebuilds row labels; call when entering and after a section returns (the
// UI language may have changed).
void SettingsListActivity::rebuildRows() {
  const auto& icons =
#if FREEINK_DEVICE_XUESU_300
      XuesuSettingsLayout::enabled() ? COMPACT_SECTION_ICONS :
#endif
                                     SECTION_ICONS;
  for (size_t i = 0; i < SETTINGS_SECTION_COUNT; ++i) {
    auto& item = rowItems_[i];
    item.label = I18N.get(SETTINGS_SECTION_TITLES[i]);
    item.icon = fui::bitmapFromIcon(*icons[i]);
    item.actionValue = static_cast<int16_t>(i);
  }
}

void SettingsListActivity::buildScreen(UiScreen& screen) {
#if FREEINK_DEVICE_XUESU_300
  const bool compact = XuesuSettingsLayout::enabled();
  const Rect content = compact ? XuesuSettingsLayout::content(renderer) : pageContentRect();
#else
  constexpr bool compact = false;
  const Rect content = pageContentRect();
#endif
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(content.y), static_cast<int16_t>(renderer.getScreenWidth() - content.x - content.width),
      static_cast<int16_t>(renderer.getScreenHeight() - content.y - content.height), static_cast<int16_t>(content.x)});
  if (!compact) screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  const auto& theme = screen.theme();
  int16_t textHeight = screen.target().lineHeight(theme.bodyText.font);
  if (!FREEINK_DEVICE_XUESU_300) textHeight += screen.target().lineHeight(theme.smallText.font);
  const int16_t paddingY = mappedInput.hasTouch() ? theme.listTouchRowPaddingY : theme.listRowPaddingY;
  props.rowHeight = static_cast<int16_t>(std::max<int16_t>(textHeight, ICON_SIZE) + 2 * paddingY);
#if FREEINK_DEVICE_XUESU_300
  const auto viewport = screen.contentRect();
  if (compact) {
    XuesuSettingsLayout::fitRows(screen.target(), theme, props);
    const int stretchedHeight = (viewport.height - (listCount() - 1) * props.rowGap) / listCount();
    const int iconHeight = COMPACT_ICON_SIZE + 2 * props.rowPaddingY;
    props.rowHeight = static_cast<int16_t>(std::max({static_cast<int>(props.rowHeight), stretchedHeight, iconHeight}));
  } else {
    props.rowGap = 0;
    props.rowHeight = std::max<int16_t>(props.rowHeight, viewport.height / listCount());
  }
#endif
  syncListViewport(screen, props);
  if (!showMainTabContentSelection()) props.selectedIndex = -1;
  XuesuSettingsLayout::drawRows(screen, props);
}

void SettingsListActivity::selectMainTabContentEdge(const MainTabContentEdge edge) {
  moveSelectionTo(MainTabs::contentEdgeIndex(edge, listCount()));
}

void SettingsListActivity::activateIndex(const int index) {
  // The section screen replaces this one; a lingering flash would gray an
  // unrelated element on return.
  app.clearTapFlash();
  openSection(static_cast<SettingsSection>(index));
}

void SettingsListActivity::openSection(const SettingsSection section) {
  nav.selected = static_cast<int>(section);
  auto activity = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, section);
  if (!activity) {
    LOG_ERR("SETTINGS", "OOM: SettingsActivity");
    return;
  }
  if (usesMainTabBar()) activityManager.focusMainTabContent();
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    rebuildRows();
    // A theme change inside the section must restyle this screen too.
    resetUi();
    requestUpdate();
  });
}

void SettingsListActivity::onBackButton() { onGoHome(); }

void SettingsListActivity::drawChrome() {
#if FREEINK_DEVICE_XUESU_300
  if (XuesuSettingsLayout::enabled()) {
    // General → About has the full build version. Give the title the available width.
    drawPageHeader(XuesuSettingsLayout::header(renderer), tr(STR_SETTINGS_TITLE));
    return;
  }
#endif
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Version rides in the header's trailing label slot: the footer position
  // conflicts with button hints on non-touch devices.
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_SETTINGS_TITLE),
                 CROSSPOINT_VERSION);
}
