#include "nimble_improv.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace nimble_improv {

static const char *const TAG = "nimble_improv";

// Global instance pointer for NimBLE callbacks
static NimBLEImprov *global_nimble_improv = nullptr;

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
  ESP_LOGI(TAG, "Starting Improv BLE service (implementation pending)");
  // TODO: Implement NimBLE GATT service registration
  // This requires:
  // 1. Initialize NimBLE if not already done
  // 2. Register GATT service with Improv UUIDs
  // 3. Register characteristics (status, error, rpc_command, rpc_result, capabilities)
  // 4. Start advertising with Improv service UUID
  
  this->set_state_(IMPROV_STATE_AWAITING_AUTHORIZATION);
}

void NimBLEImprov::stop_service_() {
  ESP_LOGI(TAG, "Stopping Improv BLE service");
  this->set_state_(IMPROV_STATE_STOPPED);
}

void NimBLEImprov::set_state_(ImprovState state) {
  if (this->state_ == state)
    return;
    
  this->state_ = state;
  ESP_LOGD(TAG, "State changed to: %d", state);
  
  // TODO: Notify state characteristic
}

void NimBLEImprov::set_error_(ImprovError error) {
  if (this->error_ == error)
    return;
    
  this->error_ = error;
  ESP_LOGD(TAG, "Error set to: %d", error);
  
  // TODO: Notify error characteristic
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
      // TODO: Send device info response
      break;
    }
    
    case GET_WIFI_NETWORKS: {
      ESP_LOGI(TAG, "Get WiFi networks command received");
      // TODO: Scan and send WiFi networks
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
  
  // Get the global WiFi component and configure new credentials
  wifi::global_wifi_component->set_sta(wifi::WiFiAP(ssid, password));
  wifi::global_wifi_component->start_connecting();
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
    
    // TODO: Send success response with IP address
    // TODO: Save credentials to flash
  }
}

void NimBLEImprov::send_response_(const std::vector<uint8_t> &data) {
  // TODO: Send notification on rpc_result characteristic
  ESP_LOGD(TAG, "Sending response (%d bytes)", data.size());
}

// NimBLE callback handlers (static)
int NimBLEImprov::characteristic_access_callback(uint16_t conn_handle, uint16_t attr_handle,
                                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
  // TODO: Implement characteristic read/write handling
  return 0;
}

void NimBLEImprov::gap_event_handler(struct ble_gap_event *event, void *arg) {
  // TODO: Implement GAP event handling (connections, disconnections)
}

}  // namespace nimble_improv
}  // namespace esphome
