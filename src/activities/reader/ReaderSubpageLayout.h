#pragma once

#include <GfxRenderer.h>

#include <algorithm>

#include "components/UITheme.h"
#include "fontIds.h"

namespace ReaderSubpageLayout {

inline bool usesCompactHeader(const GfxRenderer& renderer) {
  return FREEINK_DEVICE_XUESU_300 && renderer.getScreenWidth() > renderer.getScreenHeight();
}

inline Rect headerRect(const GfxRenderer& renderer, const bool upstreamStyle) {
  Rect header = UITheme::getInstance().getHeaderRect(renderer, true, false, upstreamStyle);
#if FREEINK_DEVICE_XUESU_300
  if (usesCompactHeader(renderer)) header.height = std::min(header.height, renderer.getLineHeight(UI_12_FONT_ID) + 12);
#endif
  return header;
}

}  // namespace ReaderSubpageLayout
