# Phase 2 Implementation Roadmap: Active GATT Connections for nimble_proxy

This document provides a step-by-step implementation guide for adding active GATT connection support to nimble_proxy, matching the functionality of ESPHome's Bluedroid bluetooth_proxy.

## Table of Contents
1. [Overview](#overview)
2. [Implementation Phases](#implementation-phases)
3. [Data Structures](#data-structures)
4. [Code Changes by File](#code-changes-by-file)
5. [Testing Strategy](#testing-strategy)

---

## Overview

### Current State (Phase 1)
- ✅ Passive BLE scanning and advertisement forwarding
- ✅ Raw advertisement data support
- ✅ Scanner state/mode reporting
- ✅ Bluedroid API compatibility (`connection_slots`, `cache_services` parameters)

### Phase 2 Goals
- 🎯 Active GATT connections (connect/disconnect)
- 🎯 Service/characteristic/descriptor discovery
- 🎯 GATT read/write operations
- 🎯 Notification/indication subscriptions
- 🎯 Optional: Service caching in NVS flash
- 🎯 Optional: Pairing/bonding support

### Feature Flags After Phase 2
```cpp
FEATURE_PASSIVE_SCAN          // Already supported
FEATURE_RAW_ADVERTISEMENTS    // Already supported
FEATURE_STATE_AND_MODE        // Already supported
FEATURE_ACTIVE_CONNECTIONS    // NEW
FEATURE_REMOTE_CACHING        // NEW (if NVS caching implemented)
FEATURE_PAIRING               // NEW (if pairing implemented)
FEATURE_CACHE_CLEARING        // NEW (if NVS caching implemented)
```

---

## Implementation Phases

### Phase 2.1: Connection State Machine (Start Here)
**Goal**: Basic connect/disconnect functionality

**Files to Modify**:
- `nimble_proxy.h` - Add connection structures
- `nimble_proxy.cpp` - Implement connection management
- `api_server_extensions.h` - Add API response helpers

**Steps**:
1. Add `NimBLEConnectionState` enum
2. Add `NimBLEConnection` struct
3. Add `connections_` array member
4. Implement `get_connection()` helper
5. Implement `bluetooth_device_request()` for CONNECT/DISCONNECT
6. Add connection GAP event handling
7. Update `get_feature_flags()` to include `ACTIVE_CONNECTIONS`

**Estimated Complexity**: Medium (2-4 hours)

### Phase 2.2: Service Discovery
**Goal**: Automatically discover services/characteristics/descriptors on connection

**Steps**:
1. Add service/characteristic/descriptor storage to `NimBLEConnection`
2. Implement automatic service discovery after connection
3. Add discovery state machine (SERVICES → CHARS → DSCS → READY)
4. Send discovery results to Home Assistant
5. Implement UUID conversion helpers

**Estimated Complexity**: High (4-6 hours)

### Phase 2.3: GATT Read/Write
**Goal**: Basic characteristic read/write operations

**Steps**:
1. Implement `bluetooth_gatt_read()`
2. Implement `bluetooth_gatt_write()`
3. Implement `bluetooth_gatt_read_descriptor()`
4. Implement `bluetooth_gatt_write_descriptor()`
5. Add callbacks for read/write completion
6. Handle `os_mbuf` data buffers

**Estimated Complexity**: Medium (3-4 hours)

### Phase 2.4: Notifications
**Goal**: Subscribe/unsubscribe to characteristic notifications

**Steps**:
1. Implement `bluetooth_gatt_notify()` for subscribe/unsubscribe
2. Add CCCD handle discovery
3. Handle `BLE_GAP_EVENT_NOTIFY_RX` events
4. Forward notifications to Home Assistant

**Estimated Complexity**: Medium (2-3 hours)

### Phase 2.5: Service Caching (Optional)
**Goal**: Store discovered services in NVS flash for faster reconnection

**Steps**:
1. Add NVS namespace for GATT cache
2. Implement `save_service_cache()`
3. Implement `load_service_cache()`
4. Implement `clear_service_cache()`
5. Support `CONNECT_V3_WITH_CACHE` / `CONNECT_V3_WITHOUT_CACHE`
6. Update `cache_services` parameter to be functional

**Estimated Complexity**: Medium (3-4 hours)

### Phase 2.6: Pairing/Bonding (Optional)
**Goal**: Security and pairing support

**Steps**:
1. Configure NimBLE security manager
2. Implement `bluetooth_device_request()` PAIR/UNPAIR
3. Handle encryption events
4. Implement bond storage/deletion

**Estimated Complexity**: Medium-High (4-5 hours)

**Total Estimated Time**: 14-22 hours (excluding optional features)

---

## Data Structures

### Connection State Enum

```cpp
// nimble_proxy.h
enum class NimBLEConnectionState : uint8_t {
  IDLE = 0,              // No connection, slot available
  CONNECTING,            // ble_gap_connect() called
  CONNECTED,             // BLE_GAP_EVENT_CONNECT received
  DISCOVERING_SERVICES,  // ble_gattc_disc_all_svcs() in progress
  DISCOVERING_CHARS,     // ble_gattc_disc_all_chrs() in progress
  DISCOVERING_DSCS,      // ble_gattc_disc_all_dscs() in progress
  READY,                 // Discovery complete, ready for operations
  PAIRING,               // ble_gap_security_initiate() in progress
  DISCONNECTING,         // ble_gap_terminate() called
  ERROR                  // Connection failed, retry possible
};
```

### Connection Tracking Structure

```cpp
// nimble_proxy.h
struct NimBLEConnection {
  // Identity
  uint64_t address{0};
  uint8_t address_type{BLE_ADDR_PUBLIC};
  uint16_t conn_handle{BLE_HS_CONN_HANDLE_NONE};

  // State management
  NimBLEConnectionState state{NimBLEConnectionState::IDLE};
  uint32_t state_timestamp{0};

  // Connection parameters
  uint16_t mtu{23};
  uint16_t conn_interval{0};
  uint16_t conn_latency{0};
  uint16_t supervision_timeout{0};

  // Service discovery state
  std::vector<ble_gatt_svc> services;
  std::vector<ble_gatt_chr> characteristics;
  std::vector<ble_gatt_dsc> descriptors;

  // Discovery iteration state
  int current_service_idx{-1};
  int current_char_idx{-1};
  bool discovery_complete{false};

  // Security
  bool encrypted{false};
  bool bonded{false};

  // Notification subscriptions
  std::set<uint16_t> subscribed_handles;

  // Retry policy
  struct {
    uint8_t max_attempts{3};
    uint32_t retry_delay_ms{1000};
    uint8_t attempt_count{0};
    uint32_t last_attempt_time{0};
  } retry_policy;

  void reset() {
    address = 0;
    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    state = NimBLEConnectionState::IDLE;
    services.clear();
    characteristics.clear();
    descriptors.clear();
    subscribed_handles.clear();
    retry_policy.attempt_count = 0;
  }

  bool is_active() const {
    return state != NimBLEConnectionState::IDLE;
  }
};
```

---

## Code Changes by File

### 1. `nimble_proxy.h` Changes

#### Add to includes (top of file):
```cpp
#include <set>
```

#### Add after `#define BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE`:
```cpp
// Connection state enum (see above)
enum class NimBLEConnectionState : uint8_t { /* ... */ };

// Connection tracking structure (see above)
struct NimBLEConnection { /* ... */ };
```

#### Replace template stubs with real declarations:
```cpp
// OLD (lines 69-76):
template<typename T> void bluetooth_device_request(const T &msg) { }
template<typename T> void bluetooth_gatt_read(const T &msg) { }
template<typename T> void bluetooth_gatt_write(const T &msg) { }
template<typename T> void bluetooth_gatt_read_descriptor(const T &msg) { }
template<typename T> void bluetooth_gatt_write_descriptor(const T &msg) { }
template<typename T> void bluetooth_gatt_send_services(const T &msg) { }
template<typename T> void bluetooth_gatt_notify(const T &msg) { }

// NEW:
#ifdef USE_API
  template<typename T> void bluetooth_device_request(const T &msg);
  template<typename T> void bluetooth_gatt_read(const T &msg);
  template<typename T> void bluetooth_gatt_write(const T &msg);
  template<typename T> void bluetooth_gatt_read_descriptor(const T &msg);
  template<typename T> void bluetooth_gatt_write_descriptor(const T &msg);
  template<typename T> void bluetooth_gatt_send_services(const T &msg);
  template<typename T> void bluetooth_gatt_notify(const T &msg);
#else
  template<typename T> void bluetooth_device_request(const T &msg) { }
  template<typename T> void bluetooth_gatt_read(const T &msg) { }
  template<typename T> void bluetooth_gatt_write(const T &msg) { }
  template<typename T> void bluetooth_gatt_read_descriptor(const T &msg) { }
  template<typename T> void bluetooth_gatt_write_descriptor(const T &msg) { }
  template<typename T> void bluetooth_gatt_send_services(const T &msg) { }
  template<typename T> void bluetooth_gatt_notify(const T &msg) { }
#endif
```

#### Add to protected section:
```cpp
protected:
  // Existing members...

  // Connection tracking
  std::array<NimBLEConnection, BLUETOOTH_PROXY_MAX_CONNECTIONS> connections_;
  std::mutex connections_mutex_;

  // Connection management helpers
  NimBLEConnection* get_connection_(uint64_t address, bool reserve);
  void reset_connection_(NimBLEConnection *conn);
  void send_connection_response_(NimBLEConnection *conn, bool connected, int error = 0);

  // Service discovery helpers
  void start_service_discovery_(NimBLEConnection *conn);
  void start_char_discovery_(NimBLEConnection *conn);
  void start_dsc_discovery_(NimBLEConnection *conn);
  void send_service_response_(NimBLEConnection *conn);

  // GATT operation helpers
  static int on_read_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg);
  static int on_write_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg);

  // Discovery callbacks
  static int on_disc_svc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg);
  static int on_disc_chr_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);
  static int on_disc_dsc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

  // UUID conversion helpers
  static std::array<uint64_t, 2> nimble_uuid_to_api_(const ble_uuid_any_t *uuid);
  static ble_uuid_any_t api_uuid_to_nimble_(const std::array<uint64_t, 2> &uuid);
  static uint16_t find_cccd_handle_(NimBLEConnection *conn, uint16_t char_handle);
```

### 2. `nimble_proxy.cpp` Changes

#### Add to includes:
```cpp
#include <algorithm>
```

#### Modify `gap_event_handler_()` to handle connection events:
```cpp
int NimBLEProxy::gap_event_handler_(struct ble_gap_event *event, void *arg) {
  auto *conn = static_cast<NimBLEConnection *>(arg);

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (global_nimble_proxy != nullptr) {
        global_nimble_proxy->handle_gap_connect_(event, conn);
      }
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      if (global_nimble_proxy != nullptr) {
        global_nimble_proxy->handle_gap_disconnect_(event, conn);
      }
      break;

    case BLE_GAP_EVENT_NOTIFY_RX:
      if (global_nimble_proxy != nullptr) {
        global_nimble_proxy->handle_gap_notify_(event, conn);
      }
      break;

    case BLE_GAP_EVENT_MTU:
      if (conn != nullptr) {
        conn->mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU updated; conn_handle=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
      }
      break;

    case BLE_GAP_EVENT_ENC_CHANGE:
      if (conn != nullptr) {
        conn->encrypted = (event->enc_change.status == 0);
        ESP_LOGI(TAG, "Encryption changed; conn_handle=%d status=%d",
                 conn->conn_handle, event->enc_change.status);
      }
      break;

    // ... existing cases for ADV_COMPLETE, CONN_UPDATE ...

    default:
      break;
  }

  return 0;
}
```

#### Add helper implementations at end of file:
```cpp
// Helper: Get or reserve connection slot
NimBLEConnection* NimBLEProxy::get_connection_(uint64_t address, bool reserve) {
  std::lock_guard<std::mutex> lock(this->connections_mutex_);

  // Find existing connection by address
  for (auto &conn : this->connections_) {
    if (conn.address == address) {
      return &conn;
    }
  }

  // Reserve new slot if requested
  if (reserve) {
    for (auto &conn : this->connections_) {
      if (conn.state == NimBLEConnectionState::IDLE) {
        conn.address = address;
        conn.state = NimBLEConnectionState::CONNECTING;
        conn.state_timestamp = millis();
        return &conn;
      }
    }
    ESP_LOGW(TAG, "No available connection slots");
  }

  return nullptr;
}

// Helper: Reset connection to IDLE state
void NimBLEProxy::reset_connection_(NimBLEConnection *conn) {
  if (conn == nullptr) return;

  std::lock_guard<std::mutex> lock(this->connections_mutex_);

  ESP_LOGI(TAG, "Resetting connection slot (address=%012llX)", conn->address);
  conn->reset();
}

// UUID conversion helper
std::array<uint64_t, 2> NimBLEProxy::nimble_uuid_to_api_(const ble_uuid_any_t *uuid) {
  std::array<uint64_t, 2> result{0, 0};

  if (uuid->u.type == BLE_UUID_TYPE_16) {
    result[0] = uuid->u16.value;
  } else if (uuid->u.type == BLE_UUID_TYPE_32) {
    result[0] = uuid->u32.value;
  } else if (uuid->u.type == BLE_UUID_TYPE_128) {
    memcpy(&result[0], &uuid->u128.value[0], 8);
    memcpy(&result[1], &uuid->u128.value[8], 8);
  }

  return result;
}

ble_uuid_any_t NimBLEProxy::api_uuid_to_nimble_(const std::array<uint64_t, 2> &uuid) {
  ble_uuid_any_t result;
  memset(&result, 0, sizeof(result));

  if (uuid[1] == 0 && uuid[0] <= 0xFFFF) {
    result.u.type = BLE_UUID_TYPE_16;
    result.u16.value = uuid[0];
  } else if (uuid[1] == 0 && uuid[0] <= 0xFFFFFFFF) {
    result.u.type = BLE_UUID_TYPE_32;
    result.u32.value = uuid[0];
  } else {
    result.u.type = BLE_UUID_TYPE_128;
    memcpy(&result.u128.value[0], &uuid[0], 8);
    memcpy(&result.u128.value[8], &uuid[1], 8);
  }

  return result;
}
```

#### Implement `bluetooth_device_request()` template (at end of file, before closing namespace):
```cpp
#ifdef USE_API
template<typename T>
void NimBLEProxy::bluetooth_device_request(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_device_request: address=%012llX type=%d",
           msg.address, msg.request_type);

  switch (msg.request_type) {
    case BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT:
    case BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITH_CACHE:
    case BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITHOUT_CACHE: {
      // Get or reserve connection slot
      NimBLEConnection *conn = this->get_connection_(msg.address, true);
      if (conn == nullptr) {
        ESP_LOGE(TAG, "No available connection slots");
        // Send error response
        return;
      }

      // Already connected?
      if (conn->state == NimBLEConnectionState::CONNECTED ||
          conn->state == NimBLEConnectionState::READY) {
        ESP_LOGI(TAG, "Already connected");
        this->send_connection_response_(conn, true);
        return;
      }

      // Already connecting?
      if (conn->state == NimBLEConnectionState::CONNECTING) {
        ESP_LOGW(TAG, "Connection already in progress");
        return;
      }

      // Build BLE address
      ble_addr_t addr;
      addr.type = msg.has_address_type ? msg.address_type : BLE_ADDR_PUBLIC;
      for (int i = 0; i < 6; i++) {
        addr.val[i] = (msg.address >> (i * 8)) & 0xFF;
      }

      conn->address_type = addr.type;
      conn->state = NimBLEConnectionState::CONNECTING;
      conn->state_timestamp = millis();

      // Initiate connection
      int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, NULL,
                              NimBLEProxy::gap_event_handler_, conn);
      if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed; rc=%d", rc);
        this->send_connection_response_(conn, false, rc);
        this->reset_connection_(conn);
      }
      break;
    }

    case BLUETOOTH_DEVICE_REQUEST_TYPE_DISCONNECT: {
      NimBLEConnection *conn = this->get_connection_(msg.address, false);
      if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
        ESP_LOGW(TAG, "Connection not found for disconnect");
        return;
      }

      conn->state = NimBLEConnectionState::DISCONNECTING;

      int rc = ble_gap_terminate(conn->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_terminate failed; rc=%d", rc);
        this->reset_connection_(conn);
      }
      break;
    }

    case BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR:
      // TODO: Implement pairing
      ESP_LOGW(TAG, "Pairing not yet implemented");
      break;

    case BLUETOOTH_DEVICE_REQUEST_TYPE_UNPAIR:
      // TODO: Implement unpairing
      ESP_LOGW(TAG, "Unpairing not yet implemented");
      break;

    case BLUETOOTH_DEVICE_REQUEST_TYPE_CLEAR_CACHE:
      // TODO: Implement cache clearing
      ESP_LOGW(TAG, "Cache clearing not yet implemented");
      break;
  }
}

// Explicit template instantiation for API message types
template void NimBLEProxy::bluetooth_device_request(const api::BluetoothDeviceRequest &msg);
#endif
```

#### Update `get_feature_flags()`:
```cpp
uint32_t NimBLEProxy::get_feature_flags() {
  const uint32_t FEATURE_PASSIVE_SCAN = 1 << 0;
  const uint32_t FEATURE_ACTIVE_CONNECTIONS = 1 << 1;  // NEW
  const uint32_t FEATURE_RAW_ADVERTISEMENTS = 1 << 5;
  const uint32_t FEATURE_STATE_AND_MODE = 1 << 6;

  return FEATURE_PASSIVE_SCAN | FEATURE_ACTIVE_CONNECTIONS |
         FEATURE_RAW_ADVERTISEMENTS | FEATURE_STATE_AND_MODE;
}
```

### 3. `api_server_extensions.h` Changes

Add helper functions for sending responses to Home Assistant:

```cpp
// Send connection response
inline void send_connection_response(void *api_conn, uint64_t address,
                                     bool connected, uint16_t mtu, int error) {
  if (api_conn == nullptr) return;

  auto *conn = static_cast<esphome::api::APIConnection *>(api_conn);
  esphome::api::BluetoothDeviceConnectionResponse resp;
  resp.address = address;
  resp.connected = connected;
  resp.mtu = mtu;
  resp.error = error;

  conn->send_bluetooth_device_connection_response(resp);
}

// Send GATT read response
inline void send_gatt_read_response(void *api_conn, uint64_t address,
                                   uint16_t handle, const uint8_t *data, size_t len) {
  if (api_conn == nullptr) return;

  auto *conn = static_cast<esphome::api::APIConnection *>(api_conn);
  esphome::api::BluetoothGATTReadResponse resp;
  resp.address = address;
  resp.handle = handle;
  resp.data_len_ = len;
  resp.data_ptr_ = data;

  conn->send_bluetooth_gatt_read_response(resp);
}

// Send GATT error response
inline void send_gatt_error(void *api_conn, uint64_t address, uint16_t handle, int error) {
  if (api_conn == nullptr) return;

  auto *conn = static_cast<esphome::api::APIConnection *>(api_conn);
  esphome::api::BluetoothGATTErrorResponse resp;
  resp.address = address;
  resp.handle = handle;
  resp.error = error;

  conn->send_bluetooth_gatt_error_response(resp);
}
```

---

## Testing Strategy

### Phase 2.1 Testing: Basic Connection
1. Build and flash firmware
2. In Home Assistant, go to Settings → Devices & Services → ESPHome
3. Click on your device → Configure → Enable Bluetooth proxy
4. Try connecting to a BLE device from Home Assistant
5. Verify logs show:
   - `bluetooth_device_request: address=... type=0` (CONNECT)
   - `ble_gap_connect` call
   - `Connection established; conn_handle=...`
   - `BluetoothDeviceConnectionResponse` sent

### Phase 2.2 Testing: Service Discovery
1. Connect to a BLE device
2. Verify logs show:
   - Service discovery starting
   - `Service discovered: start_handle=... end_handle=...`
   - Characteristic discovery for each service
   - Descriptor discovery for each characteristic
   - `BluetoothGATTGetServicesResponse` sent
   - `BluetoothGATTGetServicesDoneResponse` sent
3. In Home Assistant, verify services/characteristics appear in device details

### Phase 2.3 Testing: GATT Read/Write
1. Use Home Assistant Developer Tools → Actions
2. Call `esphome.halo_v1_ble_client_read` with handle
3. Verify characteristic value is returned
4. Call `esphome.halo_v1_ble_client_write` with handle and data
5. Verify write succeeds

### Phase 2.4 Testing: Notifications
1. Subscribe to a characteristic that supports notifications
2. Trigger notification on peripheral device
3. Verify notification data arrives in Home Assistant

---

## Key Considerations

### Memory Management
- Use `std::vector` for dynamic service/characteristic lists
- Each connection can hold ~50 services × 10 characteristics = 500 entries
- Estimated memory: ~20KB per connection × connection_slots

### Thread Safety
- All NimBLE callbacks run on NimBLE host thread
- Use `connections_mutex_` when accessing `connections_` array
- API calls may come from main thread or API thread

### Error Handling
- Always send error responses to Home Assistant
- Reset connection state on unrecoverable errors
- Implement connection retry logic (max 3 attempts)

### Performance
- Batch service responses (max 1360 bytes per message)
- Service discovery can take 2-4 seconds for complex devices
- Service caching reduces reconnection time from 4s to <1s

---

## Resources

- **NimBLE API Reference**: https://mynewt.apache.org/latest/network/ble_hs/ble_gattc.html
- **ESP-IDF BLE Guide**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/ble/get-started/ble-connection.html
- **ESPHome bluetooth_proxy**: https://github.com/esphome/esphome/blob/dev/esphome/components/bluetooth_proxy/
- **blecent Example**: https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/nimble/blecent/main/main.c

---

## Next Steps

1. Start with Phase 2.1 (Connection State Machine)
2. Test thoroughly before moving to Phase 2.2
3. Commit after each phase completes
4. Tag releases: v2.1, v2.2, etc.
5. Update README with new capabilities

Good luck! 🚀
