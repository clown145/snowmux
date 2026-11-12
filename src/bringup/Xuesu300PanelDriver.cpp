#if FREEINK_DEVICE_XUESU_300

#include <Logging.h>
#include <Memory.h>
#include <PanelDriver.h>
#include <Xuesu300StockLut.generated.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "boards/Xuesu300Board.h"

namespace freeink {
namespace {

constexpr uint16_t kWidthBytes = xuesu300::panelWidth / 8;
constexpr uint16_t kHeight = xuesu300::panelHeight;
constexpr size_t kPlaneBytes = kWidthBytes * kHeight;
using Waveform = uint8_t[5][42];
enum class GrayPass { Main, DarkAdjustment };
enum class GrayUpdate { Absolute, Differential };

uint8_t changedGrayPixels(const uint8_t* previous, size_t index, uint8_t low, uint8_t high) {
  if (!previous) return ~(low & high);  // Compare against the white page established by the clean refresh.
  return (low ^ previous[index]) | (high ^ previous[kPlaneBytes + index]);
}

uint8_t reverseBits(uint8_t value) {
  value = ((value & 0x55) << 1) | ((value & 0xaa) >> 1);
  value = ((value & 0x33) << 2) | ((value & 0xcc) >> 2);
  return (value << 4) | (value >> 4);
}

void writePlane(EpdBus& bus, uint8_t command, const uint8_t* buffer) {
  // The panel's native portrait direction is opposite the renderer's default.
  // Rotate the command-plane rows as they go out, keeping the framebuffer in renderer order.
  bus.cmd(command);
  auto transaction = bus.beginTxn();
  std::array<uint8_t, kWidthBytes> row;
  for (size_t offset = kPlaneBytes; offset != 0; offset -= kWidthBytes) {
    for (size_t i = 0; i < row.size(); ++i) row[i] = reverseBits(buffer[offset - 1 - i]);
    transaction.writeBytes(row.data(), row.size());
  }
}

bool waitPanel(EpdBus& bus, bool busy, uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (bus.isBusy() != busy) {
    if (millis() - start >= timeoutMs) return false;
    delay(1);
  }
  return true;
}

bool preparePanel(EpdBus& bus, const Waveform& lut) {
  bus.reset(40);
  if (!waitPanel(bus, false, 300)) {
    LOG_ERR("UC8276C", "Panel busy after reset");
    return false;
  }
  for (const auto& write : xuesu300::observedPanelInit) {
    bus.cmd(write.command);
    bus.data(write.data, write.length);
  }
  for (uint8_t i = 0; i < 5; ++i) {
    bus.cmd(0x20 + i);
    bus.data(lut[i], sizeof(lut[i]));
  }
  return true;
}

bool activatePanel(EpdBus& bus, const char* kind) {
  bus.cmd(0x50);
  bus.data(0xd7);
  const uint32_t start = millis();
  bus.cmd(0x17);
  bus.data(0xa5);
  if (!waitPanel(bus, true, 300) || !waitPanel(bus, false, xuesu300::panelBusyTimeoutMs)) {
    LOG_ERR("UC8276C", "%s refresh timed out", kind);
    bus.reset(40);
    return false;
  }
  bus.cmd(0x02);
  delay(2);
  if (!waitPanel(bus, false, 1000)) {
    LOG_ERR("UC8276C", "Panel power-off timed out");
    bus.reset(40);
    return false;
  }
  LOG_DBG("UC8276C", "Refresh %s in %lu ms", kind, static_cast<unsigned long>(millis() - start));
  return true;
}

class Xuesu300PanelDriver final : public PanelDriver {
 public:
  const char* controllerName() const override { return "UC8276C"; }
  uint32_t spiHz() const override { return 4'000'000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::UcIdleHigh; }
  PanelGeometry geometry() const override { return {xuesu300::panelWidth, kHeight, kWidthBytes, kPlaneBytes}; }
  void begin(EpdBus&) override {
    clearGrayHistory();
    optical = OpticalState::Unknown;
    committed = false;
  }
  // Every completed refresh already powers the panel down.
  void deepSleep(EpdBus&) override {}
  bool displayCommitted() const override { return committed; }
  OpticalState opticalState() const override { return optical; }

  void seedBaseline(const uint8_t* framebuffer) {
    clearGrayHistory();
    std::memcpy(previous.data(), framebuffer, previous.size());
    optical = OpticalState::Bw;
    committed = true;
  }

  void display(EpdBus& bus, const uint8_t* framebuffer, const uint8_t*, RefreshMode mode, bool) override {
    clearGrayHistory();
    refreshBw(bus, framebuffer, mode);
  }

  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode) const override {
    if (mode != GrayscaleMode::Absolute) return {};
    return {.encoding = GrayscaleEncoding::AbsolutePlanes, .base = GrayscaleBase::Combined};
  }

  void beginGrayscale(EpdBus& bus, const uint8_t* framebuffer, GrayscaleMode mode, RefreshMode fallback,
                      bool) override {
    grayUpdate = GrayUpdate::Absolute;
    committed = false;
    lsbReady = msbReady = false;
    cleanBeforeGray = fallback != RefreshMode::Fast;
    if (mode != GrayscaleMode::Absolute) return;
    if (grayPlanes) return;
    // The two selector planes are reused for both stock passes. Only 50 bytes
    // at a time are sent to SPI, so the full staging buffer can live in PSRAM.
    if (memory::psramHasHeadroom(2 * kPlaneBytes, 2 * kPlaneBytes, 0)) {
      grayPlanes = memory::makePsramByteBufferUninitializedNoThrow(2 * kPlaneBytes);
    }
    if (!grayPlanes) {
      LOG_ERR("UC8276C", "OOM: grayscale selectors (%zu bytes)", 2 * kPlaneBytes);
      previousGray.reset();
      // The B/W proxy is still intact here. Later uploads reuse that buffer.
      if (framebuffer) refreshBw(bus, framebuffer, fallback);
    }
  }

  void useDifferentialGray() { grayUpdate = GrayUpdate::Differential; }

  void copyGrayscaleLsb(EpdBus&, const uint8_t* lsb) override {
    if (!grayPlanes || !lsb) return;
    std::memcpy(grayPlanes.get(), lsb, kPlaneBytes);
    lsbReady = true;
  }

  void copyGrayscaleMsb(EpdBus&, const uint8_t* msb) override {
    if (!grayPlanes || !msb || !lsbReady) return;
    std::memcpy(grayPlanes.get() + kPlaneBytes, msb, kPlaneBytes);
    msbReady = true;
  }

  void displayGray(EpdBus& bus, const uint8_t*, bool, const uint8_t*, bool) override {
    if (!grayPlanes || !lsbReady || !msbReady) return;
    if (grayUpdate == GrayUpdate::Differential) {
      displayDifferentialGray(bus);
      return;
    }
    previousGray.reset();
    const auto planes = std::move(grayPlanes);

    // The stock first pass can leave unchanged white pixels alone. Start
    // from known white so that is also correct after a gray page or a menu.
    if (!establishWhite(bus)) return;

    committed = false;
    optical = OpticalState::Unknown;
    const auto* lsb = planes.get();
    const auto* msb = lsb + kPlaneBytes;
    if (!refreshGray(bus, lsb, msb, GrayPass::Main, GrayUpdate::Absolute, nullptr)) return;
    const bool needsDarkPass =
        !std::equal(lsb, lsb + kPlaneBytes, msb, [](uint8_t low, uint8_t high) { return (low & ~high) == 0; });
    if (needsDarkPass && !refreshGray(bus, lsb, msb, GrayPass::DarkAdjustment, GrayUpdate::Absolute, nullptr)) return;

    optical = OpticalState::Gray;
    committed = true;
  }

  void cleanupGrayscaleBuffers(EpdBus&, const uint8_t* bw) override {
    lsbReady = msbReady = false;
    if (!bw || optical != OpticalState::Gray || !previousGray) clearGrayHistory();
  }

 private:
  void clearGrayHistory() {
    grayPlanes.reset();
    previousGray.reset();
    lsbReady = msbReady = false;
  }

  bool establishWhite(EpdBus& bus) {
    const bool alreadyWhite = optical == OpticalState::Bw &&
                              std::all_of(previous.begin(), previous.end(), [](uint8_t byte) { return byte == 0xff; });
    return (!cleanBeforeGray && alreadyWhite) || refreshBw(bus, nullptr, RefreshMode::Full);
  }

  void displayDifferentialGray(EpdBus& bus) {
    const uint8_t* baseline = nullptr;
    if (!cleanBeforeGray && optical == OpticalState::Gray && previousGray) {
      baseline = previousGray.get();
    } else if (!establishWhite(bus)) {
      return;
    }

    const auto* lsb = grayPlanes.get();
    const auto* msb = lsb + kPlaneBytes;
    bool changed = false;
    bool needsDarkPass = false;
    for (size_t index = 0; index < kPlaneBytes; ++index) {
      const uint8_t delta = changedGrayPixels(baseline, index, lsb[index], msb[index]);
      changed |= delta != 0;
      needsDarkPass |= (delta & lsb[index] & ~msb[index]) != 0;
    }

    if (changed) {
      committed = false;
      optical = OpticalState::Unknown;
      if (!refreshGray(bus, lsb, msb, GrayPass::Main, GrayUpdate::Differential, baseline)) return;
      if (needsDarkPass && !refreshGray(bus, lsb, msb, GrayPass::DarkAdjustment, GrayUpdate::Differential, baseline)) {
        return;
      }
    }

    // Rotate the staged and committed 30 KB PSRAM buffers to reuse both across page turns.
    previousGray.swap(grayPlanes);
    optical = OpticalState::Gray;
    committed = true;
  }

  bool refreshBw(EpdBus& bus, const uint8_t* framebuffer, RefreshMode mode) {
    const bool previousKnown = optical == OpticalState::Bw;
    // A gray image is not an eligible differential baseline.
    const bool fast = previousKnown && mode == RefreshMode::Fast;
    if (fast && framebuffer && std::memcmp(previous.data(), framebuffer, previous.size()) == 0) {
      committed = true;
      return true;
    }
    committed = false;
    optical = OpticalState::Unknown;
    if (!preparePanel(bus, fast ? stockFastLut : stockFullLut)) return false;
    if (previousKnown) {
      writePlane(bus, 0x10, previous.data());
    } else {
      bus.fillPlane(0x10, 0xff, kHeight, kWidthBytes);
    }
    if (framebuffer) {
      writePlane(bus, 0x13, framebuffer);
    } else {
      bus.fillPlane(0x13, 0xff, kHeight, kWidthBytes);
    }
    if (!activatePanel(bus, fast ? "differential" : "full")) return false;
    if (framebuffer) {
      std::memcpy(previous.data(), framebuffer, previous.size());
    } else {
      previous.fill(0xff);
    }
    optical = OpticalState::Bw;
    committed = true;
    return true;
  }

  static bool refreshGray(EpdBus& bus, const uint8_t* lsb, const uint8_t* msb, GrayPass pass, GrayUpdate update,
                          const uint8_t* baseline) {
    const bool darkPass = pass == GrayPass::DarkAdjustment;
    if (!preparePanel(bus, darkPass ? stockGraySecondLut : stockGrayFirstLut)) return false;
    for (const uint8_t command : {0x10, 0x13}) {
      bus.cmd(command);
      auto transaction = bus.beginTxn();
      std::array<uint8_t, kWidthBytes> row;
      for (size_t offset = kPlaneBytes; offset != 0; offset -= kWidthBytes) {
        for (size_t i = 0; i < row.size(); ++i) {
          const size_t index = offset - 1 - i;
          const uint8_t low = lsb[index];
          const uint8_t high = msb[index];
          uint8_t bits;
          if (update == GrayUpdate::Differential) {
            // Stock 1.3.3 displayGrayDiff encoder at 0x4215A2FC. Main writes
            // white=01, light=00, black=10, and leaves dark/unchanged at 11.
            // The second pass selects only changed pixels that end at dark.
            const uint8_t changed = changedGrayPixels(baseline, index, low, high);
            bits = darkPass ? ~(changed & low & ~high) : ~(changed & (command == 0x10 ? high : ~low));
          } else {
            // The absolute encoder starts from white and primes dark pixels in the main pass.
            bits = darkPass ? ~(low & ~high) : (command == 0x10 ? low | ~high : low & high);
          }
          row[i] = reverseBits(bits);
        }
        transaction.writeBytes(row.data(), row.size());
      }
    }
    if (update == GrayUpdate::Differential) {
      return activatePanel(bus, darkPass ? "gray differential dark" : "gray differential");
    }
    return activatePanel(bus, darkPass ? "gray dark adjustment" : "gray main");
  }

  // Single-buffer mode needs a retained OLD plane across the panel's hardware reset.
  std::array<uint8_t, kPlaneBytes> previous;
  memory::ByteBuffer grayPlanes;
  memory::ByteBuffer previousGray;
  OpticalState optical = OpticalState::Unknown;
  GrayUpdate grayUpdate = GrayUpdate::Absolute;
  bool committed = false;
  bool lsbReady = false;
  bool msbReady = false;
  bool cleanBeforeGray = false;
};

Xuesu300PanelDriver& driverInstance() {
  static Xuesu300PanelDriver driver;
  return driver;
}

}  // namespace

PanelDriver& xuesu300PanelDriver() { return driverInstance(); }

void useXuesu300DifferentialGray() { driverInstance().useDifferentialGray(); }

void seedXuesu300PanelBaseline(const uint8_t* framebuffer) { driverInstance().seedBaseline(framebuffer); }

}  // namespace freeink

#endif
