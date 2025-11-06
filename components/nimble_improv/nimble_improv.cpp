#include "nimble_improv.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"
#include "esphome/components/nimble_proxy/nimble_proxy.h"
#include <algorithm>
#include <cstring>

// Unique key for NVS storage (any non-zero 32-bit value)
static constexpr uint32_t PREF_ID_WIFI = 0x9B5A7C21;

namespace esphome {
namespace nimble_improv {

static const char *const TAG = "nimble_improv";

// Global instance pointer for NimBLE callbacks
static NimBLEImprov *global_nimble_improv = nullptr;

// If ERROR_INVALID_SSID isn't defined in your error enum, map it to INVALID_RPC
#ifndef ERROR_INVALID_SSID
#define ERROR_INVALID_SSID ERROR_INVALID_RPC
#endif

// Small helpers
static bool read_lp_str_(const std::vector<uint8_t> &in, size_t &idx, std::string &out) {
  if (idx >= in.size()) return false;
  uint8_t len = in[idx++];
  if (idx + len > in.size()) return false;
  out.assign(reinterpret_cast<const char *>(&in[idx]), len);
  idx += len;
  return true;
}

static std::string to_hex_(const std::vector<uint8_t> &in) {
  static const char *hex = "0123456789abcdef";
  std::string s; s.reserve(in.size() * 2);
  for (uint8_t b : in) { s.push_back(hex[b >> 4]); s.push_back(hex[b & 0xF]); }
  return s;
}

// Forward declarations for characteristic access (fix types)
int improv_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg);
int device_info_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);

// Standard BLE UUIDs for Device Information Service
static const ble_uuid16_t DEVICE_INFO_SERVICE_UUID = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t MODEL_NUMBER_UUID = BLE_UUID16_INIT(0x2A24);
static const ble_uuid16_t FIRMWARE_REVISION_UUID = BLE_UUID16_INIT(0x2A26);
static const ble_uuid16_t MANUFACTURER_NAME_UUID = BLE_UUID16_INIT(0x2A29);

// GATT service definitions (Device Info + Improv) with correct flags
static const struct ble_gatt_svc_def improv_gatt_svcs[] = {
    {
        // Device Information Service
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &DEVICE_INFO_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &MODEL_NUMBER_UUID.u,
                .access_cb = device_info_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &FIRMWARE_REVISION_UUID.u,
                .access_cb = device_info_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &MANUFACTURER_NAME_UUID.u,
                .access_cb = device_info_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 } // end of DIS chars
        },
    },
    {
        // Improv WiFi Service
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &IMPROV_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // 8001 Status (read + notify)
                .uuid = &IMPROV_STATUS_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // 8002 Error (read + notify) — add NOTIFY so CCCD exists
                .uuid = &IMPROV_ERROR_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // 8003 RPC Command (write / write without response)
                .uuid = &IMPROV_RPC_COMMAND_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // 8004 RPC Result (read + notify)
                .uuid = &IMPROV_RPC_RESULT_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // 8005 Capabilities (read)
                .uuid = &IMPROV_CAPABILITIES_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 } // end of Improv chars
        },
    },
    { 0 } // end of services
};

NimBLEImprov::NimBLEImprov() {
  global_nimble_improv = this;

  // Register Improv GATT services with nimble_proxy
  // This happens at construction time, before nimble_proxy's setup()
  ESP_LOGI(TAG, "Registering Improv GATT services with nimble_proxy");
  nimble_proxy::NimBLEProxy::register_gatt_services(improv_gatt_svcs);

  // Register Improv service UUID for advertising so clients can discover it
  ESP_LOGI(TAG, "Registering Improv service UUID for advertising");
  nimble_proxy::NimBLEProxy::register_advertising_service_uuid(&IMPROV_SERVICE_UUID);
}

void NimBLEImprov::setup() {
  // Prepare preference handle
  this->pref_creds_ = global_preferences->make_preference<StoredWiFi>(PREF_ID_WIFI);

  // Always try to load saved credentials from NVS if they exist
  // NVS credentials (from Improv provisioning) should override YAML credentials
  // This runs before WiFi initializes (priority 150 vs 200) so we can set the STA
  this->load_saved_credentials_();

  ESP_LOGI(TAG, "Setting up NimBLE Improv WiFi Provisioning...");
  ESP_LOGI(TAG, "Services registered, waiting for NimBLE sync");
  this->set_state_(IMPROV_STATE_STOPPED);
}

float NimBLEImprov::get_setup_priority() const {
  // Run before WiFi initializes (WiFi is ~200)
  return 150.0f;
}

// Persist credentials to NVS
void NimBLEImprov::save_credentials_(const std::string &ssid, const std::string &password) {
  StoredWiFi rec{};
  rec.version = 1;
  std::strncpy(rec.ssid, ssid.c_str(), sizeof(rec.ssid) - 1);
  std::strncpy(rec.password, password.c_str(), sizeof(rec.password) - 1);
  bool ok = this->pref_creds_.save(&rec);
  ESP_LOGI(TAG, "Saved WiFi credentials to NVS: %s", ok ? "OK" : "FAIL");

  // Replace STA with the newly provisioned network
  wifi::WiFiAP ap;
  ap.set_ssid(rec.ssid);
  ap.set_password(rec.password);
  wifi::global_wifi_component->set_sta(ap);
}

// Load credentials from NVS and switch to them
void NimBLEImprov::load_saved_credentials_() {
  StoredWiFi rec{};
  if (!this->pref_creds_.load(&rec)) {
    ESP_LOGD(TAG, "No saved WiFi credentials found in NVS");
    return;
  }
  if (rec.version != 1 || rec.ssid[0] == '\0') {
    ESP_LOGW(TAG, "Saved WiFi credentials invalid");
    return;
  }

  // Overwrite STA before WiFi starts so YAML entries don't take precedence
  wifi::WiFiAP ap;
  ap.set_ssid(rec.ssid);
  ap.set_password(rec.password);
  wifi::global_wifi_component->set_sta(ap);

  ESP_LOGI(TAG, "Loaded saved WiFi credentials for SSID '%s' from NVS", rec.ssid);
}

void NimBLEImprov::loop() {
  // Get characteristic handles and start advertising once NimBLE is synced (one-time check)
  if (!this->service_started_ && ble_hs_synced()) {
    ESP_LOGI(TAG, "NimBLE host synced, setting up Improv service");

    // Get characteristic value handles (services were registered by nimble_proxy during setup)
    int rc = ble_gatts_find_chr(&IMPROV_SERVICE_UUID.u, &IMPROV_STATUS_UUID.u,
                            nullptr, &this->status_handle_);
    if (rc == 0) {
      ESP_LOGD(TAG, "Status characteristic handle: %d", this->status_handle_);
    } else {
      ESP_LOGE(TAG, "Failed to find Status characteristic: %d", rc);
    }

    rc = ble_gatts_find_chr(&IMPROV_SERVICE_UUID.u, &IMPROV_ERROR_UUID.u,
                            nullptr, &this->error_handle_);
    if (rc == 0) {
      ESP_LOGD(TAG, "Error characteristic handle: %d", this->error_handle_);
    }

    rc = ble_gatts_find_chr(&IMPROV_SERVICE_UUID.u, &IMPROV_RPC_RESULT_UUID.u,
                            nullptr, &this->rpc_result_handle_);
    if (rc == 0) {
      ESP_LOGD(TAG, "RPC Result characteristic handle: %d", this->rpc_result_handle_);
    }

    // Don't advertise separately - nimble_proxy already handles advertising
    // The Improv service is available via the existing advertising
    ESP_LOGI(TAG, "Improv service ready (using nimble_proxy advertising)");
    this->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);
    this->service_started_ = true;
  }

  // Check if connection was lost
  if (this->service_started_ && this->conn_handle_ != BLE_HS_CONN_HANDLE_NONE) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(this->conn_handle_, &desc) != 0) {
      // Connection no longer exists
      ESP_LOGI(TAG, "BLE connection lost: handle=%d", this->conn_handle_);
      this->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
      this->authorized_ = false;
      this->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);
    }
  }

  // Check authorization timeout
  if (this->authorized_ && this->authorized_start_ > 0) {
    if (millis() - this->authorized_start_ > this->authorized_duration_) {
      ESP_LOGI(TAG, "Authorization expired");
      this->authorized_ = false;
      this->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);
    }
  }

  // Check WiFi connection progress
  if (this->wifi_connect_running_) {
    this->check_wifi_connection_();
  }
}

void NimBLEImprov::dump_config() {
  ESP_LOGCONFIG(TAG, "NimBLE Improv:");
  ESP_LOGCONFIG(TAG, "  Authorized Duration: %u ms", this->authorized_duration_);
  ESP_LOGCONFIG(TAG, "  WiFi Timeout: %u ms", this->wifi_timeout_);
}

// Advertising not needed - nimble_proxy handles advertising
// The Improv service is available via nimble_proxy's existing advertising
/*
void NimBLEImprov::start_advertising_() {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields fields;

  // Configure advertising parameters
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  // Undirected connectable
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // General discoverable

  // Set advertising data
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  // Add Improv service UUID to advertisement
  fields.uuids128 = (ble_uuid128_t[]){IMPROV_SERVICE_UUID};
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;

  // Set device name
  const char *device_name = "Halo Improv";
  fields.name = (uint8_t *)device_name;
  fields.name_len = strlen(device_name);
  fields.name_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
    return;
  }

  // Start advertising
  rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER,
                         &adv_params, NimBLEImprov::gap_event_handler, this);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    return;
  }

  ESP_LOGI(TAG, "BLE advertising started");
}
*/

void NimBLEImprov::stop_service_() {
  ESP_LOGI(TAG, "Stopping Improv BLE service");

  // Stop advertising
  ble_gap_adv_stop();

  this->set_state_(IMPROV_STATE_STOPPED);
}

// Also notify Status when it changes
void NimBLEImprov::set_state_(ImprovState state) {
  if (this->state_ == state) return;
  this->state_ = state;
  ESP_LOGD(TAG, "State changed to: %d", state);
  if (this->conn_handle_ != BLE_HS_CONN_HANDLE_NONE && this->status_handle_ != 0) {
    uint8_t val = static_cast<uint8_t>(this->state_);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&val, sizeof(val));
    if (om) {
      int rc = ble_gatts_notify_custom(this->conn_handle_, this->status_handle_, om);
      if (rc != 0) ESP_LOGW(TAG, "Failed to notify status: %d", rc);
    }
  }
}

void NimBLEImprov::set_error_(ImprovError error) {
  if (this->error_ == error)
    return;

  this->error_ = error;
  ESP_LOGD(TAG, "Error set to: %d", error);

  // Notify error characteristic if we have a connection and handle
  if (this->conn_handle_ != BLE_HS_CONN_HANDLE_NONE && this->error_handle_ != 0) {
    uint8_t val = static_cast<uint8_t>(this->error_);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&val, sizeof(val));
    if (om != nullptr) {
      int rc = ble_gatts_notify_custom(this->conn_handle_, this->error_handle_, om);
      if (rc != 0) {
        ESP_LOGW(TAG, "Failed to notify error: %d", rc);
      }
    }
  }
}

void NimBLEImprov::process_command_(const std::vector<uint8_t> &data) {
  ESP_LOGD(TAG, "process_command_: raw len=%u hex=%s", (unsigned) data.size(), to_hex_(data).c_str());

  if (data.empty()) {
    this->set_error_(ERROR_INVALID_RPC);
    return;
  }

  uint8_t cmd = data[0];
  size_t idx = 1;

  // Clear previous error on new command
  this->set_error_(ERROR_NONE);

  switch (cmd) {
    case WIFI_SETTINGS: {
      // Try BLE framing with [len][payload]
      size_t pstart = idx;
      size_t pend = data.size();
      if (data.size() >= 2 && (size_t)(2 + data[1]) <= data.size()) {
        uint8_t total_len = data[1];
        pstart = 2;
        pend = 2 + total_len;
      }

      bool parsed = false;
      std::string ssid, password;

      // Attempt LP/LP inside payload
      {
        size_t p = pstart;
        if (p < pend) {
          // local slice for LP parsing
          std::vector<uint8_t> slice(data.begin() + pstart, data.begin() + pend);
          size_t si = 0;
          if (read_lp_str_(slice, si, ssid) && read_lp_str_(slice, si, password) && si == slice.size()) {
            parsed = true;
          }
        }
      }

      // Fallback: null-delimited SSID\0PASSWORD within payload
      if (!parsed) {
        if (pstart <= pend) {
          auto beg = data.begin() + pstart;
          auto end = data.begin() + pend;
          auto it0 = std::find(beg, end, (uint8_t)0x00);
          if (it0 != end) {
            ssid.assign(reinterpret_cast<const char *>(&*beg), std::distance(beg, it0));
            auto pwd_beg = it0 + 1;
            password.assign(reinterpret_cast<const char *>(&*pwd_beg), std::distance(pwd_beg, end));
            parsed = true;
          }
        }
      }

      if (!parsed) {
        ESP_LOGW(TAG, "WIFI_SETTINGS: failed to parse payload (pstart=%u pend=%u)", (unsigned) pstart, (unsigned) pend);
        this->set_error_(ERROR_INVALID_RPC);
        return;
      }

      ESP_LOGI(TAG, "WIFI_SETTINGS: ssid_len=%u pwd_len=%u", (unsigned) ssid.size(), (unsigned) password.size());

      if (ssid.empty()) {
        this->set_error_(ERROR_INVALID_SSID);
        return;
      }

      this->set_state_(IMPROV_STATE_PROVISIONING);
      this->start_wifi_connect_(ssid, password);
      return;
    }

    case IDENTIFY: {
      // Acknowledge Identify with empty result
      this->send_response_({});
      return;
    }

    case GET_DEVICE_INFO: {
      // 4 LP strings: name, firmware, hardware, address/url (empty until WiFi connects)
      std::vector<uint8_t> payload;
      auto push_lp = [&](const std::string &s) {
        uint8_t n = static_cast<uint8_t>(std::min<size_t>(255, s.size()));
        payload.push_back(n);
        payload.insert(payload.end(), s.begin(), s.begin() + n);
      };
      push_lp(App.get_name());
      push_lp(App.get_compilation_time()); // or your version string
      push_lp("ESP32-S3");
      push_lp(""); // URL sent after WiFi connects
      this->send_response_(payload);
      return;
    }

    case GET_WIFI_NETWORKS: {
      // Not implemented: return empty list so client allows manual entry
      this->send_response_({});
      return;
    }

    default:
      this->set_error_(ERROR_INVALID_RPC);
      return;
  }
}

void NimBLEImprov::start_wifi_connect_(const std::string &ssid, const std::string &password) {
  this->incoming_ssid_ = ssid;
  this->incoming_password_ = password;
  this->wifi_connect_running_ = true;
  this->wifi_connect_start_ = millis();
  this->set_state_(IMPROV_STATE_PROVISIONING);

  ESP_LOGI(TAG, "Attempting to connect to WiFi: %s", ssid.c_str());

  // Create WiFiAP struct properly
  wifi::WiFiAP sta_ap;
  sta_ap.set_ssid(ssid);
  sta_ap.set_password(password);

  // Use the correct WiFi API - start_connecting expects WiFiAP and bool
  wifi::global_wifi_component->start_connecting(sta_ap, false);
}

void NimBLEImprov::check_wifi_connection_() {
  if (!this->wifi_connect_running_)
    return;

  if (millis() - this->wifi_connect_start_ > this->wifi_timeout_) {
    ESP_LOGW(TAG, "WiFi connection timeout");
    this->set_error_(ERROR_UNABLE_TO_CONNECT);
    this->set_state_(IMPROV_STATE_AUTHORIZED);
    this->wifi_connect_running_ = false;
    return;
  }

  if (wifi::global_wifi_component->is_connected()) {
    ESP_LOGI(TAG, "WiFi connected successfully!");
    this->set_state_(IMPROV_STATE_PROVISIONED);
    this->wifi_connect_running_ = false;

    // Persist and set as the only network so it survives reboot
    this->save_credentials_(this->incoming_ssid_, this->incoming_password_);

    // Send success response with redirect URL
    auto ip_addresses = wifi::global_wifi_component->get_ip_addresses();
    std::string redirect_url = "http://";
    redirect_url += !ip_addresses.empty() ? ip_addresses[0].str() : "192.168.1.1";
    redirect_url += "/";

    std::vector<uint8_t> response;
    uint8_t n = static_cast<uint8_t>(std::min<size_t>(255, redirect_url.size()));
    response.push_back(n);
    response.insert(response.end(), redirect_url.begin(), redirect_url.begin() + n);
    this->send_response_(response);

    this->set_error_(ERROR_NONE);
  }
}

// NimBLE GATT characteristic access callback (non-static, declared as friend)
int improv_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (global_nimble_improv == nullptr) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  // Track connection handle on first access
  if (global_nimble_improv->conn_handle_ == BLE_HS_CONN_HANDLE_NONE && conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    global_nimble_improv->conn_handle_ = conn_handle;
    ESP_LOGI(TAG, "BLE connection detected via characteristic access: handle=%d", conn_handle);

    // Grant authorization if authorizer is not used
    if (global_nimble_improv->authorizer_ == nullptr) {
      global_nimble_improv->authorized_ = true;
      global_nimble_improv->authorized_start_ = millis();

      // Clear any previous error before entering AUTHORIZED
      global_nimble_improv->set_error_(ERROR_NONE);

      // Move to AUTHORIZED (this should notify status if your set_state_ does)
      global_nimble_improv->set_state_(IMPROV_STATE_AUTHORIZED);
      ESP_LOGI(TAG, "Auto-authorized (no authorizer configured)");
    }
  }

  // Log all access attempts for debugging
  ESP_LOGD(TAG, "GATT access: conn=%d attr=%d op=%d", conn_handle, attr_handle, ctxt->op);

  const ble_uuid_t *uuid = ctxt->chr->uuid;

  // Handle CCCD (notification enable/disable) for all characteristics
  // This is needed before characteristic-specific handling
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC || ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) {
    // CCCD operations - allow client to enable/disable notifications
    // NimBLE handles the actual CCCD storage, we just need to not return an error
    ESP_LOGI(TAG, "CCCD %s operation", ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC ? "read" : "write");
    return 0;
  }

  // Determine which characteristic is being accessed
  if (ble_uuid_cmp(uuid, &IMPROV_STATUS_UUID.u) == 0) {
    // Status characteristic (read + notify)
    ESP_LOGI(TAG, "Status characteristic access, op=%d", ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      uint8_t state = static_cast<uint8_t>(global_nimble_improv->state_);
      ESP_LOGI(TAG, "Returning status: %d", state);
      int rc = os_mbuf_append(ctxt->om, &state, sizeof(state));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_ERROR_UUID.u) == 0) {
    // Error characteristic (read)
    ESP_LOGI(TAG, "Error characteristic access, op=%d", ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      uint8_t error = static_cast<uint8_t>(global_nimble_improv->error_);
      ESP_LOGI(TAG, "Returning error: %d", error);
      int rc = os_mbuf_append(ctxt->om, &error, sizeof(error));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_RPC_COMMAND_UUID.u) == 0) {
    // RPC Command characteristic (write)
    ESP_LOGI(TAG, "RPC Command characteristic access, op=%d", ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      // Extract command data from mbuf
      uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
      std::vector<uint8_t> data(om_len);
      int rc = ble_hs_mbuf_to_flat(ctxt->om, data.data(), om_len, nullptr);
      if (rc == 0) {
        ESP_LOGI(TAG, "Processing RPC command, length=%d", om_len);
        global_nimble_improv->process_command_(data);
      }
      return 0;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_RPC_RESULT_UUID.u) == 0) {
    // RPC Result characteristic (read - but typically sent via notify)
    ESP_LOGI(TAG, "RPC Result characteristic access, op=%d", ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      // Return empty data - results are sent via notifications
      return 0;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_CAPABILITIES_UUID.u) == 0) {
    // Capabilities characteristic (read)
    ESP_LOGI(TAG, "Capabilities characteristic access, op=%d", ctxt->op);
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      // Capability bitmask:
      // Bit 0 (0x01) = Identify capability
      // Setting bit 0 to indicate we support the Identify command
      uint8_t capabilities = 0x01;  // Support Identify
      ESP_LOGI(TAG, "Returning capabilities: 0x%02x", capabilities);
      int rc = os_mbuf_append(ctxt->om, &capabilities, sizeof(capabilities));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }

  // Log unhandled operations for debugging
  char uuid_str[BLE_UUID_STR_LEN];
  ble_uuid_to_str(uuid, uuid_str);
  ESP_LOGW(TAG, "Unhandled GATT operation: op=%d for UUID=%s", ctxt->op, uuid_str);
  return BLE_ATT_ERR_UNLIKELY;
}

// NimBLE GAP event handler (static)
// Gap event handler not needed - nimble_proxy handles advertising
// Connection tracking happens via characteristic access callbacks and loop() polling
/*
int NimBLEImprov::gap_event_handler(struct ble_gap_event *event, void *arg) {
  NimBLEImprov *instance = static_cast<NimBLEImprov*>(arg);
  if (instance == nullptr) {
    instance = global_nimble_improv;
  }
  if (instance == nullptr) {
    return 0;
  }

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      ESP_LOGI(TAG, "BLE GAP EVENT: Connect; status=%d conn_handle=%d",
               event->connect.status, event->connect.conn_handle);

      if (event->connect.status == 0) {
        // Connection established
        instance->conn_handle_ = event->connect.conn_handle;

        // Grant authorization if authorizer is not used
        if (instance->authorizer_ == nullptr) {
          instance->authorized_ = true;
          instance->authorized_start_ = millis();
          instance->set_state_(IMPROV_STATE_AUTHORIZED);
          ESP_LOGI(TAG, "Auto-authorized (no authorizer configured)");
        } else {
          // Wait for external authorization
          instance->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);
        }
      } else {
        // Connection failed, restart advertising
        instance->start_advertising_();
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(TAG, "BLE GAP EVENT: Disconnect; reason=%d conn_handle=%d",
               event->disconnect.reason, event->disconnect.conn.conn_handle);

      instance->conn_handle_ = BLE_HS_CONN_HANDLE_NONE;
      instance->authorized_ = false;
      instance->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);

      // Restart advertising
      instance->start_advertising_();
      break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      ESP_LOGD(TAG, "BLE GAP EVENT: Advertising complete");
      // Restart advertising if stopped
      instance->start_advertising_();
      break;

    case BLE_GAP_EVENT_SUBSCRIBE:
      ESP_LOGD(TAG, "BLE GAP EVENT: Subscribe; conn_handle=%d attr_handle=%d",
               event->subscribe.conn_handle, event->subscribe.attr_handle);
      break;

    default:
      break;
  }

  return 0;
}
*/

// Device Information Service characteristic access callback
int device_info_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
  const ble_uuid_t *uuid = ctxt->chr->uuid;

  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    const char *value = nullptr;

    if (ble_uuid_cmp(uuid, &MODEL_NUMBER_UUID.u) == 0) {
      value = App.get_name().c_str();
    } else if (ble_uuid_cmp(uuid, &FIRMWARE_REVISION_UUID.u) == 0) {
      value = App.get_compilation_time().c_str();
    } else if (ble_uuid_cmp(uuid, &MANUFACTURER_NAME_UUID.u) == 0) {
      value = "ESPHome";
    }

    if (value != nullptr) {
      int rc = os_mbuf_append(ctxt->om, value, strlen(value));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }

  return BLE_ATT_ERR_UNLIKELY;
}

void NimBLEImprov::send_response_(const std::vector<uint8_t> &payload) {
  // Ensure we have a connection and a valid value handle for RPC Result
  if (this->conn_handle_ == BLE_HS_CONN_HANDLE_NONE || this->rpc_result_handle_ == 0) {
    ESP_LOGW(TAG, "send_response_: no active connection or RPC Result handle missing");
    return;
  }

  // Build mbuf from payload (zero-length is allowed for Identify ack, etc.)
  const void *data_ptr = payload.empty() ? nullptr : payload.data();
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data_ptr, payload.size());
  if (om == nullptr) {
    ESP_LOGW(TAG, "send_response_: failed to allocate mbuf (len=%u)", (unsigned) payload.size());
    return;
  }

  int rc = ble_gatts_notify_custom(this->conn_handle_, this->rpc_result_handle_, om);
  if (rc != 0) {
    ESP_LOGW(TAG, "send_response_: notify failed rc=%d (conn=%d handle=%u len=%u)",
             rc, this->conn_handle_, (unsigned) this->rpc_result_handle_, (unsigned) payload.size());
  } else {
    ESP_LOGD(TAG, "send_response_: notified %u bytes on RPC Result", (unsigned) payload.size());
  }
}

}  // namespace nimble_improv
}  // namespace esphome
