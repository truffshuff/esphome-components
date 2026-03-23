#include "nimble_base.h"

// Undefine NimBLE macros that conflict with ESPHome API enums
#ifdef LOG_LEVEL_NONE
#undef LOG_LEVEL_NONE
#endif
#ifdef LOG_LEVEL_ERROR
#undef LOG_LEVEL_ERROR
#endif
#ifdef LOG_LEVEL_WARN
#undef LOG_LEVEL_WARN
#endif
#ifdef LOG_LEVEL_INFO
#undef LOG_LEVEL_INFO
#endif
#ifdef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_DEBUG
#endif
#ifdef LOG_LEVEL_VERBOSE
#undef LOG_LEVEL_VERBOSE
#endif

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include <cstring>

extern "C" void ble_store_config_init(void);

namespace esphome {
namespace nimble_base {

static const char *const TAG = "nimble_base";

// Static pointer for callbacks
static NimBLEBase *global_nimble_base = nullptr;

// Construct-on-first-use accessors to avoid Static Initialization Order Fiasco.
// NimBLEProxy and NimBLEImprov constructors run during static init and call
// register_*() before translation-unit-level statics may be constructed.
// Returning a local-static reference guarantees the object is initialized on
// the first call, regardless of TU init order.
static std::vector<const struct ble_gatt_svc_def *> &get_additional_gatt_services_() {
  static std::vector<const struct ble_gatt_svc_def *> v;
  return v;
}
static std::vector<const ble_uuid128_t *> &get_advertising_service_uuids_() {
  static std::vector<const ble_uuid128_t *> v;
  return v;
}
static std::vector<NimBLEBase::SyncCallback> &get_sync_callbacks_() {
  static std::vector<NimBLEBase::SyncCallback> v;
  return v;
}

// Static methods for external service registration
void NimBLEBase::register_gatt_services(const struct ble_gatt_svc_def *services) {
  get_additional_gatt_services_().push_back(services);
  ESP_LOGI(TAG, "Registered additional GATT services (%d total)", get_additional_gatt_services_().size());
}

std::vector<const struct ble_gatt_svc_def *> &NimBLEBase::get_additional_services() {
  return get_additional_gatt_services_();
}

void NimBLEBase::register_advertising_service_uuid(const ble_uuid128_t *uuid) {
  get_advertising_service_uuids_().push_back(uuid);
  ESP_LOGI(TAG, "Registered advertising service UUID (%d total)", get_advertising_service_uuids_().size());
}

std::vector<const ble_uuid128_t *> &NimBLEBase::get_advertising_service_uuids() {
  return get_advertising_service_uuids_();
}

void NimBLEBase::register_sync_callback(SyncCallback callback) {
  get_sync_callbacks_().push_back(callback);
  ESP_LOGI(TAG, "Registered sync callback (%d total)", get_sync_callbacks_().size());
}

void NimBLEBase::setup() {
  ESP_LOGI(TAG, "NimBLEBase::setup() called on instance %p", this);
  global_nimble_base = this;

  ESP_LOGI(TAG, "Setting up NimBLE Base...");

  // Ensure NVS is initialized (required by Bluetooth stack)
  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS init returned %s, erasing NVS...", esp_err_to_name(nvs_ret));
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_ret = nvs_flash_init();
  }
  if (nvs_ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_ret));
    return;
  }

  // Release Classic BT memory (ignore error if already released)
  (void) esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  // Configure host callbacks BEFORE nimble_port_init() to prevent a race
  // condition: in ESP-IDF 5.5+, nimble_port_init() may start the NimBLE host
  // task internally, which can call sync_cb before we set it.  Setting them
  // here is safe on all IDF versions because ble_hs_init() never clears them.
  ble_hs_cfg.sync_cb = NimBLEBase::on_sync_;
  ble_hs_cfg.reset_cb = NimBLEBase::on_reset_;

  // Initialize NimBLE port (handles controller init internally)
  ESP_LOGV(TAG, "Calling nimble_port_init()...");
  esp_err_t ret = nimble_port_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
    return;
  }

  // Initialize BLE store config before starting host task
  ESP_LOGV(TAG, "Initializing BLE store config...");
  ble_store_config_init();

  // Initialize GAP/GATT services before starting host task
  ESP_LOGV(TAG, "Initializing GAP/GATT services...");
  ble_svc_gap_init();
  ble_svc_gatt_init();

  // Set device name
  int rc = ble_svc_gap_device_name_set("ESPHome NimBLE");
  if (rc != 0) {
    ESP_LOGE(TAG, "Error setting device name: %d", rc);
  }

  // Register any additional GATT services (e.g., from nimble_improv)
  auto &additional_gatt_services = get_additional_gatt_services_();
  if (!additional_gatt_services.empty()) {
    ESP_LOGI(TAG, "Registering %d additional GATT service(s)", additional_gatt_services.size());
    for (const auto *services : additional_gatt_services) {
      rc = ble_gatts_count_cfg(services);
      if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed for additional service: %d", rc);
        continue;
      }

      rc = ble_gatts_add_svcs(services);
      if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed for additional service: %d", rc);
      } else {
        ESP_LOGI(TAG, "Successfully registered additional GATT service");
      }
    }
  }

  // Start NimBLE host task (only once).
  // In ESP-IDF >= 5.5.0, nimble_port_init() launches the host task internally;
  // calling nimble_port_freertos_init() again would create a duplicate task.
  if (!this->host_task_started_) {
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
    ESP_LOGV(TAG, "Starting NimBLE host task via nimble_port_freertos_init...");
    nimble_port_freertos_init(NimBLEBase::host_task_);
#else
    ESP_LOGV(TAG, "NimBLE host task managed internally by nimble_port_init (ESP-IDF >= 5.5.0)");
#endif
    this->host_task_started_ = true;
  }

  ESP_LOGI(TAG, "NimBLE Base setup complete");
}

void NimBLEBase::on_sync_() {
  ESP_LOGI(TAG, "NimBLE host synced");

  if (global_nimble_base != nullptr) {
    global_nimble_base->initialized_ = true;
  }

  // Call all registered sync callbacks
  for (auto callback : get_sync_callbacks_()) {
    if (callback != nullptr) {
      callback();
    }
  }
}

void NimBLEBase::on_reset_(int reason) {
  ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

// FreeRTOS task trampoline for NimBLE host
void NimBLEBase::host_task_(void *param) {
  (void) param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

std::string NimBLEBase::get_device_name_with_mac(const std::string &base_name, bool short_form) {
  char device_name[32];

  // Get last 3 bytes (6 hex chars) of WiFi MAC address
  uint8_t addr[6];
  int rc_addr = esp_read_mac(addr, ESP_MAC_WIFI_STA);

  if (short_form && rc_addr == 0) {
    // Use minimal name with MAC: "h-ABC123" (8 chars max)
    // Extract first char from device name
    std::string short_name = base_name;
    size_t dash_pos = short_name.find('-');
    if (dash_pos != std::string::npos) {
      short_name = short_name.substr(0, dash_pos);  // Keep only base part
    }
    // Use first char + hyphen + MAC (1+1+6 = 8 chars)
    snprintf(device_name, sizeof(device_name), "%c-%02X%02X%02X",
             short_name[0], addr[3], addr[4], addr[5]);
  } else if (rc_addr == 0) {
    // Use full name with MAC
    snprintf(device_name, sizeof(device_name), "%s-%02X%02X%02X",
             base_name.c_str(), addr[3], addr[4], addr[5]);
  } else {
    // Fallback if MAC address not available
    snprintf(device_name, sizeof(device_name), "%s", base_name.c_str());
  }

  return std::string(device_name);
}

std::string NimBLEBase::get_mac_address_pretty() {
  // Get the public device address from NimBLE
  uint8_t addr_type;
  ble_addr_t addr;
  int rc = ble_hs_id_infer_auto(0, &addr_type);
  if (rc != 0) {
    ESP_LOGW(TAG, "Failed to determine BLE address type: %d", rc);
    return "00:00:00:00:00:00";
  }

  rc = ble_hs_id_copy_addr(addr_type, addr.val, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "Failed to copy BLE address: %d", rc);
    return "00:00:00:00:00:00";
  }

  // Format as MAC address string (reversed byte order for display)
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           addr.val[5], addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
  return std::string(mac_str);
}

void NimBLEBase::dump_config() {
  ESP_LOGCONFIG(TAG, "NimBLE Base:");
  ESP_LOGCONFIG(TAG, "  Initialized: %s", YESNO(this->initialized_));
  ESP_LOGCONFIG(TAG, "  Host task started: %s", YESNO(this->host_task_started_));
  ESP_LOGCONFIG(TAG, "  Registered GATT services: %d", get_additional_gatt_services_().size());
  ESP_LOGCONFIG(TAG, "  Registered advertising UUIDs: %d", get_advertising_service_uuids_().size());
  ESP_LOGCONFIG(TAG, "  BT controller status: %d", (int) esp_bt_controller_get_status());
}

}  // namespace nimble_base
}  // namespace esphome
