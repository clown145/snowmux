#include "HalEnvironment.h"

#if FREEINK_DEVICE_XUESU_300 && CROSSPOINT_EMULATED == 0
#include <EnvironmentSensor.h>
#include <Logging.h>

#include <cmath>
#endif

namespace hal_environment {

Reading sample() {
#if FREEINK_DEVICE_XUESU_300 && CROSSPOINT_EMULATED == 0
  EnvironmentSensor sensor;
  float temperature, humidity;
  if (!sensor.begin()) {
    LOG_INF("XENV", "AHT20 unavailable");
    return {};
  }
  const bool ok = sensor.read(temperature, humidity);
  sensor.end();
  if (!ok) {
    LOG_INF("XENV", "AHT20 sample invalid");
    return {};
  }
  const Reading reading{static_cast<int16_t>(std::lround(temperature * 10)),
                        static_cast<uint16_t>(std::lround(humidity * 10))};
  LOG_INF("XENV", "AHT20 sample: %d tenths C, %u tenths %%RH", reading.temperature,
          static_cast<unsigned>(reading.humidity));
  return reading;
#else
  return {};
#endif
}

}  // namespace hal_environment
