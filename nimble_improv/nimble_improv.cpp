/**
 * NimBLE Improv - Implementation
 * 
 * WiFi provisioning via BLE using NimBLE stack and Improv protocol
 */

#include "nimble_improv.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"

namespace esphome {
namespace nimble_improv {

static const char *const TAG = "nimble_improv";

NimBLEImprov::NimBLEImprov() {}

void NimBLEImprov::setup() {
  ESP_LOGCONFIG(TAG, "Setting up NimBLE Improv...");

  // Initialize NimBLE if not already done
  if (!NimBLEDevice::getInitialized()) {
    NimBLEDevice::init("Halo-Improv");
    ESP_LOGD(TAG, "NimBLE initialized");
  }

  // Create BLE Server
  this->server_ = NimBLEDevice::createServer();
  this->server_->setCallbacks(this);

  // Setup Improv service
  this->setup_service_();

  // Start advertising
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(IMPROV_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();

  this->service_running_ = true;
  ESP_LOGI(TAG, "NimBLE Improv service started");
}

void NimBLEImprov::setup_service_() {
  // Create Improv service
  this->service_ = this->server_->createService(IMPROV_SERVICE_UUID);

  // Status characteristic (read/notify)
  this->status_char_ = this->service_->createCharacteristic(
      IMPROV_STATUS_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  // Error characteristic (read)
  this->error_char_ = this->service_->createCharacteristic(
      IMPROV_ERROR_UUID,
      NIMBLE_PROPERTY::READ
  );

  // RPC Command characteristic (write)
  this->rpc_command_char_ = this->service_->createCharacteristic(
      IMPROV_RPC_COMMAND_UUID,
      NIMBLE_PROPERTY::WRITE
  );
  this->rpc_command_char_->setCallbacks(this);

  // RPC Result characteristic (read/notify)
  this->rpc_result_char_ = this->service_->createCharacteristic(
      IMPROV_RPC_RESULT_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  // Capabilities characteristic (read)
  this->capabilities_char_ = this->service_->createCharacteristic(
      IMPROV_CAPABILITIES_UUID,
      NIMBLE_PROPERTY::READ
  );

  // Set initial values
  uint8_t capabilities = 0x00;  // No special capabilities
  this->capabilities_char_->setValue(&capabilities, 1);

  this->set_state_(ImprovState::AUTHORIZATION_REQUIRED);
  this->set_error_(Error::NO_ERROR);

  // Start the service
  this->service_->start();
}

void NimBLEImprov::loop() {
  if (!this->service_running_) return;

  // Handle authorization timeout
  if (this->authorized_ && this->authorized_start_ > 0) {
    if (millis() - this->authorized_start_ > this->authorized_duration_) {
      ESP_LOGD(TAG, "Authorization expired");
      this->authorized_ = false;
      this->set_state_(ImprovState::AUTHORIZATION_REQUIRED);
    }
  }

  // Handle identify timeout
  if (this->identifying_ && this->identify_start_ > 0) {
    if (millis() - this->identify_start_ > this->identify_duration_) {
      ESP_LOGD(TAG, "Identify period ended");
      this->identifying_ = false;
      if (this->status_indicator_ != nullptr) {
        this->status_indicator_->turn_off();
      }
    }
  }

  // Check WiFi connection progress
  if (this->wifi_connecting_) {
    this->check_wifi_connection_();
  }
}

void NimBLEImprov::dump_config() {
  ESP_LOGCONFIG(TAG, "NimBLE Improv:");
  ESP_LOGCONFIG(TAG, "  Service UUID: %s", IMPROV_SERVICE_UUID);
  ESP_LOGCONFIG(TAG, "  Authorized Duration: %u ms", this->authorized_duration_);
  ESP_LOGCONFIG(TAG, "  Identify Duration: %u ms", this->identify_duration_);
  ESP_LOGCONFIG(TAG, "  WiFi Timeout: %u ms", this->wifi_timeout_);
}

// ============================================================================
// NimBLE Callbacks
// ============================================================================

void NimBLEImprov::onConnect(NimBLEServer *server) {
  ESP_LOGI(TAG, "Client connected");
  this->client_connected_ = true;
}

void NimBLEImprov::onDisconnect(NimBLEServer *server) {
  ESP_LOGI(TAG, "Client disconnected");
  this->client_connected_ = false;
  
  // Restart advertising
  NimBLEDevice::getAdvertising()->start();
}

void NimBLEImprov::onWrite(NimBLECharacteristic *characteristic) {
  if (characteristic->getUUID().toString() == IMPROV_RPC_COMMAND_UUID) {
    std::string value = characteristic->getValue();
    this->process_command_((const uint8_t *)value.data(), value.length());
  }
}

// ============================================================================
// Command Processing
// ============================================================================

void NimBLEImprov::process_command_(const uint8_t *data, size_t length) {
  if (length < 1) {
    this->set_error_(Error::INVALID_RPC);
    return;
  }

  ImprovCommand command = (ImprovCommand)data[0];
  
  ESP_LOGD(TAG, "Received command: 0x%02X", command);

  switch (command) {
    case WIFI_SETTINGS:
      if (!this->check_authorization_()) return;
      // Parse SSID and password from data
      // Format: [cmd][ssid_len][ssid][pass_len][pass]
      if (length < 3) {
        this->set_error_(Error::INVALID_RPC);
        return;
      }
      {
        size_t pos = 1;
        uint8_t ssid_len = data[pos++];
        if (pos + ssid_len >= length) {
          this->set_error_(Error::INVALID_RPC);
          return;
        }
        std::string ssid((char *)&data[pos], ssid_len);
        pos += ssid_len;
        
        uint8_t pass_len = data[pos++];
        if (pos + pass_len > length) {
          this->set_error_(Error::INVALID_RPC);
          return;
        }
        std::string password((char *)&data[pos], pass_len);
        
        ESP_LOGI(TAG, "WiFi credentials received: SSID=%s", ssid.c_str());
        this->start_wifi_connect_(ssid, password);
      }
      break;

    case IDENTIFY:
      this->handle_identify_();
      break;

    case GET_CURRENT_STATE:
      this->handle_get_current_state_();
      break;

    case GET_DEVICE_INFO:
      this->handle_get_device_info_();
      break;

    case GET_WIFI_NETWORKS:
      if (!this->check_authorization_()) return;
      this->handle_get_wifi_networks_();
      break;

    default:
      ESP_LOGW(TAG, "Unknown command: 0x%02X", command);
      this->set_error_(Error::UNKNOWN_RPC);
      break;
  }
}

// ============================================================================
// Command Handlers
// ============================================================================

void NimBLEImprov::handle_identify_() {
  ESP_LOGI(TAG, "Identify requested");
  
  if (this->status_indicator_ != nullptr) {
    this->status_indicator_->turn_on();
  }
  
  this->identifying_ = true;
  this->identify_start_ = millis();
}

void NimBLEImprov::handle_get_current_state_() {
  ESP_LOGD(TAG, "Get current state requested");
  // State is already in status characteristic
  this->set_state_(this->state_);
}

void NimBLEImprov::handle_get_device_info_() {
  ESP_LOGD(TAG, "Get device info requested");
  
  // Build response: [cmd][name_len][name][version_len][version][chip_len][chip]
  std::vector<uint8_t> response;
  response.push_back(GET_DEVICE_INFO);
  
  std::string name = App.get_name();
  response.push_back(name.length());
  response.insert(response.end(), name.begin(), name.end());
  
  std::string version = App.get_compilation_time();
  response.push_back(version.length());
  response.insert(response.end(), version.begin(), version.end());
  
  std::string chip = "ESP32-S3";
  response.push_back(chip.length());
  response.insert(response.end(), chip.begin(), chip.end());
  
  this->send_response_(response);
}

void NimBLEImprov::handle_get_wifi_networks_() {
  ESP_LOGD(TAG, "Get WiFi networks requested");
  
  // Trigger WiFi scan
  // Note: This would need WiFi component integration
  // For now, return empty list
  std::vector<uint8_t> response;
  response.push_back(GET_WIFI_NETWORKS);
  response.push_back(0);  // No networks
  
  this->send_response_(response);
}

// ============================================================================
// WiFi Provisioning
// ============================================================================

void NimBLEImprov::start_wifi_connect_(const std::string &ssid, const std::string &password) {
  ESP_LOGI(TAG, "Starting WiFi connection to: %s", ssid.c_str());
  
  this->pending_ssid_ = ssid;
  this->pending_password_ = password;
  this->wifi_connecting_ = true;
  this->wifi_connect_start_ = millis();
  
  this->set_state_(ImprovState::PROVISIONING);
  
  // Update WiFi credentials
  wifi::global_wifi_component->set_sta(wifi::WiFiAP(ssid, password));
  wifi::global_wifi_component->start_connecting();
}

void NimBLEImprov::check_wifi_connection_() {
  if (!this->wifi_connecting_) return;
  
  // Check timeout
  if (millis() - this->wifi_connect_start_ > this->wifi_timeout_) {
    ESP_LOGW(TAG, "WiFi connection timeout");
    this->wifi_connecting_ = false;
    this->set_error_(Error::UNABLE_TO_CONNECT);
    this->set_state_(ImprovState::AUTHORIZED);
    return;
  }
  
  // Check connection status
  if (wifi::global_wifi_component->is_connected()) {
    ESP_LOGI(TAG, "WiFi connected successfully!");
    this->wifi_connecting_ = false;
    this->set_state_(ImprovState::PROVISIONED);
    
    // Send success response with IP address
    std::vector<uint8_t> response;
    response.push_back(WIFI_SETTINGS);
    
    auto ip_str = wifi::global_wifi_component->get_ip_address().str();
    response.push_back(ip_str.length());
    response.insert(response.end(), ip_str.begin(), ip_str.end());
    
    this->send_response_(response);
  }
}

// ============================================================================
// Authorization
// ============================================================================

bool NimBLEImprov::check_authorization_() {
  if (this->state_ == ImprovState::AUTHORIZED || this->state_ == ImprovState::PROVISIONED) {
    return true;
  }
  
  if (this->authorizer_ == nullptr) {
    // No authorizer configured, auto-authorize
    this->start_authorization_();
    return true;
  }
  
  // Check if authorization button is pressed
  // This would need to be implemented with GPIO monitoring
  // For now, require explicit authorization
  this->set_error_(Error::NOT_AUTHORIZED);
  return false;
}

void NimBLEImprov::start_authorization_() {
  ESP_LOGI(TAG, "Authorization granted");
  this->authorized_ = true;
  this->authorized_start_ = millis();
  this->set_state_(ImprovState::AUTHORIZED);
}

// ============================================================================
// State Management
// ============================================================================

void NimBLEImprov::set_state_(ImprovState state) {
  if (this->state_ != state) {
    ESP_LOGD(TAG, "State changed: %d -> %d", this->state_, state);
    this->state_ = state;
    
    uint8_t state_byte = (uint8_t)state;
    this->status_char_->setValue(&state_byte, 1);
    this->status_char_->notify();
  }
}

void NimBLEImprov::set_error_(Error error) {
  if (this->error_state_ != error) {
    ESP_LOGD(TAG, "Error state changed: %d -> %d", this->error_state_, error);
    this->error_state_ = error;
    
    uint8_t error_byte = (uint8_t)error;
    this->error_char_->setValue(&error_byte, 1);
  }
}

void NimBLEImprov::send_response_(std::vector<uint8_t> &response) {
  this->rpc_result_char_->setValue(response.data(), response.size());
  this->rpc_result_char_->notify();
}

}  // namespace nimble_improv
}  // namespace esphome
