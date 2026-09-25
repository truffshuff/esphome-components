#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32
#include "esphome/components/ble_device_base/ble_device.h"

namespace esphome {
namespace pinky_winky {

struct PinkyState {
  uint8_t battery_level;
  bool pressed;
  uint32_t ts;
};

class PinkyParser {

public:
  void setup();
  bool is_pinky_uuid(const ble_device_base::ESPBTUUID uuid);
  void set_secret(const std::string &secret);
  bool have_secret();

  optional<PinkyState> parse_state(const std::vector<uint8_t> &mfg_data, const uint32_t last_ts);

protected:
  std::string secret_;
  ble_device_base::ESPBTUUID company_uuid_;

};

}  // namespace pinky_winky
}  // namespace esphome

#endif
