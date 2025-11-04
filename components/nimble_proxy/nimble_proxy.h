#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/defines.h"

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
#include <array>
#include <string>
#include <mutex>

#ifndef BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE
#define BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE 5
#endif

namespace esphome {
namespace nimble_proxy {

class NimBLEProxy : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_active(bool active) { this->active_ = active; }
  void set_max_connections(uint8_t max_connections) { this->max_connections_ = max_connections; }

  // API integration methods for Home Assistant connectivity
  void *get_api_connection() {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    return this->api_connection_;
  }
  void subscribe_api_connection(void *conn, uint32_t flags);
  void unsubscribe_api_connection(void *conn);
  template<typename T> void bluetooth_device_request(const T &msg) { }
  template<typename T> void bluetooth_gatt_read(const T &msg) { }
  template<typename T> void bluetooth_gatt_write(const T &msg) { }
  template<typename T> void bluetooth_gatt_read_descriptor(const T &msg) { }
  template<typename T> void bluetooth_gatt_write_descriptor(const T &msg) { }
  template<typename T> void bluetooth_gatt_send_services(const T &msg) { }
  template<typename T> void bluetooth_gatt_notify(const T &msg) { }
  void send_connections_free(void *conn) { }
  void bluetooth_scanner_set_mode(bool mode);
  uint32_t get_feature_flags();
  std::string get_bluetooth_mac_address_pretty();

 protected:
  bool active_{true};
  uint8_t max_connections_{3};
  bool initialized_{false};
  bool host_task_started_{false};
  bool scanning_{false};

  // API connection tracking (void* to avoid including api headers)
  // We only support one active API connection at a time (like native bluetooth_proxy)
  // Protected by mutex since it's accessed from NimBLE host thread and ESPHome main thread
  void *api_connection_{nullptr};
  std::mutex api_connection_mutex_;

  // Advertisement batching for Home Assistant (opaque buffer to avoid header dependencies)
  void *adv_buffer_{nullptr};
  uint16_t adv_buffer_count_{0};
  uint32_t last_send_time_{0};
  bool adv_buffer_allocated_{false};

  void start_scan_();
  void stop_scan_();
  void start_advertising_();
  void setup_services_();

  void send_advertisements_();
  void add_advertisement_(const ble_gap_disc_desc *disc);
  void send_scanner_state_();

  // NimBLE callbacks
  static int gap_event_handler_(struct ble_gap_event *event, void *arg);
  static int scan_callback_(struct ble_gap_event *event, void *arg);
  static void on_sync_();
  static void on_reset_(int reason);
  static void host_task_(void *param);
};

}  // namespace nimble_proxy
}  // namespace esphome
