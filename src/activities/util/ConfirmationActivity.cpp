#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "InxItemLayout.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const BodyPlacement bodyPlacement)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body), bodyPlacement(bodyPlacement) {}

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           const std::string& cancelLabel, const std::string& confirmLabel)
    : ConfirmationActivity(renderer, mappedInput, heading, body, BodyPlacement::Page) {
  this->cancelLabel = cancelLabel;
  this->confirmLabel = confirmLabel;
}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  const char* options[] = {cancelLabel.empty() ? tr(STR_CANCEL) : cancelLabel.c_str(),
                           confirmLabel.empty() ? tr(STR_CONFIRM) : confirmLabel.c_str()};
  const auto resolve = [this](int index) {
    ActivityResult result;
    result.isCancelled = index != 1;
    setResult(std::move(result));
    finish();
  };

  auto& theme = UITheme::getInstance();
  if (!theme.hasMainTabs()) {
    confirmPopup.show(heading.c_str(), body.c_str(), options, 2, 0, resolve);
    requestUpdate(true);
    return;
  }

  const Rect safe = theme.getScreenSafeArea(renderer, true, false);
  const int maxWidth = safe.width - 2 * MARGIN;
  auto pageHeading = renderer.truncatedText(HEADING_FONT_ID, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  std::string firstLine;
  std::string secondLine;
  if (bodyPlacement == BodyPlacement::Page && !body.empty()) {
    auto lines = renderer.wrappedText(BODY_FONT_ID, body.c_str(), maxWidth, MAX_BODY_LINES, EpdFontFamily::REGULAR);
    if (!lines.empty()) firstLine = std::move(lines[0]);
    if (lines.size() > 1) secondLine = std::move(lines[1]);
  }

  const int headingHeight = pageHeading.empty() ? 0 : renderer.getLineHeight(HEADING_FONT_ID);
  const int bodyLines = firstLine.empty() ? 0 : (secondLine.empty() ? 1 : 2);
  const int textHeight = headingHeight + (headingHeight > 0 && bodyLines > 0 ? SPACING : 0) +
                         bodyLines * renderer.getLineHeight(BODY_FONT_ID);
  const int top = safe.y + std::max(MARGIN, safe.height / 6 - textHeight / 2);
  const Rect panel = InxOptionGeometry::layout(safe, 2, 0).panel;
  if (textHeight > 0 && top + textHeight + SubpageLayout::sectionGap(theme.getMetrics()) > panel.y) {
    // The wrapped headline and the options are measured together on a short screen.
    confirmPopup.show(heading.c_str(), body.c_str(), options, 2, 0, resolve);
  } else {
    safeHeading = std::move(pageHeading);
    safeBody = std::move(firstLine);
    safeBodySecondLine = std::move(secondLine);
    pageTextBounds = Rect{safe.x + MARGIN, top, maxWidth, textHeight};
    const char* popupTitle = bodyPlacement == BodyPlacement::Page ? safeHeading.c_str() : body.c_str();
    confirmPopup.show(popupTitle, options, 2, 0, resolve);
  }

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (pageTextBounds.height > 0) {
    const GfxRenderer::ClipScope clip(renderer, pageTextBounds.x, pageTextBounds.y, pageTextBounds.width,
                                      pageTextBounds.height);
    int y = pageTextBounds.y;
    if (!safeHeading.empty()) {
      UITheme::drawCenteredText(renderer, pageTextBounds, HEADING_FONT_ID, y, safeHeading.c_str(), true,
                                EpdFontFamily::BOLD);
      y += renderer.getLineHeight(HEADING_FONT_ID) + SPACING;
    }
    if (!safeBody.empty()) {
      UITheme::drawCenteredText(renderer, pageTextBounds, BODY_FONT_ID, y, safeBody.c_str());
      y += renderer.getLineHeight(BODY_FONT_ID);
    }
    if (!safeBodySecondLine.empty()) {
      UITheme::drawCenteredText(renderer, pageTextBounds, BODY_FONT_ID, y, safeBodySecondLine.c_str());
    }
  }

  if (confirmPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (confirmPopup.handleInput(mappedInput, [this] {
        if (confirmPopup.isActive()) requestUpdate();
      }))
    return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
