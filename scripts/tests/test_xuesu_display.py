"""Record the real Snow 3 panel driver and exercise the factory XMPIC raster reader."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from test_reading_ui_regressions import ROOT, method, run_cpp


class XuesuDisplayTest(unittest.TestCase):
    def test_almanac_absolute_planes_preserve_antialias_and_reversed_text(self):
        gfx = ROOT / 'lib/GfxRenderer'
        source = (gfx / 'GfxRenderer.cpp').read_text(encoding='utf-8')
        header = (gfx / 'GfxRenderer.h').read_text(encoding='utf-8')
        run_cpp(r'''
#include <BitmapHelpers.h>
#include <cassert>
#include <initializer_list>
struct GfxRenderer {
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  struct TwoBitPixel { bool draw; bool state; };
  mutable bool ink = false;
  bool grayPlanesAreAbsolute() const { return true; }
  bool antialiasesAbsoluteGrayText() const { return true; }
  void drawPixel(int, int, bool state) const { ink = state; }
''' + method(header, 'static constexpr TwoBitPixel mapTwoBitPixel(') +
            method(header, 'static constexpr TwoBitPixel mapTwoBitGlyphCoverage(') + r'''
};
''' + method(source, 'static void draw2BitGlyphPixel(') + r'''
int main() {
  GfxRenderer renderer;
  for (const bool blackText : {false, true}) {
    for (uint8_t coverage = 0; coverage < 4; ++coverage) {
      renderer.ink = !blackText;
      draw2BitGlyphPixel(renderer, GfxRenderer::GRAYSCALE_LSB, 0, 0, blackText, coverage);
      const int lsb = !renderer.ink;
      renderer.ink = !blackText;
      draw2BitGlyphPixel(renderer, GfxRenderer::GRAYSCALE_MSB, 0, 0, blackText, coverage);
      const int msb = !renderer.ink;
      assert((msb * 2 + lsb) == (blackText ? 3 - coverage : coverage));
    }
  }
}
''', include_dirs=[gfx])

    def test_gray_plane_encoding_and_bw_recovery(self):
        display = ROOT / 'freeink-sdk/libs/display/FreeInkDisplay'
        with TemporaryDirectory(prefix='xuesu-panel-') as directory:
            fake = Path(directory)
            (fake / 'Arduino.h').write_text('''
#pragma once
#include <cstdint>
inline constexpr int LOW = 0, HIGH = 1;
int digitalRead(int);
uint32_t millis();
void delay(uint32_t);
''')
            (fake / 'SPI.h').write_text('struct SPISettings {};\n')
            (fake / 'Logging.h').write_text('''
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(tag, format, kind, elapsed) ((void)(kind), (void)(elapsed))
''')
            (fake / 'esp_heap_caps.h').write_text('''
#pragma once
#include <cstdlib>
inline constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_8BIT = 4;
inline size_t heapSize = 8 * 1024 * 1024;
inline size_t heap_caps_get_free_size(unsigned) { return heapSize; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return heapSize; }
inline void* heap_caps_malloc(size_t size, unsigned) { return std::malloc(size); }
inline void* heap_caps_calloc(size_t count, size_t size, unsigned) { return std::calloc(count, size); }
''')
            (fake / 'Xuesu300StockLut.generated.h').write_text('''
#include <cstdint>
// Distinct markers to check which generated waveform bank the driver uploads.
inline constexpr uint8_t stockFullLut[5][42]{{11}};
inline constexpr uint8_t stockFastLut[5][42]{{22}};
inline constexpr uint8_t stockGrayFirstLut[5][42]{{33}};
inline constexpr uint8_t stockGraySecondLut[5][42]{{44}};
''')
            run_cpp(r'''
#include <PanelDriver.h>
#include <esp_heap_caps.h>
#include <array>
#include <cassert>
#include <vector>

namespace freeink {
PanelDriver& xuesu300PanelDriver();
void useXuesu300DifferentialGray();
}
using namespace freeink;

struct Command { uint8_t code; std::vector<uint8_t> bytes; };
std::vector<Command> trace;
uint32_t now = 0;
uint32_t busyUntil = 0;
int activations = 0;
int holdBusyAt = 0;

uint32_t millis() { return now; }
void delay(uint32_t ms) { now += ms; }
int digitalRead(int) { return now < busyUntil ? LOW : HIGH; }

EpdBus::~EpdBus() = default;
void EpdBus::begin(const EpdPins&, uint32_t, BusyPolarity polarity, int8_t, int8_t) { _busy = polarity; }
void EpdBus::reset(uint16_t) { busyUntil = now; }
void EpdBus::cmd(uint8_t value) { trace.push_back({value, {}}); }
void EpdBus::data(uint8_t value) {
  trace.back().bytes.push_back(value);
  if (trace.back().code == 0x17) {
    assert(value == 0xa5);
    ++activations;
    busyUntil = now + (activations == holdBusyAt ? 20'000 : 2);
  }
}
void EpdBus::data(const uint8_t* data, uint16_t size) {
  trace.back().bytes.insert(trace.back().bytes.end(), data, data + size);
}
void EpdBus::fillPlane(uint8_t code, uint8_t byte, uint16_t rows, uint16_t stride) {
  cmd(code);
  trace.back().bytes.assign(rows * stride, byte);
}
EpdBus::Transaction::Transaction(EpdBus& bus) : _bus(&bus) {}
EpdBus::Transaction::~Transaction() = default;
void EpdBus::Transaction::writeBytes(const uint8_t* bytes, uint16_t size) { _bus->data(bytes, size); }
EpdBus::Transaction EpdBus::beginTxn() { return Transaction(*this); }

std::vector<std::vector<uint8_t>> payloads(uint8_t code) {
  std::vector<std::vector<uint8_t>> result;
  for (const auto& command : trace) if (command.code == code) result.push_back(command.bytes);
  return result;
}

int main() {
  auto& driver = xuesu300PanelDriver();
  EpdBus bus;
  bus.begin({}, driver.spiHz(), driver.busyPolarity());
  driver.begin(bus);
  assert(driver.grayscaleCapabilities(GrayscaleMode::Absolute).encoding == GrayscaleEncoding::AbsolutePlanes);
  assert(!driver.grayscaleCapabilities(GrayscaleMode::Overlay).supported());

  std::array<uint8_t, 15'000> white, lsb, msb;
  white.fill(0xff);
  lsb = msb = white;
  // Absolute black, dark, light, white, repeated at the first wire byte.
  lsb.back() = 0xaa;
  msb.back() = 0xcc;
  const auto gray = [&](RefreshMode fallback = RefreshMode::Fast) {
    driver.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, fallback, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    driver.displayGray(bus, white.data(), false, nullptr, true);
  };

  gray();
  assert(driver.displayCommitted() && driver.opticalState() == OpticalState::Gray);
  const auto luts = payloads(0x20);
  const auto old = payloads(0x10);
  const auto next = payloads(0x13);
  assert(luts.size() == 3 && luts[0][0] == 11 && luts[1][0] == 33 && luts[2][0] == 44);
  assert(old.size() == 3 && next.size() == 3);
  assert(old[0] == std::vector<uint8_t>(15'000, 0xff) && next[0] == old[0]);
  assert(old[1].size() == 15'000 && old[1][0] == 0xdd && next[1][0] == 0x11);
  assert(old[2].size() == 15'000 && old[2][0] == 0xbb && old[2] == next[2]);
  assert(old[1][1] == 0xff && next[1].back() == 0xff);

  trace.clear();
  driver.cleanupGrayscaleBuffers(bus, white.data());
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(driver.opticalState() == OpticalState::Bw && payloads(0x20)[0][0] == 11);
  trace.clear();
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(trace.empty());

  lsb.back() = 0x00;  // no dark pixels, so the second stock pass can be omitted
  gray();
  assert(driver.displayCommitted() && payloads(0x20).size() == 1 && payloads(0x20)[0][0] == 33);
  trace.clear();
  gray(RefreshMode::Full);
  assert(driver.displayCommitted() && payloads(0x20).size() == 2 && payloads(0x20)[0][0] == 11);

  trace.clear();
  heapSize = 0;
  gray();
  assert(driver.displayCommitted() && driver.opticalState() == OpticalState::Bw);
  assert(payloads(0x20).size() == 1 && payloads(0x20)[0][0] == 11);
  heapSize = 8 * 1024 * 1024;
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  trace.clear();
  driver.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, RefreshMode::Fast, false);
  driver.copyGrayscaleLsb(bus, lsb.data());
  driver.cleanupGrayscaleBuffers(bus, nullptr);
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(driver.displayCommitted() && trace.empty());

  lsb.back() = 0xaa;
  holdBusyAt = activations + 2;  // timeout on the dark pass
  gray();
  assert(!driver.displayCommitted() && driver.opticalState() == OpticalState::Unknown);
  assert(payloads(0x17).size() == 2 && payloads(0x02).size() == 1);
  trace.clear();
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(driver.displayCommitted() && payloads(0x20)[0][0] == 11);
  trace.clear();
  white[0] = 0xfe;
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(driver.displayCommitted() && payloads(0x20)[0][0] == 22);

  const auto differential = [&](RefreshMode fallback = RefreshMode::Fast) {
    driver.beginGrayscale(bus, white.data(), GrayscaleMode::Absolute, fallback, false);
    useXuesu300DifferentialGray();
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    driver.displayGray(bus, white.data(), false, nullptr, true);
    driver.cleanupGrayscaleBuffers(bus, white.data());
  };
  const auto setLevels = [&](bool previousPage) {
    lsb.fill(0xff);
    msb.fill(0xff);
    for (int pixel = 0; pixel < 16; ++pixel) {
      const int level = previousPage ? pixel / 4 : pixel % 4;
      const auto index = lsb.size() - 1 - pixel / 8;
      const auto bit = static_cast<uint8_t>(1u << (pixel % 8));
      if (!(level & 1)) lsb[index] &= ~bit;
      if (!(level & 2)) msb[index] &= ~bit;
    }
  };

  trace.clear();
  setLevels(true);
  differential();
  assert(driver.displayCommitted() && payloads(0x20).size() == 3 && payloads(0x20)[0][0] == 11);
  trace.clear();
  differential();
  assert(driver.displayCommitted() && trace.empty());

  // Check all sixteen old-to-new pixel transitions in wire order.
  setLevels(false);
  differential();
  assert(driver.displayCommitted());
  const auto diffLuts = payloads(0x20);
  const auto diffOld = payloads(0x10);
  const auto diffNext = payloads(0x13);
  assert(diffLuts.size() == 2 && diffLuts[0][0] == 33 && diffLuts[1][0] == 44);
  for (int pixel = 0; pixel < 16; ++pixel) {
    const int from = pixel / 4, to = pixel % 4;
    constexpr int mainCodes[] = {2, 3, 0, 1};
    const int mainCode = from == to ? 3 : mainCodes[to];
    const int darkCode = from != to && to == 1 ? 0 : 3;
    const int shift = 7 - pixel % 8;
    const int byte = pixel / 8;
    assert(((diffOld[0][byte] >> shift) & 1) == (mainCode >> 1));
    assert(((diffNext[0][byte] >> shift) & 1) == (mainCode & 1));
    assert(((diffOld[1][byte] >> shift) & 1) == (darkCode >> 1));
    assert(((diffNext[1][byte] >> shift) & 1) == (darkCode & 1));
  }
  assert(diffOld[0][2] == 0xff && diffNext[1].back() == 0xff);

  trace.clear();
  differential(RefreshMode::Half);
  assert(driver.displayCommitted() && payloads(0x20).size() == 3 && payloads(0x20)[0][0] == 11);

  trace.clear();
  lsb.fill(0xff);
  msb.fill(0xff);
  differential();
  assert(driver.displayCommitted() && payloads(0x20).size() == 1 && payloads(0x20)[0][0] == 33);

  trace.clear();
  setLevels(true);
  holdBusyAt = activations + 2;
  differential();
  assert(!driver.displayCommitted() && driver.opticalState() == OpticalState::Unknown);
  trace.clear();
  differential();
  assert(driver.displayCommitted() && payloads(0x20).size() == 3 && payloads(0x20)[0][0] == 11);

  trace.clear();
  driver.display(bus, white.data(), nullptr, RefreshMode::Fast, false);
  assert(driver.opticalState() == OpticalState::Bw && payloads(0x20)[0][0] == 11);
}
''', include_dirs=(fake, ROOT / 'include', ROOT / 'lib/Memory'),
                    system_include_dirs=(display / 'include', display / 'src/driver'),
                    defines=('FREEINK_DEVICE_XUESU_300=1', 'BOARD_HAS_PSRAM=1'),
                    extra_sources=(ROOT / 'src/bringup/Xuesu300PanelDriver.cpp',))

    def test_xmpic_preview_preserves_factory_bits_and_rejects_bad_sizes(self):
        with TemporaryDirectory(prefix='xuesu-xmpic-') as directory:
            fake = Path(directory)
            (fake / 'Logging.h').write_text('#define LOG_ERR(...) ((void)0)\n')
            (fake / 'HalStorage.h').write_text('''
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
struct HalFile {
  std::vector<uint8_t>* bytes = nullptr;
  size_t position = 0;
  size_t fileSize64() const { return bytes->size(); }
  int read(void* dst, size_t size) {
    if (position + size > bytes->size()) return 0;
    memcpy(dst, bytes->data() + position, size);
    position += size;
    return static_cast<int>(size);
  }
  size_t write(const void* src, size_t size) {
    const auto* first = static_cast<const uint8_t*>(src);
    bytes->insert(bytes->end(), first, first + size);
    return size;
  }
  bool close() { return true; }
};
struct MockStorage {
  std::vector<uint8_t> source, destination;
  bool openFileForRead(const char*, const char*, HalFile& file) { file.bytes = &source; return true; }
  bool openFileForWrite(const char*, const char*, HalFile& file) { file.bytes = &destination; return true; }
};
extern MockStorage Storage;
''')
            run_cpp(r'''
#include <Bitmap.h>
#include <XuesuXmpic.h>
#include <cassert>
MockStorage Storage;
int main() {
  Storage.source.resize(15'199);
  assert(!xuesu_xmpic::writeBmpPreview("source", "preview") && Storage.destination.empty());
  Storage.source.resize(15'201);
  assert(!xuesu_xmpic::writeBmpPreview("source", "preview") && Storage.destination.empty());
  Storage.source.resize(15'200);
  Storage.source[0] = 0x80;
  Storage.source[37] = 0x10;
  Storage.source[38] = 0x40;
  Storage.source.back() = 0x20;
  assert(xuesu_xmpic::writeBmpPreview("source", "preview"));
  BmpHeader header{};
  memcpy(&header, Storage.destination.data(), sizeof(header));
  assert(header.fileHeader.bfType == 0x4d42 && header.fileHeader.bfSize == sizeof(header) + 16'000);
  assert(header.infoHeader.biWidth == 300 && header.infoHeader.biHeight == -400 && header.infoHeader.biBitCount == 1);
  assert(header.colors[0].rgbRed == 255 && header.colors[1].rgbRed == 0);
  const auto* raster = Storage.destination.data() + sizeof(header);
  assert(raster[0] == 0x80 && raster[37] == 0x10 && raster[38] == 0 && raster[39] == 0 && raster[40] == 0x40);
  assert(raster[399 * 40 + 37] == 0x20);
}
''', include_dirs=(fake, ROOT / 'lib/GfxRenderer', ROOT / 'src/util'), defines=('FREEINK_DEVICE_XUESU_300=1',),
                    extra_sources=(ROOT / 'src/util/XuesuXmpic.cpp', ROOT / 'lib/GfxRenderer/BitmapHelpers.cpp'))


if __name__ == '__main__':
    unittest.main()
