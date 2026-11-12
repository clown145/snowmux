#include "XuesuXmpic.h"

#if FREEINK_DEVICE_XUESU_300

#include <Bitmap.h>
#include <HalStorage.h>
#include <Logging.h>

#include <array>
#include <utility>

namespace xuesu_xmpic {
namespace {
// The factory wallpaper exporter writes a portrait, top-down, MSB-first raster.
constexpr int kWidth = 300;
constexpr int kHeight = 400;
constexpr size_t kSourceStride = (kWidth + 7) / 8;
constexpr size_t kBmpStride = (kWidth + 31) / 32 * 4;
}  // namespace

bool writeBmpPreview(const char* sourcePath, const char* destinationPath) {
  HalFile source;
  if (!Storage.openFileForRead("XMPIC", sourcePath, source)) return false;
  if (source.fileSize64() != kSourceStride * kHeight) {
    LOG_ERR("XMPIC", "Expected a 300x400 XMPIC raster (%u bytes)", static_cast<unsigned>(kSourceStride * kHeight));
    return false;
  }

  HalFile output;
  if (!Storage.openFileForWrite("XMPIC", destinationPath, output)) return false;
  BmpHeader header;
  createBmpHeader(&header, kWidth, kHeight, BmpRowOrder::TopDown);
  // XMPIC uses palette index 1 for black. Preserve the source bits.
  std::swap(header.colors[0], header.colors[1]);
  if (output.write(&header, sizeof(header)) != sizeof(header)) return false;
  std::array<uint8_t, kBmpStride> row{};
  for (int y = 0; y < kHeight; ++y) {
    if (source.read(row.data(), kSourceStride) != kSourceStride || output.write(row.data(), row.size()) != row.size()) {
      LOG_ERR("XMPIC", "Incomplete preview row %d", y);
      return false;
    }
  }
  // Report any buffered SD write failure before handing the file to the viewer.
  return output.close();
}
}  // namespace xuesu_xmpic

#endif
