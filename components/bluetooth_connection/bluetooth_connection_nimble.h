// NimBLE (ESP32) GATT client backend: the ble_device_base::BLEGattConnection
// implementation the vendored bluetooth_connection hub wrapper drives on a
// NimBLE build, in place of upstream's BluedroidGattClient.
//
// [nimble] Not part of upstream ESPHome; added by truffshuff/esphome-components.
//
// Structure follows upstream's rp2 (BTstack) backend, the other non-Bluedroid
// backend: an engine state machine on the main loop, one outgoing connect
// stack-wide (NimBLE allows a single ble_gap_connect at a time), the scan
// stopped for the duration of each connect attempt (NimBLE refuses to connect
// while discovery runs), and a transient service table streamed by the
// wrapper's generic table streamer. NimBLE callbacks run on the host task and
// only copy into events_; loop() delivers every GattClientListener call on the
// main loop.

#pragma once

#include "esphome/core/defines.h"

#if defined(USE_NIMBLE_BLE) && defined(USE_BLE_GATT_CLIENT)

#include "esphome/components/ble_device_base/ble_gatt_client.h"
#include "esphome/components/nimble_base/nimble_base.h"
#include "esphome/components/nimble_base/nimble_event_ring.h"
#include "esphome/components/nimble_tracker/nimble_tracker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

// NimBLE host types stay out of this header (it reaches every proxy
// translation unit through the backend alias; see nimble_base.h).
struct ble_gap_event;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_chr;
struct ble_gatt_dsc;
struct ble_gatt_attr;

namespace esphome::bluetooth_connection {

// Caps for the transient service table (PSRAM when available, freed after
// streaming). Above rp2's 16/96/96: the table is not in internal RAM here. A
// peer exceeding a cap fails discovery with INSUFFICIENT_RESOURCES rather than
// streaming a truncated database a V3 client would cache.
static constexpr uint16_t NIMBLE_GATT_MAX_SERVICES = 32;
static constexpr uint16_t NIMBLE_GATT_MAX_CHARACTERISTICS = 160;
static constexpr uint16_t NIMBLE_GATT_MAX_DESCRIPTORS = 192;

// Concurrent notify subscriptions per connection (esp32_ble max_notifications
// defaults to 12).
static constexpr uint8_t NIMBLE_GATT_MAX_NOTIFY_SUBSCRIPTIONS = 16;

// ATT maximum attribute value length.
static constexpr uint16_t NIMBLE_GATT_MAX_ATTR_LEN = 512;

// Reads in flight at once. Bluedroid queues GATT requests per link, so a client
// may overlap reads; each needs its own reassembly buffer for long values.
static constexpr uint8_t NIMBLE_GATT_MAX_READS = 4;

// Host-task -> main-loop events. Sized for notification bursts between two
// loop passes; overflow drops notifications (counted) and forces a teardown if
// a control event was lost (rp2 parity).
static constexpr uint16_t NIMBLE_GATT_EVENT_QUEUE_SIZE = 16;

struct NimBLEGattEvent {
  enum Type : uint8_t {
    CONNECTED,       // value = conn handle
    CONNECT_FAILED,  // status = NimBLE rc
    DISCONNECTED,    // status = NimBLE disconnect reason
    MTU_EXCHANGED,   // status = rc, value = MTU
    DISCOVERY_DONE,  // status = mapped error
    READ_DONE,       // status = mapped error, handle, data
    WRITE_DONE,      // status = mapped error, handle
    NOTIFY,          // handle, data
    ENC_CHANGE,      // status = NimBLE rc
  };
  Type type{CONNECTED};
  int status{0};
  uint16_t handle{0};
  uint16_t value{0};
  uint16_t len{0};
  uint8_t data[NIMBLE_GATT_MAX_ATTR_LEN]{};
};

class NimBLEGattClient final : public Component,
                               public Parented<nimble_tracker::NimBLETracker>,
                               public nimble_base::NimBLEStackListener
#ifdef USE_OTA_STATE_LISTENER
    ,
                               public ota::OTAGlobalStateListener
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_listener(ble_device_base::GattClientListener *listener) { this->listener_ = listener; }

  // ---- ble_device_base::BLEGattConnection contract ----
  int connect(uint64_t address, uint8_t addr_type);
  int gatt_disconnect();
  // Teardown starts inside gatt_disconnect(); nothing is ever scheduled.
  bool cancel_gatt_disconnect() { return false; }
  int discover_services();
  int read_characteristic(uint16_t handle) { return this->start_read_(handle); }
  int write_characteristic(uint16_t handle, const uint8_t *data, uint16_t len, bool response);
  int read_descriptor(uint16_t handle) { return this->start_read_(handle); }
  int write_descriptor(uint16_t handle, const uint8_t *data, uint16_t len) {
    return this->write_characteristic(handle, data, len, true);
  }
  int notify_characteristic(uint16_t handle, bool enable);
  int pair();
  int update_connection_params(uint16_t min_interval, uint16_t max_interval, uint16_t latency, uint16_t timeout);
  ble_device_base::GattServiceTable get_service_table();
  void set_connection_type(ble_device_base::ConnectionType ct) { this->connection_type_ = ct; }
  void release_services();

#ifdef USE_OTA_STATE_LISTENER
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) override;
#endif

  // ---- nimble_base::NimBLEStackListener ----
  void on_nimble_disabling() override;

 protected:
  enum class EngineState : uint8_t {
    IDLE,
    CONNECT_PENDING,  // another backend owns the single ble_gap_connect
    CONNECTING,       // ble_gap_connect issued
    MTU_EXCHANGE,     // link up, MTU exchange in flight
    READY,            // on_connection_state(true) delivered
    DISCONNECTING,
  };

  struct ServiceArena {
    ble_device_base::GattService services[NIMBLE_GATT_MAX_SERVICES];
    ble_device_base::GattCharacteristic characteristics[NIMBLE_GATT_MAX_CHARACTERISTICS];
    ble_device_base::GattDescriptor descriptors[NIMBLE_GATT_MAX_DESCRIPTORS];
  };

  struct ReadContext {
    NimBLEGattClient *owner{nullptr};
    // Claimed on the main loop, released by the host-task completion.
    std::atomic<bool> in_use{false};
    uint16_t handle{0};
    uint16_t len{0};
    uint8_t buffer[NIMBLE_GATT_MAX_ATTR_LEN];
  };

  // Host-task callbacks (copy-and-queue only).
  static int gap_event_(struct ble_gap_event *event, void *arg);
  static int mtu_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg);
  static int disc_svc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service,
                          void *arg);
  static int disc_chr_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                          void *arg);
  static int disc_dsc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                          const struct ble_gatt_dsc *dsc, void *arg);
  static int read_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg);
  static int write_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                       void *arg);

  // Discovery chaining, host task (under arena_mutex_).
  int issue_characteristic_query_();
  int issue_descriptor_query_();
  void finish_discovery_(int error);

  void push_event_(NimBLEGattEvent::Type type, int status, uint16_t handle = 0, uint16_t value = 0,
                   const uint8_t *data = nullptr, uint16_t len = 0);

  // Main loop.
  void handle_event_(const NimBLEGattEvent &event);
  void handle_connected_(uint16_t conn_handle);
  void fail_connection_(int reason);
  int try_gap_connect_();
  void cleanup_link_state_();
  void release_scan_inhibit_();
  /// "AA:BB:CC:DD:EE:FF" (display order) into a caller-owned 18-byte buffer.
  const char *peer_str_(char *buf) const;
  int start_read_(uint16_t handle);
  bool notify_subscribed_(uint16_t handle) const;
  uint16_t conn_handle_() const { return this->conn_handle_atomic_.load(); }

  ble_device_base::GattClientListener *listener_{nullptr};
  nimble_base::EventRing<NimBLEGattEvent, NIMBLE_GATT_EVENT_QUEUE_SIZE> events_;

  // Service table; host-task discovery callbacks write it under arena_mutex_,
  // the main loop reads it only after DISCOVERY_DONE.
  // Recursive: discovery callbacks chain the next query while holding it.
  std::recursive_mutex arena_mutex_;
  ServiceArena *arena_{nullptr};
  uint16_t service_count_{0};
  uint16_t char_count_{0};
  uint16_t desc_count_{0};
  uint16_t disc_service_cursor_{0};
  uint16_t disc_char_cursor_{0};

  ReadContext *reads_{nullptr};  // NIMBLE_GATT_MAX_READS, allocated in setup()

  std::array<uint16_t, NIMBLE_GATT_MAX_NOTIFY_SUBSCRIPTIONS> notify_subscriptions_{};
  uint8_t notify_subscription_count_{0};

  uint32_t connect_started_{0};
  uint32_t connect_retry_ms_{0};
  uint32_t disconnecting_started_{0};
  uint32_t dropped_notifications_{0};
  // A control event could not be queued; loop() forces a teardown.
  std::atomic<bool> control_lost_{false};

  // Written by the host task (connect event / MTU), read on the main loop.
  std::atomic<uint16_t> conn_handle_atomic_{0xFFFF};  // BLE_HS_CONN_HANDLE_NONE
  std::atomic<uint16_t> mtu_{ble_device_base::DEFAULT_ATT_MTU};

  uint8_t peer_addr_[MAC_ADDRESS_SIZE]{};  // controller order (LSB first)
  uint8_t peer_addr_type_{0};
  uint8_t engine_index_{0};
  ble_device_base::ConnectionType connection_type_{ble_device_base::ConnectionType::V3_WITHOUT_CACHE};
  EngineState state_{EngineState::IDLE};
  bool holds_scan_inhibit_{false};
  bool link_up_{false};
  bool cancel_requested_{false};
  bool discovery_in_progress_{false};

  // NimBLE allows one outgoing connect stack-wide; only the owner may cancel.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static std::atomic<NimBLEGattClient *> connect_owner;
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
  static uint8_t instance_count;
};

}  // namespace esphome::bluetooth_connection

#endif  // USE_NIMBLE_BLE && USE_BLE_GATT_CLIENT
