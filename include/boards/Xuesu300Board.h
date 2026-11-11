#pragma once

#include <stdint.h>

namespace xuesu300 {

// Display settings observed in stock xuesu-300-std firmware 1.3.3.
struct PanelPins {
  int8_t sck;
  int8_t mosi;
  int8_t miso;
  int8_t cs;
  int8_t dc;
  int8_t reset;
  int8_t busy;
};

inline constexpr PanelPins panelPins{
    .sck = 4,
    .mosi = 5,
    .miso = -1,
    .cs = 3,
    .dc = 2,
    .reset = 0,
    .busy = 1,
};
inline constexpr uint16_t panelWidth = 400;
inline constexpr uint16_t panelHeight = 300;
inline constexpr uint32_t stockPanelSpiHz = 20'000'000;
inline constexpr uint8_t panelSpiMode = 0;
inline constexpr bool panelSpiMsbFirst = true;
inline constexpr uint16_t panelResetPulseMs = 10;
inline constexpr bool panelBusyActiveLow = true;
inline constexpr uint32_t panelBusyTimeoutMs = 10'000;

// Register 0x70, measured on the reference unit. Matches UC8276C.
inline constexpr uint8_t referencePanelChipRevision = 0x07;

// Key edges measured on the reference unit. Previous/next are active high,
// confirm is active low. The stock SDMMC init drives GPIO42 high.
inline constexpr int previousKeyPin = 46;
inline constexpr int nextKeyPin = 45;
inline constexpr int confirmKeyPin = 17;
inline constexpr int sdEnablePin = 42;

// Stock battery initialization uses ADC2 channel 3 at 2.5 dB and an
// active-low charge status input with a pull-up (0x42025050).
inline constexpr int8_t batteryAdcPin = 14;
inline constexpr int8_t batteryChargeStatusPin = 16;
inline constexpr float batteryDividerMultiplier = 4.3f;
inline constexpr uint16_t batteryFullMillivolts = 4200;

constexpr uint16_t batteryPercentFromMillivolts(uint16_t millivolts) {
  struct Point {
    uint16_t mv;
    uint8_t percent;
  };
  // Voltage/percentage points used by the stock estimator (0x3C187838).
  constexpr Point curve[]{{3300, 0},
                          {3450, 5},
                          {3600, 10},
                          {3680, 20},
                          {3740, 30},
                          {3790, 40},
                          {3830, 50},
                          {3870, 60},
                          {3920, 70},
                          {3980, 80},
                          {4060, 90},
                          {4150, 95},
                          {batteryFullMillivolts, 100}};
  if (millivolts <= curve[0].mv) return 0;
  for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
    const auto& low = curve[i - 1];
    const auto& high = curve[i];
    if (millivolts > high.mv) continue;
    const int span = high.mv - low.mv;
    return low.percent + ((millivolts - low.mv) * (high.percent - low.percent) + span / 2) / span;
  }
  return 100;
}

struct SdPins {
  int clk;
  int cmd;
  int d0;
  int d1;
  int d2;
  int d3;
};
inline constexpr SdPins sdPins{38, 39, 47, 48, 41, 40};

struct PanelRegisterWrite {
  uint8_t command;
  uint8_t length;
  uint8_t data[5];
};

// Stock register trace at 0x4209278c, for comparison during bring-up.
inline constexpr PanelRegisterWrite observedPanelInit[] = {
    {0x00, 2, {0x3b, 0x4a}},
    {0xe1, 1, {0x02}},
    {0x01, 5, {0x03, 0x10, 0x3f, 0x3f, 0x03}},
    {0x06, 3, {0x25, 0x25, 0x34}},
    {0x30, 1, {0x09}},
    {0x61, 4, {0x01, 0x90, 0x01, 0x2c}},
    {0x82, 1, {0x11}},
    {0x50, 1, {0x97}},
    {0x60, 1, {0x22}},
    {0xe3, 1, {0x88}},
};

}  // namespace xuesu300
