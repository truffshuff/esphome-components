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

// Helper to send connection response to the connected API client
inline void send_connection_response(
    void *api_connection,
    uint64_t address,
    bool connected,
    uint16_t mtu,
    int error) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send connection response to");
    return;
  }

  ESP_LOGD("nimble_proxy", "Sending connection response (address=%012llX, connected=%d, mtu=%d, error=%d) to API client",
           address, connected, mtu, error);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Build and send the response
  api::BluetoothDeviceConnectionResponse resp;
  resp.address = address;
  resp.connected = connected;
  resp.mtu = mtu;
  resp.error = error;

  if (!conn->send_message(resp, api::BluetoothDeviceConnectionResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send connection response to API connection %p", api_connection);
  }
}

// Helper to send GATT read response to the connected API client
inline void send_gatt_read_response(
    void *api_connection,
    uint64_t address,
    uint16_t handle,
    const uint8_t *data,
    size_t len) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send GATT read response to");
    return;
  }

  ESP_LOGD("nimble_proxy", "Sending GATT read response (address=%012llX, handle=%d, len=%d) to API client",
           address, handle, len);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Build and send the response
  api::BluetoothGATTReadResponse resp;
  resp.address = address;
  resp.handle = handle;

  // Copy data to the response using the pointer and length fields
  if (data != nullptr && len > 0) {
    resp.data_ptr_ = data;
    resp.data_len_ = len;
  } else {
    resp.data_ptr_ = nullptr;
    resp.data_len_ = 0;
  }

  if (!conn->send_message(resp, api::BluetoothGATTReadResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send GATT read response to API connection %p", api_connection);
  }
}

// Helper to send GATT error response to the connected API client
inline void send_gatt_error(
    void *api_connection,
    uint64_t address,
    uint16_t handle,
    int error) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send GATT error to");
    return;
  }

  ESP_LOGD("nimble_proxy", "Sending GATT error (address=%012llX, handle=%d, error=%d) to API client",
           address, handle, error);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Build and send the error response
  api::BluetoothGATTErrorResponse resp;
  resp.address = address;
  resp.handle = handle;
  resp.error = error;

  if (!conn->send_message(resp, api::BluetoothGATTErrorResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send GATT error to API connection %p", api_connection);
  }
}

// Helper to fill UUID fields in GATT messages
inline void fill_gatt_uuid(std::array<uint64_t, 2> &uuid_field, uint32_t &short_uuid, const std::array<uint64_t, 2> &uuid) {
  // Check if this is a short UUID (16 or 32-bit)
  if (uuid[1] == 0 && uuid[0] <= 0xFFFFFFFF) {
    // Use short UUID for 16 or 32-bit UUIDs
    short_uuid = static_cast<uint32_t>(uuid[0]);
  } else {
    // Use 128-bit UUID
    uuid_field[0] = uuid[0];
    uuid_field[1] = uuid[1];
  }
}

// Helper to send GATT services done response to the connected API client
inline void send_gatt_services_done_response(
    void *api_connection,
    uint64_t address) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send GATT services done response to");
    return;
  }

  ESP_LOGD("nimble_proxy", "Sending GATT services done (address=%012llX) to API client", address);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Build and send the response
  api::BluetoothGATTGetServicesDoneResponse resp;
  resp.address = address;

  if (!conn->send_message(resp, api::BluetoothGATTGetServicesDoneResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send GATT services done response to API connection %p", api_connection);
  }
}

// Helper to send GATT characteristic notification to the connected API client
inline void send_gatt_notification(
    void *api_connection,
    uint64_t address,
    uint16_t handle,
    const uint8_t *data,
    size_t len) {

  if (api_connection == nullptr) {
    ESP_LOGV("nimble_proxy", "No API connection to send GATT notification to");
    return;
  }

  ESP_LOGV("nimble_proxy", "Sending GATT notification (address=%012llX, handle=%d, len=%d) to API client",
           address, handle, len);

  // Cast void* back to APIConnection*
  auto *conn = static_cast<api::APIConnection *>(api_connection);

  // Build and send the notification
  api::BluetoothGATTNotifyDataResponse resp;
  resp.address = address;
  resp.handle = handle;

  // Copy data to the response
  if (data != nullptr && len > 0) {
    resp.data_ptr_ = data;
    resp.data_len_ = len;
  } else {
    resp.data_ptr_ = nullptr;
    resp.data_len_ = 0;
  }

  if (!conn->send_message(resp, api::BluetoothGATTNotifyDataResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send GATT notification to API connection %p", api_connection);
  }
}

// Helper to send bluetooth connections free count to the connected API client
// Called on initial HA subscription and whenever connection state changes.
inline void send_bluetooth_connections_free(
    void *api_connection,
    uint32_t free_connections,
    uint32_t total_connections) {

  if (api_connection == nullptr) return;

  auto *conn = static_cast<api::APIConnection *>(api_connection);

  api::BluetoothConnectionsFreeResponse resp;
  resp.free = free_connections;
  resp.limit = total_connections;

  if (!conn->send_message(resp, api::BluetoothConnectionsFreeResponse::MESSAGE_TYPE)) {
    ESP_LOGW("nimble_proxy", "Failed to send connections free response");
  }
}

}  // namespace nimble_proxy
}  // namespace esphome
