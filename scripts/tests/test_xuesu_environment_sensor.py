"""Exercise the actual FreeInk AHT20 driver against a recorded I2C interface."""

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from test_reading_ui_regressions import ROOT, run_cpp


class XuesuEnvironmentSensorTest(unittest.TestCase):
    def test_initialization_measurement_and_crc(self):
        driver = ROOT / 'freeink-sdk/libs/hardware/EnvironmentSensor'
        with TemporaryDirectory(prefix='xuesu-i2c-') as directory:
            fake = Path(directory)
            (fake / 'soc').mkdir()
            (fake / 'soc/soc_caps.h').write_text('')
            (fake / 'Arduino.h').write_text('void delay(unsigned long ms);\n')
            (fake / 'BoardConfig.h').write_text('''
#define FREEINK_CAP_TEMP_HUMIDITY 1
namespace BoardConfig {
constexpr struct { struct { int i2cSda, i2cScl; unsigned i2cHz, tempHumidityAddr; } sensors; }
    ACTIVE{{13, 12, 100000, 0x38}};
}
''')
            (fake / 'Wire.h').write_text('''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
using std::size_t;
struct MockWire {
  bool open = false;
  std::vector<std::vector<uint8_t>> commands;
  std::deque<std::vector<uint8_t>> responses;
  std::deque<uint8_t> data;
  bool begin(int sda, int scl, uint32_t hz) {
    assert(!open && sda == 13 && scl == 12 && hz == 100000);
    return open = true;
  }
  bool end() { assert(open); open = false; return true; }
  void setTimeOut(uint16_t) {}
  void beginTransmission(uint8_t address) { assert(open && address == 0x38); commands.emplace_back(); }
  size_t write(const uint8_t* bytes, size_t length) {
    commands.back().assign(bytes, bytes + length);
    return length;
  }
  int endTransmission() { return 0; }
  size_t requestFrom(uint8_t address, size_t count) {
    assert(open && address == 0x38 && data.empty() && !responses.empty());
    const auto& packet = responses.front();
    assert(packet.size() <= count);
    data.assign(packet.begin(), packet.end());
    responses.pop_front();
    return data.size();
  }
  int read() { assert(!data.empty()); const int value = data.front(); data.pop_front(); return value; }
};
extern MockWire Wire;
''')
            run_cpp(r'''
#include <EnvironmentSensor.h>
#include <Wire.h>
MockWire Wire;
std::vector<unsigned long> delays;
void delay(unsigned long ms) { delays.push_back(ms); }
int main() {
  EnvironmentSensor sensor;
  Wire.responses = {{0}, {8}, {8, 128, 0, 0, 64, 0, 138}, {8, 128, 0, 0, 64, 0, 139},
                    {136, 128, 0, 0, 64, 0, 102}};
  assert(sensor.begin());
  const std::vector<std::vector<uint8_t>> expectedInit{{0xba}, {0xbe, 8, 0}};
  assert(Wire.commands == expectedInit);
  float temperature = 0, humidity = 0;
  assert(sensor.read(temperature, humidity));
  assert(temperature == -46.875f && humidity == 50.0f);
  const std::vector<uint8_t> measure{0xac, 0x33, 0};
  assert(Wire.commands.back() == measure && delays.back() >= 80);
  assert(!sensor.read(temperature, humidity));  // Damaged CRC.
  assert(!sensor.read(temperature, humidity));  // Still busy, even with a correct CRC.
  sensor.end();
  assert(!Wire.open && !sensor.present());
  Wire.commands.clear();
  Wire.responses = {{8}};
  assert(sensor.begin() && Wire.commands.empty());  // No reset on a routine wake.
  sensor.end();
  Wire.responses = {{0}, {0}};
  assert(!sensor.begin() && !Wire.open);  // Release the bus after failed initialization.
}
''', include_dirs=(fake, driver / 'include'), defines=('FREEINK_DEVICE_XUESU_300=1',),
                    extra_sources=(driver / 'src/EnvironmentSensor.cpp',))
