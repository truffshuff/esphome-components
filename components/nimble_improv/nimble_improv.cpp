#include "nimble_improv.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace nimble_improv {

static const char *const TAG = "nimble_improv";

// Global instance pointer for NimBLE callbacks
static NimBLEImprov *global_nimble_improv = nullptr;

// Forward declarations for characteristic access
static int improv_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

// GATT service definition
static const struct ble_gatt_svc_def improv_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &IMPROV_SERVICE_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Status characteristic (read + notify)
                .uuid = &IMPROV_STATUS_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = nullptr,
            },
            {
                // Error characteristic (read)
                .uuid = &IMPROV_ERROR_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = nullptr,
            },
            {
                // RPC Command characteristic (write)
                .uuid = &IMPROV_RPC_COMMAND_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = nullptr,
            },
            {
                // RPC Result characteristic (read + notify)
                .uuid = &IMPROV_RPC_RESULT_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = nullptr,
            },
            {
                // Capabilities characteristic (read)
                .uuid = &IMPROV_CAPABILITIES_UUID.u,
                .access_cb = improv_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
                .val_handle = nullptr,
            },
            {
                0, // No more characteristics
            }
        },
    },
    {
        0, // No more services
    }
};

NimBLEImprov::NimBLEImprov() {
  global_nimble_improv = this;
}

void NimBLEImprov::setup() {
  ESP_LOGI(TAG, "Setting up NimBLE Improv WiFi Provisioning...");

  // Start the Improv service
  this->start_service_();
}

void NimBLEImprov::loop() {
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

void NimBLEImprov::start_service_() {
  ESP_LOGI(TAG, "Starting Improv BLE service");

  // Check if NimBLE is already initialized by nimble_proxy
  // If not, we can't proceed (nimble_proxy should be loaded first)
  if (!ble_hs_is_enabled()) {
    ESP_LOGE(TAG, "NimBLE host not initialized! Make sure nimble_proxy is loaded before nimble_improv");
    return;
  }

  // Register GATT services
  int rc = ble_gatts_count_cfg(improv_gatt_svcs);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
    return;
  }

  rc = ble_gatts_add_svcs(improv_gatt_svcs);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
    return;
  }

  // Get characteristic value handles for later use
  rc = ble_gatts_find_chr(&IMPROV_SERVICE_UUID.u, &IMPROV_STATUS_UUID.u,
                          nullptr, &this->status_handle_);
  if (rc == 0) {
    ESP_LOGD(TAG, "Status characteristic handle: %d", this->status_handle_);
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

  // Start advertising
  this->start_advertising_();

  this->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);
  ESP_LOGI(TAG, "Improv BLE service started successfully");
}

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

void NimBLEImprov::stop_service_() {
  ESP_LOGI(TAG, "Stopping Improv BLE service");

  // Stop advertising
  ble_gap_adv_stop();

  this->set_state_(IMPROV_STATE_STOPPED);
}

void NimBLEImprov::set_state_(ImprovState state) {
  if (this->state_ == state)
    return;

  this->state_ = state;
  ESP_LOGD(TAG, "State changed to: %d", state);

  // Notify status characteristic if we have a connection
  if (this->conn_handle_ != BLE_HS_CONN_HANDLE_NONE) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&state, sizeof(state));
    if (om != nullptr) {
      ble_gatts_notify_custom(this->conn_handle_, this->status_handle_, om);
    }
  }
}

void NimBLEImprov::set_error_(ImprovError error) {
  if (this->error_ == error)
    return;

  this->error_ = error;
  ESP_LOGD(TAG, "Error set to: %d", error);

  // Error characteristic is read-only, not notified
  // Client will read it when needed
}

void NimBLEImprov::process_command_(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    this->set_error_(ERROR_INVALID_RPC);
    return;
  }

  ImprovCommand command = static_cast<ImprovCommand>(data[0]);

  switch (command) {
    case WIFI_SETTINGS: {
      if (!this->authorized_) {
        this->set_error_(ERROR_NOT_AUTHORIZED);
        return;
      }

      // Parse SSID and password from command data
      // Format: [command][ssid_len][ssid][password_len][password]
      if (data.size() < 3) {
        this->set_error_(ERROR_INVALID_RPC);
        return;
      }

      size_t pos = 1;
      uint8_t ssid_len = data[pos++];
      if (pos + ssid_len > data.size()) {
        this->set_error_(ERROR_INVALID_RPC);
        return;
      }

      std::string ssid(data.begin() + pos, data.begin() + pos + ssid_len);
      pos += ssid_len;

      if (pos >= data.size()) {
        this->set_error_(ERROR_INVALID_RPC);
        return;
      }

      uint8_t password_len = data[pos++];
      if (pos + password_len > data.size()) {
        this->set_error_(ERROR_INVALID_RPC);
        return;
      }

      std::string password(data.begin() + pos, data.begin() + pos + password_len);

      ESP_LOGI(TAG, "Received WiFi credentials for SSID: %s", ssid.c_str());
      this->start_wifi_connect_(ssid, password);
      break;
    }

    case IDENTIFY: {
      ESP_LOGI(TAG, "Identify command received");
      if (this->status_indicator_ != nullptr) {
        this->status_indicator_->turn_on();
        // TODO: Turn off after identify_duration_
      }
      break;
    }

    case GET_DEVICE_INFO: {
      ESP_LOGI(TAG, "Get device info command received");

      // Send device info response
      // Format: [command][firmware_len][firmware][version_len][version][hardware_len][hardware][device_name_len][device_name]
      std::vector<uint8_t> response;
      response.push_back(GET_DEVICE_INFO);

      std::string firmware = "ESPHome";
      response.push_back(firmware.length());
      response.insert(response.end(), firmware.begin(), firmware.end());

      std::string version = App.get_compilation_time();
      response.push_back(version.length());
      response.insert(response.end(), version.begin(), version.end());

      std::string hardware = "ESP32-S3";
      response.push_back(hardware.length());
      response.insert(response.end(), hardware.begin(), hardware.end());

      std::string device_name = App.get_name();
      response.push_back(device_name.length());
      response.insert(response.end(), device_name.begin(), device_name.end());

      this->send_response_(response);
      break;
    }

    case GET_WIFI_NETWORKS: {
      ESP_LOGI(TAG, "Get WiFi networks command received");
      // TODO: Scan and send WiFi networks
      // This would require triggering a WiFi scan and formatting results
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown command: 0x%02X", command);
      this->set_error_(ERROR_UNKNOWN_RPC);
      break;
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

  // Check timeout
  if (millis() - this->wifi_connect_start_ > this->wifi_timeout_) {
    ESP_LOGW(TAG, "WiFi connection timeout");
    this->set_error_(ERROR_UNABLE_TO_CONNECT);
    this->set_state_(IMPROV_STATE_AUTHORIZED);
    this->wifi_connect_running_ = false;
    return;
  }

  // Check if connected
  if (wifi::global_wifi_component->is_connected()) {
    ESP_LOGI(TAG, "WiFi connected successfully!");
    this->set_state_(IMPROV_STATE_PROVISIONED);
    this->wifi_connect_running_ = false;

    // Send success response with redirect URL (IP address)
    auto ip = wifi::global_wifi_component->get_ip_address();
    std::string redirect_url = "http://" + ip.str() + "/";

    std::vector<uint8_t> response;
    response.push_back(WIFI_SETTINGS);
    response.push_back(redirect_url.length());
    response.insert(response.end(), redirect_url.begin(), redirect_url.end());

    this->send_response_(response);

    // TODO: Save credentials to flash
  }
}

void NimBLEImprov::send_response_(const std::vector<uint8_t> &data) {
  if (this->conn_handle_ == BLE_HS_CONN_HANDLE_NONE) {
    ESP_LOGW(TAG, "No active connection to send response");
    return;
  }

  // Send notification on rpc_result characteristic
  struct os_mbuf *om = ble_hs_mbuf_from_flat(data.data(), data.size());
  if (om == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate mbuf for response");
    return;
  }

  int rc = ble_gatts_notify_custom(this->conn_handle_, this->rpc_result_handle_, om);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to send notification: %d", rc);
  } else {
    ESP_LOGD(TAG, "Sent response (%d bytes)", data.size());
  }
}

// NimBLE GATT characteristic access callback
static int improv_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (global_nimble_improv == nullptr) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  const ble_uuid_t *uuid = ctxt->chr->uuid;

  // Determine which characteristic is being accessed
  if (ble_uuid_cmp(uuid, &IMPROV_STATUS_UUID.u) == 0) {
    // Status characteristic (read)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      uint8_t state = global_nimble_improv->state_;
      int rc = os_mbuf_append(ctxt->om, &state, sizeof(state));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_ERROR_UUID.u) == 0) {
    // Error characteristic (read)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      uint8_t error = global_nimble_improv->error_;
      int rc = os_mbuf_append(ctxt->om, &error, sizeof(error));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_RPC_COMMAND_UUID.u) == 0) {
    // RPC Command characteristic (write)
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
      // Extract command data from mbuf
      uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
      std::vector<uint8_t> data(om_len);
      int rc = ble_hs_mbuf_to_flat(ctxt->om, data.data(), om_len, nullptr);
      if (rc == 0) {
        global_nimble_improv->process_command_(data);
      }
      return 0;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_RPC_RESULT_UUID.u) == 0) {
    // RPC Result characteristic (read - but typically sent via notify)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      // Return empty data - results are sent via notifications
      return 0;
    }
  }
  else if (ble_uuid_cmp(uuid, &IMPROV_CAPABILITIES_UUID.u) == 0) {
    // Capabilities characteristic (read)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
      // Capability: 0x00 = WiFi provisioning only
      uint8_t capabilities = 0x00;
      int rc = os_mbuf_append(ctxt->om, &capabilities, sizeof(capabilities));
      return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
  }

  return BLE_ATT_ERR_UNLIKELY;
}

// NimBLE GAP event handler (static)
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

}  // namespace nimble_improv
}  // namespace esphome
