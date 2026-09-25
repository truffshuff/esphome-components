#include "nimble_base.h"

#include "esp_bt.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

// NimBLE's modlog defines LOG_LEVEL_* macros that collide with ESPHome names.
#undef LOG_LEVEL_NONE
#undef LOG_LEVEL_ERROR
#undef LOG_LEVEL_WARN
#undef LOG_LEVEL_INFO
#undef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_VERBOSE

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>

extern "C" void ble_store_config_init(void);

namespace esphome::nimble_base {

static const char *const TAG = "nimble_base";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
NimBLEBase *global_nimble_base = nullptr;

// Construct-on-first-use: nimble_improv registers from its constructor, which
// can run before this translation unit's statics are initialised.
static std::vector<const struct ble_gatt_svc_def *> &gatt_services() {
  static std::vector<const struct ble_gatt_svc_def *> services;
  return services;
}

NimBLEBase::NimBLEBase() { global_nimble_base = this; }

void NimBLEBase::register_gatt_services(const struct ble_gatt_svc_def *services) {
  gatt_services().push_back(services);
}

void NimBLEBase::setup() {
  // The Bluetooth stack keeps its bond store and PHY calibration in NVS.
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS init returned %s, erasing", esp_err_to_name(err));
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    this->state_ = StackState::FAILED;
    this->mark_failed();
    return;
  }

  // Classic BT is never used; give its controller memory back once.
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  if (!this->enable_on_boot_) {
    this->state_ = StackState::DISABLED;
    return;
  }
  if (!this->start_stack_()) {
    // esp32_ble parity: a stack that cannot start fails the component, and
    // every dependent reports itself failed.
    this->mark_failed();
  }
}

void NimBLEBase::apply_host_config_() {
  // Must be set before the host starts: ble_hs_init() keeps these, and the
  // sync callback can fire as soon as the host task runs.
  ble_hs_cfg.sync_cb = NimBLEBase::on_sync_;
  ble_hs_cfg.reset_cb = NimBLEBase::on_reset_;
  // A full bond store evicts the oldest bond instead of failing every new
  // pairing (the recycling Bluedroid does on its own).
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  ble_hs_cfg.sm_io_cap = this->io_capability_;
  ble_hs_cfg.sm_bonding = this->bonding_ ? 1 : 0;
  ble_hs_cfg.sm_mitm = this->mitm_ ? 1 : 0;
  ble_hs_cfg.sm_sc = this->secure_connections_ ? 1 : 0;
  // Distribute encryption and identity keys both ways: the identity key is
  // what lets a bonded peer using a resolvable private address be recognised.
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

bool NimBLEBase::start_stack_() {
  this->state_ = StackState::STARTING;
  this->host_synced_.store(false);

  this->apply_host_config_();

  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
    this->state_ = StackState::FAILED;
    return false;
  }

  if (this->started_once_) {
    // A previous enable registered the services; the host keeps that table
    // across a deinit, so start from an empty server before adding them again.
    ble_gatts_reset();
  }
  ble_svc_gap_init();
  ble_svc_gatt_init();

  const std::string &name = this->device_name_.empty() ? App.get_name() : this->device_name_;
  int rc = ble_svc_gap_device_name_set(name.c_str());
  if (rc != 0) {
    ESP_LOGW(TAG, "Setting GAP device name failed: %d", rc);
  }

  for (const auto *services : gatt_services()) {
    rc = ble_gatts_count_cfg(services);
    if (rc == 0)
      rc = ble_gatts_add_svcs(services);
    if (rc != 0) {
      ESP_LOGE(TAG, "GATT service registration failed: %d", rc);
    }
  }

  ble_store_config_init();

  // nimble_port_init() does not start the host task on any IDF version; the
  // host only runs (and only syncs) once this is called.
  nimble_port_freertos_init(NimBLEBase::host_task_);
  this->started_once_ = true;
  ESP_LOGD(TAG, "NimBLE host started, waiting for sync");
  return true;
}

void NimBLEBase::stop_stack_() {
  // Blocks until the host has stopped: every connection is terminated and
  // every procedure failed first (ble_hs_stop).
  int rc = nimble_port_stop();
  if (rc != 0) {
    ESP_LOGW(TAG, "nimble_port_stop failed: %d", rc);
  }
  // The host task deletes itself once nimble_port_run() returns (host_task_).
  esp_err_t err = nimble_port_deinit();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "nimble_port_deinit failed: %s", esp_err_to_name(err));
  }
  this->host_synced_.store(false);
}

void NimBLEBase::enable() {
  if (this->state_ != StackState::DISABLED)
    return;
  ESP_LOGD(TAG, "Enabling BLE");
  if (!this->start_stack_()) {
    ESP_LOGE(TAG, "BLE enable failed");
  }
}

void NimBLEBase::disable() {
  if (this->state_ != StackState::ACTIVE && this->state_ != StackState::STARTING)
    return;
  ESP_LOGD(TAG, "Disabling BLE");
  // Listeners settle first: they must not wait for completions the stopped
  // stack will never deliver.
  for (auto *listener : this->listeners_) {
    listener->on_nimble_disabling();
  }
  this->stop_stack_();
  this->state_ = StackState::DISABLED;
}

void NimBLEBase::loop() {
  // Host-task callbacks only set these; listeners run here, on the main loop.
  uint32_t resets = this->reset_count_.load();
  if (resets != this->reset_count_seen_) {
    this->reset_count_seen_ = resets;
    int reason = this->last_reset_reason_.load();
    if (this->state_ == StackState::ACTIVE)
      this->state_ = StackState::STARTING;
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
    for (auto *listener : this->listeners_) {
      listener->on_nimble_reset(reason);
    }
  }

  bool synced = this->host_synced_.load();
  if (synced && this->state_ == StackState::STARTING) {
    this->state_ = StackState::ACTIVE;
    ESP_LOGD(TAG, "NimBLE host synced");
    for (auto *listener : this->listeners_) {
      listener->on_nimble_synced();
    }
  } else if (!synced && this->state_ == StackState::ACTIVE) {
    this->state_ = StackState::STARTING;
  }
}

void NimBLEBase::on_sync_() {
  if (global_nimble_base == nullptr)
    return;
  // Make sure a usable identity address exists before anyone scans or connects.
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(TAG, "No usable BLE identity address: %d", rc);
    return;
  }
  global_nimble_base->host_synced_.store(true);
}

void NimBLEBase::on_reset_(int reason) {
  if (global_nimble_base == nullptr)
    return;
  global_nimble_base->host_synced_.store(false);
  global_nimble_base->last_reset_reason_.store(reason);
  global_nimble_base->reset_count_.fetch_add(1);
}

void NimBLEBase::host_task_(void *param) {
  // Returns only after nimble_port_stop().
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void NimBLEBase::get_mac_msb_first(uint8_t out[MAC_ADDRESS_SIZE]) const {
  // esp_read_mac yields the controller's public address in printable order and,
  // unlike the host's identity copy, is valid before the host has synced.
  if (esp_read_mac(out, ESP_MAC_BT) != ESP_OK) {
    memset(out, 0, MAC_ADDRESS_SIZE);
  }
}

// [[maybe_unused]]: only ESP_LOGCONFIG uses it, which the logger compiles out below CONFIG level.
[[maybe_unused]] static const char *state_to_string(StackState state) {
  switch (state) {
    case StackState::OFF:
      return "OFF";
    case StackState::STARTING:
      return "STARTING";
    case StackState::ACTIVE:
      return "ACTIVE";
    case StackState::DISABLED:
      return "DISABLED";
    case StackState::FAILED:
      return "FAILED";
  }
  return "UNKNOWN";
}

void NimBLEBase::dump_config() {
  uint8_t mac[MAC_ADDRESS_SIZE];
  this->get_mac_msb_first(mac);
  char mac_str[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(mac, mac_str);
  ESP_LOGCONFIG(TAG,
                "NimBLE:\n"
                "  State: %s\n"
                "  MAC: %s\n"
                "  IO capability: %u\n"
                "  Bonding: %s, MITM: %s, Secure connections: %s\n"
                "  Connection timeout: %" PRIu32 " ms\n"
                "  GATT service sets: %u",
                state_to_string(this->state_), mac_str, this->io_capability_, YESNO(this->bonding_),
                YESNO(this->mitm_), YESNO(this->secure_connections_), this->connection_timeout_ms_,
                static_cast<unsigned>(gatt_services().size()));
}

}  // namespace esphome::nimble_base
