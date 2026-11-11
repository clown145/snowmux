#if FREEINK_DEVICE_XUESU_300

#include <Logging.h>
#include <PanelDriver.h>
#include <Xuesu300StockLut.generated.h>

#include <array>
#include <cstring>

#include "boards/Xuesu300Board.h"

namespace freeink {
namespace {

constexpr uint16_t kWidthBytes = xuesu300::panelWidth / 8;
constexpr uint16_t kHeight = xuesu300::panelHeight;
constexpr size_t kPlaneBytes = kWidthBytes * kHeight;

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

class Xuesu300PanelDriver final : public PanelDriver {
 public:
  const char* controllerName() const override { return "UC8276C"; }
  uint32_t spiHz() const override { return 4'000'000; }
  BusyPolarity busyPolarity() const override { return BusyPolarity::UcIdleHigh; }
  PanelGeometry geometry() const override { return {xuesu300::panelWidth, kHeight, kWidthBytes, kPlaneBytes}; }
  void begin(EpdBus&) override { committed = false; }
  // Every completed refresh already powers the panel down.
  void deepSleep(EpdBus&) override {}
  bool displayCommitted() const override { return committed; }
  OpticalState opticalState() const override { return committed ? OpticalState::Bw : OpticalState::Unknown; }

  void seedBaseline(const uint8_t* framebuffer) {
    std::memcpy(previous.data(), framebuffer, previous.size());
    committed = true;
  }

  void display(EpdBus& bus, const uint8_t* framebuffer, const uint8_t*, RefreshMode mode, bool) override {
    const bool previousKnown = committed;
    // Half is the reader's scheduled cleanup. This panel has two verified waveforms.
    const bool fast = previousKnown && mode == RefreshMode::Fast;
    if (fast && std::memcmp(previous.data(), framebuffer, previous.size()) == 0) return;
    committed = false;
    bus.reset(40);
    if (!waitPanel(bus, false, 300)) {
      LOG_ERR("UC8276C", "Panel busy after reset");
      return;
    }

    for (const auto& write : xuesu300::observedPanelInit) {
      bus.cmd(write.command);
      bus.data(write.data, write.length);
    }
    const auto& lut = fast ? stockFastLut : stockFullLut;
    for (uint8_t i = 0; i < 5; ++i) {
      bus.cmd(0x20 + i);
      bus.data(lut[i], sizeof(lut[i]));
    }
    // Send both complete planes after reset. The stock fast activation uses
    // the differential waveform without setting a hardware window.
    if (previousKnown) {
      writePlane(bus, 0x10, previous.data());
    } else {
      bus.fillPlane(0x10, 0xff, kHeight, kWidthBytes);
    }
    writePlane(bus, 0x13, framebuffer);
    bus.cmd(0x50);
    bus.data(0xd7);
    const char* const kind = fast ? "differential" : "full";
    const uint32_t start = millis();
    bus.cmd(0x17);
    bus.data(0xa5);

    if (!waitPanel(bus, true, 300) || !waitPanel(bus, false, xuesu300::panelBusyTimeoutMs)) {
      LOG_ERR("UC8276C", "%s refresh timed out", kind);
      bus.reset(40);
      return;
    }
    bus.cmd(0x02);
    delay(2);
    if (!waitPanel(bus, false, 1000)) {
      LOG_ERR("UC8276C", "Panel power-off timed out");
      bus.reset(40);
      return;
    }
    std::memcpy(previous.data(), framebuffer, previous.size());
    committed = true;
    LOG_DBG("UC8276C", "Refresh %s in %lu ms", kind, static_cast<unsigned long>(millis() - start));
  }

 private:
  // Single-buffer mode needs a retained OLD plane across the panel's hardware reset.
  std::array<uint8_t, kPlaneBytes> previous;
  bool committed = false;
};

Xuesu300PanelDriver& driverInstance() {
  static Xuesu300PanelDriver driver;
  return driver;
}

}  // namespace

PanelDriver& xuesu300PanelDriver() { return driverInstance(); }

void seedXuesu300PanelBaseline(const uint8_t* framebuffer) { driverInstance().seedBaseline(framebuffer); }

}  // namespace freeink

#endif
