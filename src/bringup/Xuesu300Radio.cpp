#if FREEINK_DEVICE_XUESU_300

#include <esp_wifi.h>

// The original firmware owns the NVS partition. Keep Arduino and NimBLE out
// of their default NVS initialization and recovery paths on this board.
extern "C" esp_err_t __wrap_nvs_flash_init() { return ESP_OK; }

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  wifi_init_config_t ramConfig = *config;
  ramConfig.nvs_enable = 0;
  return __real_esp_wifi_init(&ramConfig);
}

#endif
