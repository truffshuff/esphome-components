#include "weather_helpers.h"
#include "esphome/core/log.h"

namespace weather_helpers_component {
  static const char *const TAG = "weather_helpers";

  // Empty init hook — never actually runs unless you want it to
  void setup() {
    ESP_LOGI(TAG, "weather_helpers component initialized");
  }
}