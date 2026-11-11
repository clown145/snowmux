#pragma once

#include <algorithm>

#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/UiAppHost.h"

namespace XuesuSettingsLayout {
constexpr int16_t ROW_GAP = 6;

inline bool enabled() { return !UITheme::getInstance().hasMainTabs(); }

inline Rect header(const GfxRenderer& renderer) {
  auto& theme = UITheme::getInstance();
  Rect bounds = theme.getHeaderRect(renderer, true);
  const auto fonts = uiScaleSpec();
  const int textHeight = renderer.getLineHeight(fonts.titleFontId);
  const int statusHeight = std::max(GUI.headerStatusHeight(), theme.getMetrics().batteryBarHeight);
  // A compact header centers the title in the band. Reserve both sides of
  // its line box so the ink begins below the status strip.
  bounds.height = std::min(bounds.height, 2 * statusHeight + textHeight);
  return bounds;
}

inline Rect content(const GfxRenderer& renderer) {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  const Rect chrome = header(renderer);
  const int top = chrome.y + chrome.height + ROW_GAP;
  return Rect{safe.x, top, safe.width, std::max(0, safe.y + safe.height - top - ROW_GAP)};
}

inline void fitRows(const freeink::ui::DrawTarget& target, const freeink::ui::ThemeTokens& theme,
                    freeink::ui::ListProps& props) {
  props.labelText = theme.bodyText;
  props.labelText.maxLines = 2;
  props.valueText = theme.smallText;
  props.valueText.align = freeink::ui::TextAlign::Right;
  props.balanceWrappedLabelWithValue = false;
  props.rowPaddingY = std::clamp<int16_t>(theme.listRowPaddingY, 4, 6);
  props.rowGap = std::max<int16_t>(ROW_GAP, theme.listRowGap);
  const int textHeight = target.lineHeight(props.labelText.font) + 2 * props.rowPaddingY;
  // FreeInkUI also publishes a touch box around full rows. Keep its expansion
  // inside each row's cadence even though this board drives focus by keys.
  props.rowHeight = static_cast<int16_t>(std::max(textHeight, theme.minTouchSize - props.rowGap));
}

#if FREEINK_DEVICE_XUESU_300
inline int readableRowWidth(const freeink::ui::Rect viewport, const freeink::ui::ListProps& props) {
  // The caller has already resolved props and attached ListNav. Its list keeps
  // the scrollbar reservation stable, including when a page happens to fit.
  const int inset = std::max(0, static_cast<int>(props.rowInset));
  const int strip = props.scrollIndicator && props.scrollIndicatorWidth > 0
                        ? std::max(0, props.scrollIndicatorWidth + props.scrollIndicatorInset + 2 - inset)
                        : 0;
  return std::max(1, viewport.width - 2 * inset - strip);
}

inline void reflowValue(const freeink::ui::DrawTarget& target, const freeink::ui::ListProps& props, int rowWidth,
                        freeink::ui::ListItem& row) {
  if (!row.label || !row.value || row.subtitle || row.toggle || row.isHeader) return;
  const int sidePadding = std::max(0, static_cast<int>(props.sidePadding));
  const int iconWidth = row.icon ? (props.iconSize > 0 ? props.iconSize : row.icon.width) + props.textGap : 0;
  auto labelStyle =
      freeink::ui::hasState(row.state, freeink::ui::StateEmphasized) ? props.emphasizedText : props.labelText;
  labelStyle.bold |= row.bold;
  const int labelWidth = target.measureText(labelStyle.font, row.label, labelStyle).width;
  const int valueWidth = target.measureText(props.valueText.font, row.value, props.valueText).width;
  if (labelWidth + valueWidth + props.textGap + props.valueInset + iconWidth + 2 * sidePadding > rowWidth ||
      (props.valueMaxWidth > 0 && valueWidth > props.valueMaxWidth)) {
    row.subtitle = row.value;
    row.value = nullptr;
  }
}

inline void allowReadableRows(const freeink::ui::DrawTarget& target, const freeink::ui::Rect viewport,
                              freeink::ui::ListProps& props) {
  // Preserve the SDK's exact row hit boxes but omit its clipped preview of
  // the following row on the bottom of a settings list.
  props.partialTrailingMinHeight = static_cast<int16_t>(viewport.height + 1);
  props.subtitleText = props.valueText;
  props.subtitleText.align = props.rtl ? freeink::ui::TextAlign::Right : freeink::ui::TextAlign::Left;
  const int labelLine = std::max<int>(1, target.lineHeight(props.labelText.font));
  const int valueLine = std::max<int>(1, target.lineHeight(props.subtitleText.font));
  const int padding =
      props.rowPaddingY < 0 ? std::max(0, props.rowHeight - labelLine - valueLine) : 2 * props.rowPaddingY;
  const int textRoom = viewport.height - padding;
  props.labelText.maxLines = static_cast<uint8_t>(std::clamp((textRoom - valueLine) / labelLine, 1, 3));
  // Keep even the largest row inside the viewport so three-key follow can
  // put that whole row on screen. About and connection URLs can take five lines.
  const int valueRoom = textRoom - props.labelText.maxLines * labelLine;
  props.subtitleText.maxLines = static_cast<uint8_t>(std::clamp(valueRoom / valueLine, 1, 5));
}
#endif

inline void drawRows(UiAppHost::UiScreen& screen, freeink::ui::ListProps& props) {
#if FREEINK_DEVICE_XUESU_300
  if (enabled()) {
    // Resolve only visible rows, without copying strings or mutating the
    // source rows. The provider lives through this synchronous list call.
    struct ReflowRows {
      const freeink::ui::DrawTarget& target;
      const freeink::ui::ListProps& props;
      decltype(freeink::ui::ListProps::rowProvider) source;
      void* sourceContext;
      int width;
    } rows{screen.target(), props, props.rowProvider, props.rowProviderCtx,
           readableRowWidth(screen.contentRect(), props)};
    allowReadableRows(screen.target(), screen.contentRect(), props);
    props.rowProviderCtx = &rows;
    props.rowProvider = [](void* context, uint16_t index, freeink::ui::ListItem& item) {
      const auto& rows = *static_cast<const ReflowRows*>(context);
      if (rows.source) {
        rows.source(rows.sourceContext, index, item);
      } else {
        item = rows.props.items[index];
      }
      reflowValue(rows.target, rows.props, rows.width, item);
    };
    screen.list(props);
    props.rowProvider = rows.source;
    props.rowProviderCtx = rows.sourceContext;
    return;
  }
#endif
  screen.list(props);
}
}  // namespace XuesuSettingsLayout
