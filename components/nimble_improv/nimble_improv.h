/**
 * NimBLE Improv - WiFi Provisioning via BLE using ESP-IDF NimBLE stack
 * 
 * This component implements the Improv WiFi provisioning protocol using
 * ESP-IDF's native NimBLE Bluetooth stack (not NimBLE-Arduino), providing:
 * - Lower memory footprint
 * - Better compatibility with nimble_proxy component
 * - Ability to provision WiFi while device is already connected
 * 
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

namespace esphome {
namespace nimble_improv {

// Improv Protocol UUIDs (standard from improv-wifi.com)
// Service: 00467768-6228-2272-4663-277478268000
static const ble_uuid128_t IMPROV_SERVICE_UUID = 
  BLE_UUID128_INIT(0x00, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00);

// Characteristics UUIDs
static const ble_uuid128_t IMPROV_STATUS_UUID = 
  BLE_UUID128_INIT(0x01, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00);

static const ble_uuid128_t IMPROV_ERROR_UUID = 
  BLE_UUID128_INIT(0x02, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00);

static const ble_uuid128_t IMPROV_RPC_COMMAND_UUID = 
  BLE_UUID128_INIT(0x03, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00);

static const ble_uuid128_t IMPROV_RPC_RESULT_UUID = 
  BLE_UUID128_INIT(0x04, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00);

static const ble_uuid128_t IMPROV_CAPABILITIES_UUID = 
  BLE_UUID128_INIT(0x05, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00);

enum ImprovCommand : uint8_t {
  WIFI_SETTINGS = 0x01,
  IDENTIFY = 0x02,
  GET_CURRENT_STATE = 0x02,
  GET_DEVICE_INFO = 0x03,
  GET_WIFI_NETWORKS = 0x04,
};

enum ImprovState : uint8_t {
  IMPROV_STATE_STOPPED = 0x00,
  IMPROV_STATE_AWAITING_AUTHORIZATION = 0x01,
  IMPROV_STATE_AUTHORIZED = 0x02,
  IMPROV_STATE_PROVISIONING = 0x03,
  IMPROV_STATE_PROVISIONED = 0x04,
};

enum ImprovError : uint8_t {
  ERROR_NONE = 0x00,
  ERROR_INVALID_RPC = 0x01,
  ERROR_UNKNOWN_RPC = 0x02,
  ERROR_UNABLE_TO_CONNECT = 0x03,
  ERROR_NOT_AUTHORIZED = 0x04,
  ERROR_UNKNOWN = 0xFF,
};

class NimBLEImprov : public Component {
 public:
  NimBLEImprov();
  
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Configuration methods
  void set_authorizer(output::BinaryOutput *authorizer) { this->authorizer_ = authorizer; }
  void set_authorized_duration(uint32_t duration) { this->authorized_duration_ = duration; }
  void set_status_indicator(output::BinaryOutput *status_indicator) { this->status_indicator_ = status_indicator; }
  void set_identify_duration(uint32_t duration) { this->identify_duration_ = duration; }
  void set_wifi_timeout(uint32_t timeout) { this->wifi_timeout_ = timeout; }

  // NimBLE callback handlers
  static int characteristic_access_callback(uint16_t conn_handle, uint16_t attr_handle,
                                           struct ble_gatt_access_ctxt *ctxt, void *arg);
  static int gap_event_handler(struct ble_gap_event *event, void *arg);

 protected:
  void start_service_();
  void start_advertising_();
  void stop_service_();
  void set_state_(ImprovState state);
  void set_error_(ImprovError error);
  void process_command_(const std::vector<uint8_t> &data);
  void start_wifi_connect_(const std::string &ssid, const std::string &password);
  void check_wifi_connection_();
  void send_response_(const std::vector<uint8_t> &data);

  // State management
  ImprovState state_{IMPROV_STATE_STOPPED};
  ImprovError error_{ERROR_NONE};
  bool authorized_{false};
  uint32_t authorized_start_{0};
  uint32_t wifi_connect_start_{0};
  
  // Configuration
  output::BinaryOutput *authorizer_{nullptr};
  uint32_t authorized_duration_{60000};  // 1 minute default
  output::BinaryOutput *status_indicator_{nullptr};
  uint32_t identify_duration_{10000};  // 10 seconds default
  uint32_t wifi_timeout_{60000};  // 1 minute default
  
  // BLE handles
  uint16_t conn_handle_{BLE_HS_CONN_HANDLE_NONE};
  uint16_t status_handle_;
  uint16_t error_handle_;
  uint16_t rpc_command_handle_;
  uint16_t rpc_result_handle_;
  uint16_t capabilities_handle_;
  
  // WiFi provisioning state
  std::string incoming_ssid_;
  std::string incoming_password_;
  bool wifi_connect_running_{false};
};

}  // namespace nimble_improv
}  // namespace esphome
