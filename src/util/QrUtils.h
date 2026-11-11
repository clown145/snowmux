#pragma once

#include <GfxRenderer.h>

#include <string>

#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Renders a QR code with an optional white quiet zone inside the bounds.
void drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload,
                int quietZoneModules = 0);

}  // namespace QrUtils
