#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

// ESP-IDF native NimBLE headers
#include "esp_bt.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <vector>
#include <string>

namespace esphome {
namespace nimble_base {

class NimBLEBase : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // Allow external components (like nimble_improv, nimble_proxy) to register GATT services
  // Must be called BEFORE setup() runs (use static registration in component constructors)
  static void register_gatt_services(const struct ble_gatt_svc_def *services);
  static std::vector<const struct ble_gatt_svc_def *> &get_additional_services();

  // Allow external components to add 128-bit service UUIDs to advertising
  // Must be called BEFORE setup() runs (use static registration in component constructors)
  static void register_advertising_service_uuid(const ble_uuid128_t *uuid);
  static std::vector<const ble_uuid128_t *> &get_advertising_service_uuids();

  // Check if NimBLE host is initialized and synced
  bool is_initialized() const { return this->initialized_; }

  // Device name and MAC utilities
  static std::string get_device_name_with_mac(const std::string &base_name, bool short_form = false);
  static std::string get_mac_address_pretty();

  // Callback registration for sync events
  using SyncCallback = void (*)();
  static void register_sync_callback(SyncCallback callback);

 protected:
  bool initialized_{false};
  bool host_task_started_{false};

  // NimBLE callbacks
  static void on_sync_();
  static void on_reset_(int reason);
  static void host_task_(void *param);
};

}  // namespace nimble_base
}  // namespace esphome
