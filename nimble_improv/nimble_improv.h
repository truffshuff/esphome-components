/**
 * NimBLE Improv - WiFi Provisioning via BLE using NimBLE stack
 * 
 * This component implements the Improv WiFi provisioning protocol using
 * the NimBLE Bluetooth stack instead of Bluedroid, providing:
 * - Lower memory footprint (~100KB less RAM usage)
 * - Better compatibility with nimble_proxy component
 * - Ability to provision WiFi while device is already connected
 * 
 * Based on ESPHome's esp32_improv but adapted for NimBLE stack.
 * Protocol: https://www.improv-wifi.com/
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/wifi/wifi_component.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#ifdef USE_OUTPUT
#include "esphome/components/output/binary_output.h"
#endif

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>
#include <vector>

namespace esphome {
namespace nimble_improv {

// Improv Protocol UUIDs (standard from improv-wifi.com)
static const char *IMPROV_SERVICE_UUID = "00467768-6228-2272-4663-277478268000";
static const char *IMPROV_STATUS_UUID = "00467768-6228-2272-4663-277478268001";
static const char *IMPROV_ERROR_UUID = "00467768-6228-2272-4663-277478268002";
static const char *IMPROV_RPC_COMMAND_UUID = "00467768-6228-2272-4663-277478268003";
static const char *IMPROV_RPC_RESULT_UUID = "00467768-6228-2272-4663-277478268004";
static const char *IMPROV_CAPABILITIES_UUID = "00467768-6228-2272-4663-277478268005";

enum ImprovCommand : uint8_t {
  WIFI_SETTINGS = 0x01,
  IDENTIFY = 0x02,
  GET_CURRENT_STATE = 0x02,
  GET_DEVICE_INFO = 0x03,
  GET_WIFI_NETWORKS = 0x04,
};

enum ImprovState : uint8_t {
  AUTHORIZATION_REQUIRED = 0x01,
  AUTHORIZED = 0x02,
  PROVISIONING = 0x03,
  PROVISIONED = 0x04,
};

enum Error : uint8_t {
  NO_ERROR = 0x00,
  INVALID_RPC = 0x01,
  UNKNOWN_RPC = 0x02,
  UNABLE_TO_CONNECT = 0x03,
  NOT_AUTHORIZED = 0x04,
  UNKNOWN = 0xFF,
};

class NimBLEImprov : public Component, public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
 public:
  NimBLEImprov();
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // Configuration setters
  void set_authorizer(output::BinaryOutput *authorizer) { this->authorizer_ = authorizer; }
  void set_status_indicator(output::BinaryOutput *status_indicator) { this->status_indicator_ = status_indicator; }
  void set_authorized_duration(uint32_t duration) { this->authorized_duration_ = duration; }
  void set_identify_duration(uint32_t duration) { this->identify_duration_ = duration; }
  void set_wifi_timeout(uint32_t timeout) { this->wifi_timeout_ = timeout; }

  // NimBLE Server Callbacks
  void onConnect(NimBLEServer *server) override;
  void onDisconnect(NimBLEServer *server) override;

  // NimBLE Characteristic Callbacks
  void onWrite(NimBLECharacteristic *characteristic) override;

 protected:
  void setup_service_();
  void set_state_(ImprovState state);
  void set_error_(Error error);
  void send_response_(std::vector<uint8_t> &response);
  void process_command_(const uint8_t *data, size_t length);
  
  // Command handlers
  void handle_wifi_settings_(const std::vector<std::string> &strings);
  void handle_identify_();
  void handle_get_current_state_();
  void handle_get_device_info_();
  void handle_get_wifi_networks_();

  // Authorization
  bool check_authorization_();
  void start_authorization_();

  // WiFi helpers
  void start_wifi_connect_(const std::string &ssid, const std::string &password);
  void check_wifi_connection_();

  // NimBLE objects
  NimBLEServer *server_{nullptr};
  NimBLEService *service_{nullptr};
  NimBLECharacteristic *status_char_{nullptr};
  NimBLECharacteristic *error_char_{nullptr};
  NimBLECharacteristic *rpc_command_char_{nullptr};
  NimBLECharacteristic *rpc_result_char_{nullptr};
  NimBLECharacteristic *capabilities_char_{nullptr};

  // State
  ImprovState state_{ImprovState::AUTHORIZATION_REQUIRED};
  Error error_state_{Error::NO_ERROR};
  bool service_running_{false};
  bool client_connected_{false};

  // WiFi provisioning state
  std::string pending_ssid_;
  std::string pending_password_;
  uint32_t wifi_connect_start_{0};
  bool wifi_connecting_{false};

  // Authorization
  output::BinaryOutput *authorizer_{nullptr};
  uint32_t authorized_start_{0};
  uint32_t authorized_duration_{60000};  // 1 minute default
  bool authorized_{false};

  // Identification
  output::BinaryOutput *status_indicator_{nullptr};
  uint32_t identify_start_{0};
  uint32_t identify_duration_{10000};  // 10 seconds default
  bool identifying_{false};

  // Timeouts
  uint32_t wifi_timeout_{60000};  // 1 minute default
};

}  // namespace nimble_improv
}  // namespace esphome
