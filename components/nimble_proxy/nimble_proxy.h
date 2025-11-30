#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/defines.h"

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
#include <array>
#include <string>
#include <mutex>
#include <set>

#ifndef BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE
#define BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE 5
#endif

#ifndef BLUETOOTH_PROXY_MAX_CONNECTIONS
#define BLUETOOTH_PROXY_MAX_CONNECTIONS 3
#endif

namespace esphome {
namespace nimble_proxy {

// Connection state enum for tracking BLE connection lifecycle
enum class NimBLEConnectionState : uint8_t {
  IDLE = 0,              // No connection, slot available
  CONNECTING,            // ble_gap_connect() called
  CONNECTED,             // BLE_GAP_EVENT_CONNECT received
  DISCOVERING_SERVICES,  // ble_gattc_disc_all_svcs() in progress
  DISCOVERING_CHARS,     // ble_gattc_disc_all_chrs() in progress
  DISCOVERING_DSCS,      // ble_gattc_disc_all_dscs() in progress
  READY,                 // Discovery complete, ready for operations
  PAIRING,               // ble_gap_security_initiate() in progress
  DISCONNECTING,         // ble_gap_terminate() called
  ERROR                  // Connection failed, retry possible
};

// Connection tracking structure
struct NimBLEConnection {
  // Identity
  uint64_t address{0};
  uint8_t address_type{BLE_ADDR_PUBLIC};
  uint16_t conn_handle{BLE_HS_CONN_HANDLE_NONE};

  // State management
  NimBLEConnectionState state{NimBLEConnectionState::IDLE};
  uint32_t state_timestamp{0};

  // Connection parameters
  uint16_t mtu{23};
  uint16_t conn_interval{0};
  uint16_t conn_latency{0};
  uint16_t supervision_timeout{0};

  // Service discovery state
  std::vector<ble_gatt_svc> services;
  std::vector<ble_gatt_chr> characteristics;
  std::vector<ble_gatt_dsc> descriptors;

  // Discovery iteration state
  int current_service_idx{-1};
  int current_char_idx{-1};
  bool discovery_complete{false};

  // Security
  bool encrypted{false};
  bool bonded{false};

  // Notification subscriptions
  std::set<uint16_t> subscribed_handles;

  // Retry policy
  struct {
    uint8_t max_attempts{3};
    uint32_t retry_delay_ms{1000};
    uint8_t attempt_count{0};
    uint32_t last_attempt_time{0};
  } retry_policy;

  void reset() {
    address = 0;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    state = NimBLEConnectionState::IDLE;
    services.clear();
    characteristics.clear();
    descriptors.clear();
    subscribed_handles.clear();
    retry_policy.attempt_count = 0;
  }

  bool is_active() const {
    return state != NimBLEConnectionState::IDLE;
  }
};

/**
 * NimBLEProxy - Bluetooth LE Proxy for Home Assistant
 *
 * Provides BLE scanning and advertising functionality for Home Assistant integration:
 * - Scans for BLE advertisements and forwards them to Home Assistant
 * - Supports GATT service registration for features like Improv WiFi provisioning
 * - Manages API connections and advertisement batching
 * - Requires nimble_base component for BLE stack initialization
 */
class NimBLEProxy : public Component {
 public:
  NimBLEProxy();
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Run before nimble_base (which is at AFTER_BLUETOOTH=500)
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH + 1.0f; }

  void set_active(bool active) { this->active_ = active; }
  void set_connection_slots(uint8_t connection_slots) { this->connection_slots_ = connection_slots; }

  // Legacy method for backward compatibility
  void set_max_connections(uint8_t max_connections) { this->connection_slots_ = max_connections; }

  // Allow external components (like nimble_improv) to register GATT services
  // Must be called BEFORE setup() runs (use static registration)
  static void register_gatt_services(const struct ble_gatt_svc_def *services);
  static std::vector<const struct ble_gatt_svc_def *> &get_additional_services();

  // Allow external components to add 128-bit service UUIDs to advertising
  // Must be called BEFORE setup() runs (use static registration)
  static void register_advertising_service_uuid(const ble_uuid128_t *uuid);

  // API integration methods for Home Assistant connectivity
  void *get_api_connection() {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    return this->api_connection_;
  }
  void subscribe_api_connection(void *conn, uint32_t flags);
  void unsubscribe_api_connection(void *conn);

  // Bluetooth proxy API methods
#ifdef USE_API
  template<typename T> void bluetooth_device_request(const T &msg);
  template<typename T> void bluetooth_gatt_read(const T &msg);
  template<typename T> void bluetooth_gatt_write(const T &msg);
  template<typename T> void bluetooth_gatt_read_descriptor(const T &msg);
  template<typename T> void bluetooth_gatt_write_descriptor(const T &msg);
  template<typename T> void bluetooth_gatt_send_services(const T &msg);
  template<typename T> void bluetooth_gatt_notify(const T &msg);
#else
  template<typename T> void bluetooth_device_request(const T &msg) { }
  template<typename T> void bluetooth_gatt_read(const T &msg) { }
  template<typename T> void bluetooth_gatt_write(const T &msg) { }
  template<typename T> void bluetooth_gatt_read_descriptor(const T &msg) { }
  template<typename T> void bluetooth_gatt_write_descriptor(const T &msg) { }
  template<typename T> void bluetooth_gatt_send_services(const T &msg) { }
  template<typename T> void bluetooth_gatt_notify(const T &msg) { }
#endif
  void send_connections_free(void *conn) { }
  void bluetooth_scanner_set_mode(bool mode);
  uint32_t get_feature_flags();
  std::string get_bluetooth_mac_address_pretty();

 protected:
  bool active_{true};
  uint8_t connection_slots_{3};  // Renamed from max_connections_ to match Bluedroid API
  bool initialized_{false};
  bool host_task_started_{false};
  bool scanning_{false};

  // API connection tracking (void* to avoid including api headers)
  // We only support one active API connection at a time (like native bluetooth_proxy)
  // Protected by mutex since it's accessed from NimBLE host thread and ESPHome main thread
  void *api_connection_{nullptr};
  std::mutex api_connection_mutex_;

  // Advertisement batching for Home Assistant (opaque buffer to avoid header dependencies)
  void *adv_buffer_{nullptr};
  uint16_t adv_buffer_count_{0};
  uint32_t last_send_time_{0};
  bool adv_buffer_allocated_{false};

  // Connection tracking
  std::array<NimBLEConnection, BLUETOOTH_PROXY_MAX_CONNECTIONS> connections_;
  std::mutex connections_mutex_;

  void start_scan_();
  void stop_scan_();
  void start_advertising_();
  void setup_services_();

  void send_advertisements_();
  void add_advertisement_(const ble_gap_disc_desc *disc);
  void send_scanner_state_();

  // Connection management helpers
  NimBLEConnection* get_connection_(uint64_t address, bool reserve);
  NimBLEConnection* get_connection_by_handle_(uint16_t conn_handle);
  void reset_connection_(NimBLEConnection *conn);
  void send_connection_response_(NimBLEConnection *conn, bool connected, int error = 0);

  // GAP event handlers for connections
  void handle_gap_connect_(struct ble_gap_event *event, NimBLEConnection *conn);
  void handle_gap_disconnect_(struct ble_gap_event *event, NimBLEConnection *conn);
  void handle_gap_notify_(struct ble_gap_event *event, NimBLEConnection *conn);

  // Service discovery helpers (Phase 2.2)
  void start_service_discovery_(NimBLEConnection *conn);
  void start_char_discovery_(NimBLEConnection *conn);
  void start_dsc_discovery_(NimBLEConnection *conn);
  void send_service_response_(NimBLEConnection *conn);

  // GATT operation helpers (Phase 2.3)
  static int on_read_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg);
  static int on_write_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);

  // Discovery callbacks (Phase 2.2)
  static int on_disc_svc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg);
  static int on_disc_chr_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);
  static int on_disc_dsc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

  // UUID conversion helpers
  static std::array<uint64_t, 2> nimble_uuid_to_api_(const ble_uuid_any_t *uuid);
  static ble_uuid_any_t api_uuid_to_nimble_(const std::array<uint64_t, 2> &uuid);
  static uint16_t find_cccd_handle_(NimBLEConnection *conn, uint16_t char_handle);

  // NimBLE callbacks
  static int gap_event_handler_(struct ble_gap_event *event, void *arg);
  static int scan_callback_(struct ble_gap_event *event, void *arg);
  static void on_sync_();
  static void on_reset_(int reason);
  static void host_task_(void *param);
};

}  // namespace nimble_proxy
}  // namespace esphome
