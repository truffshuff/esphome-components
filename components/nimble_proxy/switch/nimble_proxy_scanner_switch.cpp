#include "nimble_proxy_scanner_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nimble_proxy {

static const char *const TAG = "nimble_proxy.switch";

void NimBLEProxyScannerSwitch::setup() {
  if (this->parent_ == nullptr) {
    return;
  }

  auto restored = this->get_initial_state_with_restore_mode();
  if (restored.has_value()) {
    this->write_state(*restored);
    return;
  }

  this->publish_state(this->parent_->is_scanner_enabled());
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

  // Publish effective desired scanner mode.
  this->publish_state(this->parent_->is_scanner_enabled());
}

}  // namespace nimble_proxy
}  // namespace esphome
