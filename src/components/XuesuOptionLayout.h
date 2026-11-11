#pragma once

#include <FreeInkUI.h>

#include <algorithm>
#include <span>
#include <string>

namespace XuesuOptionLayout {
inline constexpr int16_t SIDE_MARGIN = 12;
inline constexpr int16_t VERTICAL_MARGIN = 8;
inline constexpr int16_t SCROLL_WIDTH = 4;
inline constexpr int16_t SCROLL_INSET = 5;

inline int fit(const freeink::ui::DrawTarget& target, freeink::ui::OptionDialogProps& props,
               std::span<const std::string> labels, int width, int height, int optionLimit) {
  namespace fui = freeink::ui;
  const int maxOptions = std::min<int>(labels.size(), optionLimit);
  if (maxOptions <= 0) return 0;
  constexpr int16_t MIN_GAP = 6;
  const auto verticalPadding = std::clamp<int16_t>(props.padding.top, 10, 14);
  // The centered label column leaves room for the scroll track in the panel margin.
  const auto horizontalPadding = std::max<int16_t>(verticalPadding, SCROLL_WIDTH + SCROLL_INSET + MIN_GAP);
  props.padding = fui::Insets{verticalPadding, horizontalPadding, verticalPadding, horizontalPadding};
  props.gap = std::clamp<int16_t>(props.gap, MIN_GAP, 8);
  if (labels.size() > 4) props.buttonText.font = fui::FONT_SLOT_SMALL;
  const int lineHeight = target.lineHeight(props.buttonText.font);
  props.buttonHeight = static_cast<int16_t>(std::clamp<int>(props.buttonHeight, lineHeight + 8, lineHeight + 12));

  // FreeInkUI's button uses 4px horizontal padding on each side. One stable
  // height across the options prevents the overlay from shifting as focus moves.
  constexpr int BUTTON_HORIZONTAL_PADDING = 8;
  const int labelWidth = width - 2 * horizontalPadding - BUTTON_HORIZONTAL_PADDING;
  if (std::any_of(labels.begin(), labels.end(), [&](const auto& label) {
        return target.measureText(props.buttonText.font, label.c_str(), props.buttonText).width > labelWidth;
      })) {
    props.buttonText.maxLines = 2;
    props.buttonHeight = static_cast<int16_t>(2 * lineHeight + MIN_GAP);
  }

  // Fit the content before painting. Clamping the panel after measurement
  // would move the bottom-anchored buttons up into the text on a short screen.
  const int optionStep = props.buttonHeight + props.gap;
  int chrome = fui::optionDialogHeight(target, props, static_cast<int16_t>(width));
  while (chrome + optionStep > height) {
    if (props.message && props.messageText.maxLines > 2) {
      --props.messageText.maxLines;
    } else if (props.headline && props.headlineText.maxLines > 1) {
      --props.headlineText.maxLines;
    } else if (props.title && props.titleText.maxLines > 1) {
      --props.titleText.maxLines;
    } else if (props.message && props.messageText.maxLines > 1) {
      --props.messageText.maxLines;
    } else {
      break;
    }
    chrome = fui::optionDialogHeight(target, props, static_cast<int16_t>(width));
  }
  return std::clamp((height - chrome) / optionStep, 1, maxOptions);
}
}  // namespace XuesuOptionLayout
