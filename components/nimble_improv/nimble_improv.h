#pragma once

// Improv WiFi provisioning over NimBLE: the NimBLE counterpart of esp32_improv.
//
// Behaviour follows esp32_improv (ESPHome 2026.9.0): the service only runs
// while WiFi needs provisioning - no stored network, or no connection for
// wifi_timeout - and stops 10 s after the device is provisioned. The optional
// binary_sensor authorizer gates WiFi settings for authorized_duration; with
// no authorizer the service starts authorized (esp32_improv parity; it is only
// reachable while WiFi is unprovisioned). The WiFi component drives
// esp32_improv's start/stop directly and has no hook for other components, so
// this component watches the WiFi state itself and applies the same triggers.
//
// NimBLE runs GATT access and GAP callbacks on its host task; they only copy
// data or flags, and everything else runs on the main loop.

#include "esphome/components/improv_base/improv_base.h"
#include "esphome/components/nimble_base/nimble_base.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#ifdef USE_NIMBLE_IMPROV_STATE_CALLBACK
#include "esphome/core/automation.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_OUTPUT
#include "esphome/components/output/binary_output.h"
#endif

#include <improv.h>

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <vector>

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace esphome::nimble_improv {

class NimBLEImprov : public Component, public improv_base::ImprovBase, public nimble_base::NimBLEStackListener {
 public:
  NimBLEImprov();
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void start();
  void stop();
  bool is_active() const { return this->state_ != improv::STATE_STOPPED; }
  bool should_start() const { return this->should_start_; }

#ifdef USE_NIMBLE_IMPROV_STATE_CALLBACK
  template<typename F> void add_on_state_callback(F &&callback) {
    this->state_callback_.add(std::forward<F>(callback));
  }
#endif
#ifdef USE_BINARY_SENSOR
  void set_authorizer(binary_sensor::BinarySensor *authorizer) { this->authorizer_ = authorizer; }
#endif
#ifdef USE_OUTPUT
  void set_status_indicator(output::BinaryOutput *status_indicator) { this->status_indicator_ = status_indicator; }
#endif
  void set_identify_duration(uint32_t duration) { this->identify_duration_ = duration; }
  void set_authorized_duration(uint32_t duration) { this->authorized_duration_ = duration; }
  void set_wifi_timeout(uint32_t timeout) { this->wifi_timeout_ = timeout; }
  uint32_t get_wifi_timeout() const { return this->wifi_timeout_; }
  improv::State get_improv_state() const { return this->state_; }
  improv::Error get_improv_error_state() const { return this->error_state_; }

  // ---- nimble_base::NimBLEStackListener ----
  void on_nimble_synced() override;
  void on_nimble_reset(int reason) override;
  void on_nimble_disabling() override;

  // NimBLE GATT access / GAP callbacks (host task).
  static int gatt_access_(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
  static int gap_event_(struct ble_gap_event *event, void *arg);

 protected:
  void set_state_(improv::State state, bool update_advertising = true);
  void set_error_(improv::Error error);
  improv::State get_initial_state_() const;
  void send_response_(std::span<const uint8_t> response);
  void process_incoming_data_();
  void on_wifi_connect_timeout_();
  void check_wifi_connection_();
  bool check_identify_();
  void set_status_indicator_state_(bool state);
  uint8_t capabilities_() const;
  void watch_wifi_();
  void migrate_legacy_credentials_();

  // Advertising (esp32_ble advertising parity).
  void update_advertising_type_();
  void apply_advertising_();
  void start_advertising_();
  void stop_advertising_();

  bool should_start_{false};
  bool service_running_{false};  // the stack is up and the GATT handles are resolved
  uint32_t identify_start_{0};
  uint32_t identify_duration_{10000};
  uint32_t authorized_start_{0};
  uint32_t authorized_duration_{60000};
  uint32_t wifi_timeout_{90000};
  uint32_t last_wifi_connected_{0};
  uint32_t stop_at_{0};  // pending delayed stop (esp32_improv's "end-service" timeout)
  uint32_t wifi_connect_deadline_{0};
  wifi::WiFiAP connecting_sta_;

  // RPC bytes written by the client (host task) for the main loop.
  std::mutex incoming_mutex_;
  std::vector<uint8_t> incoming_data_;
  std::vector<uint8_t> pending_data_;  // main-loop working copy
  // Last RPC result, read by the access callback.
  std::mutex response_mutex_;
  std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> response_{};
  uint16_t response_len_{0};

  // Read by the host task's access callback.
  std::atomic<uint8_t> state_byte_{improv::STATE_STOPPED};
  std::atomic<uint8_t> error_byte_{improv::ERROR_NONE};
  std::atomic<uint16_t> conn_handle_{0xFFFF};  // BLE_HS_CONN_HANDLE_NONE
  std::atomic<bool> disconnected_{false};      // a client dropped; handled on the loop

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *authorizer_{nullptr};
#endif
#ifdef USE_OUTPUT
  output::BinaryOutput *status_indicator_{nullptr};
#endif
  improv::State state_{improv::STATE_STOPPED};
  improv::Error error_state_{improv::ERROR_NONE};
#ifdef USE_NIMBLE_IMPROV_STATE_CALLBACK
  CallbackManager<void(improv::State, improv::Error)> state_callback_{};
#endif
  bool status_indicator_state_{false};
  uint32_t last_name_adv_time_{0};
  bool advertising_device_name_{false};
  bool advertising_{false};
  bool legacy_checked_{false};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern NimBLEImprov *global_nimble_improv;

}  // namespace esphome::nimble_improv
