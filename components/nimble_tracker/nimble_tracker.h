#pragma once

// NimBLE BLE tracker for ESP32: the ble_device_base::BLEHub implementation
// that stands in for esp32_ble_tracker on a NimBLE build.
//
// Scanning mirrors esp32_ble_tracker: continuous scan periods of `duration`
// (on_scan_end at each), active/passive with runtime switching, the scan
// stopped for every outgoing connect, the connection-time scan window, the
// coexistence preference while links are up, and the OTA pause. Advertisement
// and scan response arrive as separate NimBLE reports; they are merged into
// one frame (ble_device_base::ScanResponseMerger), which is what the IDF
// Bluedroid stack hands esp32_ble_tracker. Neutral ble_device_base consumers
// (on_ble_advertise triggers, bluetooth_proxy, BLEHub sensors) bind to it
// unchanged.
//
// NimBLE callbacks run on the host task and only copy into scan_events_; all
// dispatch happens in loop() on the main loop.

#include "esphome/core/defines.h"

#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/ble_device_base/ble_hub.h"
#include "esphome/components/ble_device_base/scan_response_merger.h"
#include "esphome/components/nimble_base/nimble_base.h"
#include "esphome/components/nimble_base/nimble_event_ring.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif

#include <cstdint>

// Kept out of this header: it is reached through the BLEHub alias from every
// proxy translation unit (see nimble_base.h).
struct ble_gap_event;

namespace esphome::nimble_tracker {

/// One host-task scan callback, copied for the main loop.
struct ScanEvent {
  enum Kind : uint8_t { REPORT, COMPLETE };
  Kind kind{REPORT};
  uint8_t event_type{0};  // HCI advertising report type (ADV_IND = 0 ... SCAN_RSP = 4)
  uint8_t addr_type{0};
  int8_t rssi{0};
  uint8_t mac[MAC_ADDRESS_SIZE]{};  // controller order (LSB first)
  uint8_t data_len{0};
  uint8_t data[31]{};  // legacy PDU payload
  int reason{0};       // COMPLETE: why the scan ended
};

// Drained every loop iteration. 64 reports cover ~a second of a busy
// environment at the default duty cycle; overflow is counted, never blocking.
static constexpr uint16_t SCAN_EVENT_QUEUE_SIZE = 64;

class NimBLETracker : public Component,
                      public nimble_base::NimBLEStackListener,
                      public Parented<nimble_base::NimBLEBase>
#ifdef USE_OTA_STATE_LISTENER
    ,
                      public ota::OTAGlobalStateListener
#endif
{
 public:
  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

#ifdef USE_OTA_STATE_LISTENER
  // esp32_ble_tracker parity: no scanning during an OTA transfer.
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif

  // ---- configuration (codegen) ----
  void set_scan_interval(uint32_t units) { this->scan_interval_ = units; }
  void set_scan_window(uint32_t units) { this->scan_window_ = units; }
  void set_connection_scan_window(uint32_t units) { this->connection_scan_window_ = units; }
  void set_scan_duration(uint32_t ms) { this->scan_duration_ = ms; }
  void set_scan_active(bool active) { this->scan_active_ = active; }
  void set_configured_continuous(bool continuous) {
    this->configured_continuous_ = continuous;
    this->scan_continuous_ = continuous;
  }

  // ---- scan control (esp32_ble_tracker / rp2_ble_tracker surface) ----
  void start_scan();
  void stop_scan();
  void set_scan_continuous(bool continuous) { this->scan_continuous_ = continuous; }
  bool scan_continuous() const { return this->scan_continuous_; }
  bool configured_continuous() const { return this->configured_continuous_; }
  void restart_scan_duration();

  // ---- ble_device_base::BLEHub contract ----
  void register_listener(ble_device_base::ESPBTDeviceListener *listener) {
    this->dispatcher_.register_listener(listener);
  }
  void set_raw_advertisement_callback(ble_device_base::RawAdvertisementCallback callback) {
    this->dispatcher_.set_raw_advertisement_callback(callback);
  }
  static constexpr ble_device_base::HubCapabilities get_capabilities() {
#ifdef USE_BLE_GATT_CLIENT
    constexpr bool has_gatt = true;
#else
    constexpr bool has_gatt = false;
#endif
    return {.active_scan = true, .merges_scan_response = true, .gatt = has_gatt, .scan_mode_switch = true};
  }
  void get_adapter_mac(uint8_t out[MAC_ADDRESS_SIZE]) { this->parent_->get_mac_msb_first(out); }
  bool scan_running() { return this->scan_running_; }
  bool scan_active() { return this->scan_active_; }
  bool request_scan_mode(bool active);
  void set_scanner_state_callback(ble_device_base::ScannerStateCallback callback) {
    this->scanner_state_callback_ = callback;
  }
  ble_device_base::ScannerState get_scanner_state() { return this->scanner_state_; }

  // ---- GATT backend coordination ----
  /// An outgoing connect is starting: the scan is stopped now (NimBLE refuses
  /// ble_gap_connect() while discovery runs) and stays stopped until every
  /// holder released it. esp32_ble_tracker's promote-loop parity.
  void inhibit_scan();
  void release_scan_inhibit();
  /// Established links: select the connection scan window and hold the
  /// coexistence preference on Bluetooth while any link is up.
  void on_link_up();
  void on_link_down();

  // ---- nimble_base::NimBLEStackListener ----
  void on_nimble_synced() override;
  void on_nimble_reset(int reason) override;
  void on_nimble_disabling() override;

 protected:
  static int gap_event_(struct ble_gap_event *event, void *arg);

  bool controller_scan_start_();
  void start_scan_();
  /// Stop the controller scan. end_period: this ends the scan period
  /// (on_scan_end fires) rather than pausing it for a connect.
  void stop_scan_(bool end_period);
  void fire_scan_end_();
  void set_scanner_state_(ble_device_base::ScannerState state);
  void handle_event_(const ScanEvent &event);
  uint32_t desired_window_() const {
    return (this->connection_scan_window_ != 0 && this->links_up_ > 0) ? this->connection_scan_window_
                                                                         : this->scan_window_;
  }
  void update_coex_preference_();

  nimble_base::EventRing<ScanEvent, SCAN_EVENT_QUEUE_SIZE> scan_events_;
  ble_device_base::ScanResponseMerger merger_;
  ble_device_base::AdvDispatcher dispatcher_;
  ble_device_base::ScannerStateCallback scanner_state_callback_{};

  // esp32_ble_tracker defaults: 320 ms interval / 30 ms window, 5 min period.
  uint32_t scan_interval_{512};
  uint32_t scan_window_{48};
  uint32_t connection_scan_window_{0};  // 0 = keep the normal window
  uint32_t programmed_window_{0};       // window of the running scan
  uint32_t scan_duration_{300000};
  uint32_t scan_period_start_{0};
  uint32_t last_scan_start_attempt_{0};
  uint32_t dropped_reports_{0};
  uint8_t inhibit_count_{0};
  uint8_t links_up_{0};
  ble_device_base::ScannerState scanner_state_{ble_device_base::ScannerState::IDLE};
  bool scan_running_{false};
  bool scan_active_{true};
  bool configured_continuous_{true};
  // Plain bool: the merger binds its address.
  bool scan_continuous_{true};
  bool last_start_failed_{false};
  bool pending_start_{false};
  bool coex_prefer_bt_{false};
#ifdef USE_OTA_STATE_LISTENER
  bool ota_in_progress_{false};
  bool scan_continuous_before_ota_{false};
#endif
};

}  // namespace esphome::nimble_tracker
