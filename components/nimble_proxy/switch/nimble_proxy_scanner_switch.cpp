#include "nimble_proxy_scanner_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nimble_proxy {

static const char *const TAG = "nimble_proxy.switch";

void NimBLEProxyScannerSwitch::setup() {
  // Restore initial state from parent
  if (this->parent_ != nullptr) {
    bool initial_state = this->parent_->is_scanning();
    this->publish_state(initial_state);
  }
}

void NimBLEProxyScannerSwitch::dump_config() {
  LOG_SWITCH("", "NimBLE Proxy Scanner Switch", this);
}

void NimBLEProxyScannerSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "Cannot control scanner - parent not set");
    return;
  }

  ESP_LOGD(TAG, "Setting BLE scanner state to: %s", state ? "ON" : "OFF");

  // Use the parent's bluetooth_scanner_set_mode method
  this->parent_->bluetooth_scanner_set_mode(state);

  // Publish the new state
  this->publish_state(state);
}

}  // namespace nimble_proxy
}  // namespace esphome
