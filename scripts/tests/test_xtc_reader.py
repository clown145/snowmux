"""Exercise the XTCH reader with the actual parser and simulated panel commits."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


class XtcReaderTest(unittest.TestCase):
    def test_mixed_dimensions_and_four_gray_levels(self):
        parser = ROOT / 'lib/Xtc/Xtc'
        gfx = ROOT / 'lib/GfxRenderer'
        reader = (ROOT / 'src/activities/reader/XtcReaderActivity.cpp').read_text(encoding='utf-8')
        render_page = method(reader, 'void XtcReaderActivity::renderPage()')
        render_bar = method(reader, 'Rect XtcReaderActivity::renderStatusBarOverlay()')
        gfx_header = (gfx / 'GfxRenderer.h').read_text(encoding='utf-8')
        pixel_mapping = method(gfx_header, 'static constexpr TwoBitPixel mapTwoBitPixel(')
        dither = method(gfx_header, 'static constexpr bool ditherTwoBitPixel(')
        with TemporaryDirectory(prefix='xtch-reader-') as directory:
            fake = Path(directory)
            (fake / 'FsHelpers.h').write_text('')
            (fake / 'Logging.h').write_text('#define LOG_DBG(...)\n#define LOG_ERR(...)\n')
            (fake / 'HalStorage.h').write_text(r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

inline std::vector<uint8_t> fileBytes;
struct HalFile {
  bool opened = false;
  size_t cursor = 0;
  bool isOpen() const { return opened; }
  void close() { opened = false; }
  uint64_t fileSize64() const { return fileBytes.size(); }
  bool seek64(uint64_t offset) {
    if (offset > fileBytes.size()) return false;
    cursor = offset;
    return true;
  }
  bool seek(size_t offset) { return seek64(offset); }
  size_t read(void* dst, size_t len) {
    const size_t count = std::min(len, fileBytes.size() - cursor);
    std::memcpy(dst, fileBytes.data() + cursor, count);
    cursor += count;
    return count;
  }
};
struct {
  bool openFileForRead(const char*, const char*, HalFile& file) { file = {true, 0}; return true; }
} inline Storage;
''')
            run_cpp(r'''
#include <array>
#include <cassert>
#include <string>
#include <BitmapHelpers.h>
#include <Rect.h>
#include <XtcParser.h>
#include <Logging.h>
#define tr(id) #id

// Compile the parser with its existing legacy-version expression.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#include <XtcParser.cpp>
#pragma GCC diagnostic pop

struct HalDisplay {
  enum RefreshMode { HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode { Overlay, Absolute };
  enum class GrayscaleBase { Combined };
};
enum class DisplayRefreshContext { ContinuousReading };
struct GfxRenderer {
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  struct TwoBitPixel { bool draw; bool state; };
''' + pixel_mapping + dither + r'''
  using Pixels = std::array<bool, 8 * 10>;
  Pixels frame{}, base{}, lsb{}, msb{};
  bool committed = false, failGray = false, useOverlay = false;
  int grayUpdates = 0, bwUpdates = 0;
  int bottomMargin = 0;
  HalDisplay::RefreshMode lastRefresh{};
  int getScreenWidth() const { return 8; }
  int getScreenHeight() const { return 10; }
  void getOrientedViewableTRBL(int* top, int* right, int* bottom, int* left) const {
    *top = *right = *left = 0;
    *bottom = bottomMargin;
  }
  void clearScreen(uint8_t byte = 0xff) { frame.fill(byte == 0); }
  void drawPixel(int x, int y, bool state) { frame.at(y * getScreenWidth() + x) = state; }
  void fillRect(int x, int y, int w, int h, bool state) {
    for (int row = y; row < y + h; ++row) {
      for (int column = x; column < x + w; ++column) drawPixel(column, row, state);
    }
  }
  void drawCenteredText(int, int, const char*, bool, int) {}
  struct Capabilities {
    bool hasMode;
    HalDisplay::GrayscaleBase base = HalDisplay::GrayscaleBase::Combined;
    bool supported() const { return hasMode; }
  };
  Capabilities grayscaleCapabilities(HalDisplay::GrayscaleMode mode = HalDisplay::GrayscaleMode::Overlay) const {
    return {mode == (useOverlay ? HalDisplay::GrayscaleMode::Overlay : HalDisplay::GrayscaleMode::Absolute)};
  }
  void displayBuffer(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH) {
    committed = true;
    lastRefresh = mode;
    ++bwUpdates;
  }
  void displayGrayscaleBase(HalDisplay::RefreshMode refresh, DisplayRefreshContext) {
    assert(useOverlay);
    base = frame;
    lastRefresh = refresh;
    committed = false;
  }
  bool displayGrayscaleBase(HalDisplay::GrayscaleMode mode, HalDisplay::RefreshMode refresh) {
    assert(mode == HalDisplay::GrayscaleMode::Absolute);
    base = frame;
    lastRefresh = refresh;
    committed = false;
    return true;
  }
  void preconditionGrayscale() { assert(false); }
  void copyGrayscaleLsbBuffers() { lsb = frame; }
  void copyGrayscaleMsbBuffers() { msb = frame; }
  void displayGrayBuffer() { ++grayUpdates; committed = !failGray; }
  void cleanupGrayscaleWithFrameBuffer() {}
  bool displayCommitted() const { return committed; }
};

constexpr int UI_12_FONT_ID = 1;
struct EpdFontFamily { enum { BOLD }; };
struct CrossPointSettings {
  enum class XTC_STATUS_BAR_MODE { XTC_STATUS_BAR_HIDE, XTC_STATUS_BAR_BOTTOM, XTC_STATUS_BAR_TOP };
  XTC_STATUS_BAR_MODE bar = XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
  struct Spec { XTC_STATUS_BAR_MODE xtcMode; };
  Spec statusBarSpec() const { return {bar}; }
  int getRefreshFrequency() const { return 5; }
} SETTINGS;
struct UITheme {
  static UITheme getInstance() { return {}; }
  int getStatusBarHeight() const { return 1; }
};
struct {
  void drawStatusBar(GfxRenderer& renderer, float, int, int, std::string, int paddingBottom) {
    const int edge = renderer.getScreenHeight() - paddingBottom;
    renderer.drawPixel(1, edge - renderer.bottomMargin - UITheme::getInstance().getStatusBarHeight() - 4, true);
    renderer.drawPixel(0, edge - 1, true);
  }
} GUI;
namespace ReaderUtils { void displayWithRefreshCycle(GfxRenderer& renderer, int&) { renderer.displayBuffer(); } }
struct XtcReaderActivity {
  xtc::XtcParser* xtc;
  mutable GfxRenderer renderer;
  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 1, rendered = 0;
  struct StatusBarInfo { int currentPage, pageCount; std::string title; };
  StatusBarInfo getStatusBarInfo() const { return {1, 2, {}}; }
  Rect renderStatusBarOverlay() const;
  void markPageRendered() { ++rendered; }
  void renderPage();
};
''' + render_bar + render_page + r'''

template <typename T>
void append(const T& entry) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&entry);
  fileBytes.insert(fileBytes.end(), bytes, bytes + sizeof(T));
}

int main() {
  xtc::XtcHeader header{};
  header.magic = xtc::XTCH_MAGIC;
  header.versionMajor = 1;
  header.pageCount = 2;
  header.pageTableOffset = sizeof(header);
  header.dataOffset = sizeof(header) + 2 * sizeof(xtc::PageTableEntry);
  const auto secondOffset = header.dataOffset + sizeof(xtc::XtgPageHeader) + 4;
  append(header);
  append(xtc::PageTableEntry{header.dataOffset, 4, 2, 8});
  append(xtc::PageTableEntry{secondOffset, 16, 4, 9});
  append(xtc::XtgPageHeader{xtc::XTH_MAGIC, 2, 8, 0, 0, 4, 0});
  append(std::array<uint8_t, 4>{});
  append(xtc::XtgPageHeader{xtc::XTH_MAGIC, 4, 9, 0, 0, 16, 0});
  // Row zero is white, dark, light, black. The last pixel of row eight is black.
  append(std::array<uint8_t, 16>{0x80,0x80,0x80,0,0,0,0,0, 0x80,0x80,0,0,0x80,0,0,0});

  xtc::XtcParser book;
  assert(book.open("/mixed.xtch") == xtc::XtcError::OK);
  XtcReaderActivity reader{.xtc = &book, .renderer = {}};
  reader.renderPage();
  assert(reader.rendered == 1 && reader.renderer.lastRefresh == HalDisplay::HALF_REFRESH);
  reader.currentPage = 1;
  reader.renderPage();
  auto& renderer = reader.renderer;
  assert(reader.rendered == 2 && renderer.lastRefresh == HalDisplay::FAST_REFRESH);
  assert(renderer.grayUpdates == 2 && renderer.bwUpdates == 0);
  assert(!renderer.lsb[0] && !renderer.lsb[1] && renderer.lsb[2] && renderer.lsb[3]);
  assert(!renderer.msb[0] && renderer.msb[1] && !renderer.msb[2] && renderer.msb[3]);
  assert(renderer.lsb[67] && renderer.msb[67]);
  assert(!renderer.lsb[4] && !renderer.msb[4]);

  // The text and white background must survive both absolute planes, the B/W
  // fallback and the overlay path. Gray overlay bits in the bar stay neutral.
  using Bar = CrossPointSettings::XTC_STATUS_BAR_MODE;
  for (const auto position : {Bar::XTC_STATUS_BAR_TOP, Bar::XTC_STATUS_BAR_BOTTOM}) {
    SETTINGS.bar = position;
    const bool top = position == Bar::XTC_STATUS_BAR_TOP;
    const int pixel = top ? 1 : 4 * renderer.getScreenWidth() + 1;
    const int progressPixel = (top ? 5 : 9) * renderer.getScreenWidth();
    for (const bool overlay : {false, true}) {
      XtcReaderActivity withBar{.xtc = &book, .renderer = {.useOverlay = overlay, .bottomMargin = 1}, .currentPage = 1};
      withBar.renderPage();
      const auto& panel = withBar.renderer;
      assert(withBar.rendered == 1 && panel.grayUpdates == 1);
      assert(panel.base[pixel] && !panel.base[pixel - 1]);
      assert(panel.base[progressPixel] && panel.lsb[progressPixel] && panel.msb[progressPixel]);
      assert(panel.lsb[pixel] && panel.msb[pixel]);
      assert(panel.lsb[pixel - 1] == overlay && panel.msb[pixel - 1] == overlay);
      assert(panel.frame[pixel] && !panel.frame[pixel - 1]);
      assert(panel.frame[progressPixel]);
      withBar.renderer.failGray = true;
      withBar.renderPage();
      assert(withBar.renderer.bwUpdates == 1 && withBar.renderer.frame[pixel]);
      assert(!withBar.renderer.frame[pixel - 1]);
    }
  }
  SETTINGS.bar = Bar::XTC_STATUS_BAR_HIDE;

  renderer.failGray = true;
  reader.renderPage();
  assert(reader.rendered == 3 && renderer.bwUpdates == 1 && renderer.lastRefresh == HalDisplay::HALF_REFRESH);
  assert(!renderer.frame[0] && renderer.frame[3] && renderer.frame[67]);

  fileBytes[secondOffset + 6] = 8;
  reader.renderPage();
  assert(book.getLastError() == xtc::XtcError::CORRUPTED_HEADER);
  assert(reader.rendered == 3);

  // A wide raster shrinks to 8x4, centered in the 8x10 panel. Mark ink in
  // the last rows and columns that the old rendering would cut off.
  for (const bool gray : {false, true}) {
    fileBytes.clear();
    header.magic = gray ? xtc::XTCH_MAGIC : xtc::XTC_MAGIC;
    header.pageCount = 1;
    header.dataOffset = sizeof(header) + sizeof(xtc::PageTableEntry);
    const uint32_t size = gray ? 32 : 16;
    append(header);
    append(xtc::PageTableEntry{header.dataOffset, size, 16, 8});
    append(xtc::XtgPageHeader{gray ? xtc::XTH_MAGIC : xtc::XTG_MAGIC, 16, 8, 0, 0, size, 0});
    if (gray) {
      std::array<uint8_t, 32> data{};
      data[0] = data[16] = 0x01;  // black at (15, 7)
      data[2] = 0x01;  // light gray at (13, 7)
      append(data);
    } else {
      std::array<uint8_t, 16> data;
      data.fill(0xff);
      data[15] = 0xfe;  // black at (15, 7)
      append(data);
    }
    xtc::XtcParser wide;
    assert(wide.open(gray ? "/wide.xtch" : "/wide.xtc") == xtc::XtcError::OK);
    XtcReaderActivity fitted{.xtc = &wide, .renderer = {}};
    fitted.renderPage();
    assert(fitted.rendered == 1);
    constexpr int edge = 6 * 8 + 7;
    const auto& panel = fitted.renderer;
    if (gray) {
      assert(panel.lsb[edge] && panel.msb[edge]);
      assert(panel.lsb[edge - 1] && !panel.msb[edge - 1]);
      assert(!panel.lsb[7 * 8 + 7] && !panel.msb[7 * 8 + 7]);
    } else {
      assert(panel.frame[edge] && !panel.frame[7 * 8 + 7]);
    }
  }
}
''', include_dirs=[fake, parser, gfx, ROOT / 'src/components'])


if __name__ == '__main__':
    unittest.main()
