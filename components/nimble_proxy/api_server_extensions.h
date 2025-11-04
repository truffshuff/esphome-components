#pragma once

#include "esphome/components/api/api_server.h"
#include "esphome/components/api/api_connection.h"
#include "esphome/components/api/api_pb2.h"

namespace esphome {
namespace nimble_proxy {

// Helper to send bluetooth advertisements to the connected API client
inline void send_bluetooth_advertisements_to_client(
    void *api_connection,
    api::BluetoothLERawAdvertisementsResponse &resp) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send %d advertisements to", resp.advertisements_len);
    return;
  }

  ESP_LOGD("nimble_proxy", "Sending %d BLE advertisements to API client", resp.advertisements_len);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Send the message using the API connection's send_message method
  if (!conn->send_message(resp, api::BluetoothLERawAdvertisementsResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send BLE advertisements to API connection %p", api_connection);
  }
}

// Helper to send scanner state to the connected API client
inline void send_scanner_state_to_client(
    void *api_connection,
    api::BluetoothScannerStateResponse &resp) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send scanner state to");
    return;
  }

  ESP_LOGD("nimble_proxy", "Sending scanner state (state=%d, mode=%d) to API client",
           resp.state, resp.mode);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Send the message using the API connection's send_message method
  if (!conn->send_message(resp, api::BluetoothScannerStateResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send scanner state to API connection %p", api_connection);
  }
}

}  // namespace nimble_proxy
}  // namespace esphome
