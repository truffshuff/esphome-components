#pragma once

// NimBLE stack owner for ESP32: the NimBLE counterpart of esp32_ble.
//
// Brings the controller and host up (and down, for nimble_base.enable/disable),
// applies the security-manager configuration, owns GATT-server service
// registration, and hands the host task's sync/reset callbacks to the main
// loop through NimBLEStackListener so no consumer ever runs ESPHome code on the
// NimBLE host task.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include <atomic>
#include <string>
#include <vector>

// NimBLE's host headers stay out of this header on purpose: it is reached from
// every translation unit that includes the proxy (through the BLEHub alias),
// and those headers define macros (LOG_LEVEL_*, MIN/MAX, ...) that collide
// with ESPHome's API enums. Only the .cpp files include them.
struct ble_gatt_svc_def;

namespace esphome::nimble_base {

/// BLE_SM_IO_CAP_NO_IO (host/ble_sm.h); the Bluetooth-spec IO-capability value.
static constexpr uint8_t IO_CAP_NO_IO = 3;

/// Stack lifecycle events, always delivered on the ESPHome main loop.
class NimBLEStackListener {
 public:
  /// Host synced: first start, after a host reset, and after enable().
  virtual void on_nimble_synced() {}
  /// Host reset (controller/transport error). NimBLE has already failed every
  /// procedure and broken every link; it re-syncs on its own afterwards.
  virtual void on_nimble_reset(int reason) {}
  /// disable() is about to stop the stack: settle links and stop radio use now,
  /// no completion events follow (esp32_ble's ble_before_disabled parity).
  virtual void on_nimble_disabling() {}
};

enum class StackState : uint8_t {
  OFF,       // before setup()
  STARTING,  // host task running, waiting for sync (also after a host reset)
  ACTIVE,    // synced; GAP/GATT calls are valid
  DISABLED,  // stopped by disable() or enable_on_boot: false
  FAILED,    // controller/host init failed
};

class NimBLEBase : public Component {
 public:
  NimBLEBase();
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }

  // ---- configuration (codegen, before setup) ----
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_io_capability(uint8_t io_capability) { this->io_capability_ = io_capability; }
  void set_bonding(bool bonding) { this->bonding_ = bonding; }
  void set_mitm(bool mitm) { this->mitm_ = mitm; }
  void set_secure_connections(bool sc) { this->secure_connections_ = sc; }
  void set_enable_on_boot(bool enable_on_boot) { this->enable_on_boot_ = enable_on_boot; }
  void set_connection_timeout(uint32_t timeout_ms) { this->connection_timeout_ms_ = timeout_ms; }

  /// Outgoing-connection timeout (esp32_ble connection_timeout parity).
  uint32_t get_connection_timeout() const { return this->connection_timeout_ms_; }

  // ---- stack control (ble.enable / ble.disable parity) ----
  void enable();
  void disable();
  bool is_active() const { return this->state_ == StackState::ACTIVE; }
  StackState get_state() const { return this->state_; }

  void add_listener(NimBLEStackListener *listener) { this->listeners_.push_back(listener); }

  /// GATT server services (e.g. nimble_improv). Must be called before setup():
  /// NimBLE only accepts service registration before the host starts.
  static void register_gatt_services(const struct ble_gatt_svc_def *services);

  /// Public Bluetooth address in printable (MSB-first) order.
  void get_mac_msb_first(uint8_t out[MAC_ADDRESS_SIZE]) const;

 protected:
  bool start_stack_();
  void stop_stack_();
  void apply_host_config_();

  static void on_sync_();
  static void on_reset_(int reason);
  static void host_task_(void *param);

  std::vector<NimBLEStackListener *> listeners_;
  std::string device_name_;
  uint32_t connection_timeout_ms_{20000};

  // Written by the host task, consumed by loop().
  std::atomic<bool> host_synced_{false};
  std::atomic<uint32_t> reset_count_{0};
  std::atomic<int> last_reset_reason_{0};
  uint32_t reset_count_seen_{0};

  StackState state_{StackState::OFF};
  uint8_t io_capability_{IO_CAP_NO_IO};
  bool bonding_{true};
  bool mitm_{false};
  bool secure_connections_{true};
  bool enable_on_boot_{true};
  // Set once the stack has been initialised; a re-enable must reset the GATT
  // server registration before adding the services again.
  bool started_once_{false};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern NimBLEBase *global_nimble_base;

template<typename... Ts> class EnableAction : public Action<Ts...>, public Parented<NimBLEBase> {
 public:
  void play(const Ts &...x) override { this->parent_->enable(); }
};

template<typename... Ts> class DisableAction : public Action<Ts...>, public Parented<NimBLEBase> {
 public:
  void play(const Ts &...x) override { this->parent_->disable(); }
};

template<typename... Ts> class IsEnabledCondition : public Condition<Ts...>, public Parented<NimBLEBase> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_active(); }
};

}  // namespace esphome::nimble_base
