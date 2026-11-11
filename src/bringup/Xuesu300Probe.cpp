#ifdef CROSSMUX_XUESU_300_PROBE

#include <Arduino.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <inttypes.h>

#include "boards/Xuesu300Board.h"

namespace {

void printReport() {
  Serial.println("\nXuesu 300 board probe");
  Serial.printf("Chip: %s, %u cores, revision %u\n", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipCores()),
                static_cast<unsigned>(ESP.getChipRevision()));
  Serial.printf("SDK: %s\n", ESP.getSdkVersion());

  uint32_t physicalFlashBytes = 0;
  const esp_err_t flashResult = esp_flash_get_physical_size(nullptr, &physicalFlashBytes);
  if (flashResult == ESP_OK) {
    Serial.printf("Physical flash: %" PRIu32 " bytes\n", physicalFlashBytes);
  } else {
    Serial.printf("Physical flash query failed: %s\n", esp_err_to_name(flashResult));
  }
  Serial.printf("Initialized PSRAM: %" PRIu32 " bytes\n", ESP.getPsramSize());

  const esp_partition_t* running = esp_ota_get_running_partition();
  for (auto iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr); iterator;
       iterator = esp_partition_next(iterator)) {
    const esp_partition_t* part = esp_partition_get(iterator);
    Serial.printf("Partition %-16s address=0x%08" PRIx32 " size=0x%08" PRIx32 " type=0x%02x subtype=0x%02x%s\n",
                  part->label, part->address, part->size, static_cast<unsigned>(part->type),
                  static_cast<unsigned>(part->subtype), part == running ? " [running]" : "");
  }
  Serial.println("Send ? to repeat the report or p to print the stock panel profile.");
}

void printPanelProfile() {
  const auto& pins = xuesu300::panelPins;
  Serial.println("\nXuesu 300 panel profile (stock settings and reference unit)");
  Serial.printf("Native geometry: %u x %u\n", static_cast<unsigned>(xuesu300::panelWidth),
                static_cast<unsigned>(xuesu300::panelHeight));
  Serial.printf("SPI: SCK=%d MOSI=%d MISO=%d CS=%d at %" PRIu32 " Hz, mode %u, %s first\n", pins.sck, pins.mosi,
                pins.miso, pins.cs, xuesu300::stockPanelSpiHz, static_cast<unsigned>(xuesu300::panelSpiMode),
                xuesu300::panelSpiMsbFirst ? "MSB" : "LSB");
  Serial.printf("DC=%d RST=%d BUSY=%d (active %s), reset pulse=%u ms, BUSY timeout=%" PRIu32 " ms\n", pins.dc,
                pins.reset, pins.busy, xuesu300::panelBusyActiveLow ? "low" : "high",
                static_cast<unsigned>(xuesu300::panelResetPulseMs), xuesu300::panelBusyTimeoutMs);

  Serial.printf("Reference unit panel CHIP_REV: 0x%02X\n", static_cast<unsigned>(xuesu300::referencePanelChipRevision));
  Serial.println("Observed init writes (command: data):");
  for (const auto& write : xuesu300::observedPanelInit) {
    Serial.printf("  %02X:", static_cast<unsigned>(write.command));
    for (uint8_t i = 0; i < write.length; ++i) {
      Serial.printf(" %02X", static_cast<unsigned>(write.data[i]));
    }
    Serial.println();
  }
}

}  // namespace

void setup() { Serial.begin(115200); }

void loop() {
  static bool wasConnected = false;
  const bool connected = static_cast<bool>(Serial);
  const int command = Serial.available() > 0 ? Serial.read() : -1;
  if ((connected && !wasConnected) || command == '?') {
    printReport();
  }
  if (command == 'p') {
    printPanelProfile();
  }
  wasConnected = connected;
  delay(20);
}

#endif
