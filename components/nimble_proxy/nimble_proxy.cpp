#include "nimble_proxy.h"
#include "esphome/components/nimble_base/nimble_base.h"

// Undefine NimBLE macros that conflict with ESPHome API enums
#ifdef LOG_LEVEL_NONE
#undef LOG_LEVEL_NONE
#endif
#ifdef LOG_LEVEL_ERROR
#undef LOG_LEVEL_ERROR
#endif
#ifdef LOG_LEVEL_WARN
#undef LOG_LEVEL_WARN
#endif
#ifdef LOG_LEVEL_INFO
#undef LOG_LEVEL_INFO
#endif
#ifdef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_DEBUG
#endif
#ifdef LOG_LEVEL_VERBOSE
#undef LOG_LEVEL_VERBOSE
#endif

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include <cstring>
#include <algorithm>

#ifdef USE_API
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/api_pb2.h"
#include "api_server_extensions.h"
#endif

extern "C" void ble_store_config_init(void);

namespace esphome {
namespace nimble_proxy {

static const char *const TAG = "nimble_proxy";

// Static pointer for callbacks
static NimBLEProxy *global_nimble_proxy = nullptr;

}  // namespace nimble_proxy

// Create the global_bluetooth_proxy pointer in the bluetooth_proxy namespace
// This allows the API component to access nimble_proxy via bluetooth_proxy::global_bluetooth_proxy
namespace bluetooth_proxy {
  // Define the actual storage for the pointer here
  nimble_proxy::NimBLEProxy *global_bluetooth_proxy = nullptr;
}

namespace nimble_proxy {

NimBLEProxy::NimBLEProxy() {
  // Register sync callback with nimble_base so we get notified when BLE host is ready
  nimble_base::NimBLEBase::register_sync_callback(&NimBLEProxy::on_sync_);
}

void NimBLEProxy::setup() {
  ESP_LOGD(TAG, "NimBLEProxy::setup() called on instance %p", this);
  global_nimble_proxy = this;
  bluetooth_proxy::global_bluetooth_proxy = this;

  if (!this->active_) {
    ESP_LOGI(TAG, "NimBLE Proxy is disabled");
    return;
  }

#ifdef USE_API
  // Allocate advertisement buffer
  if (!this->adv_buffer_allocated_) {
    this->adv_buffer_ = new esphome::api::BluetoothLERawAdvertisement[BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE];
    this->adv_buffer_allocated_ = true;
  }
#endif

  ESP_LOGI(TAG, "Setting up NimBLE Proxy...");

  // NimBLE initialization is handled by nimble_base component
  // The nimble_base component must be included in the YAML configuration
  // and will run first due to setup priority (AFTER_BLUETOOTH)

  ESP_LOGI(TAG, "NimBLE Proxy setup complete (initialization handled by nimble_base)");
}

void NimBLEProxy::on_sync_() {
  ESP_LOGI(TAG, "NimBLE host synced");

  if (global_nimble_proxy == nullptr) {
    ESP_LOGW(TAG, "global_nimble_proxy is null in on_sync_()");
    return;
  }

  if (!global_nimble_proxy->active_) {
    ESP_LOGI(TAG, "Proxy is not active, skipping scan/advertising");
    return;
  }

  ESP_LOGI(TAG, "Setting initialized flag and starting BLE operations");
  global_nimble_proxy->initialized_ = true;

  // If we have advertising service UUIDs (e.g., Improv), start advertising
  // Otherwise just scan (normal BLE proxy behavior)
  auto &advertising_uuids = nimble_base::NimBLEBase::get_advertising_service_uuids();
  if (!advertising_uuids.empty()) {
    ESP_LOGI(TAG, "Starting advertising (%d registered service UUIDs detected)", advertising_uuids.size());
    global_nimble_proxy->start_advertising_();
  }

  // Always start scanning for BLE proxy functionality
  ESP_LOGI(TAG, "Starting BLE scan");
  global_nimble_proxy->start_scan_();
}

void NimBLEProxy::on_reset_(int reason) {
  ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

// FreeRTOS task trampoline for NimBLE host
void NimBLEProxy::host_task_(void *param) {
  (void) param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void NimBLEProxy::start_scan_() {
  if (this->scanning_) {
    ESP_LOGV(TAG, "Already scanning");
    return;
  }

  struct ble_gap_disc_params scan_params;
  memset(&scan_params, 0, sizeof(scan_params));

  // Configure scan parameters
  // Reduced from 512/48 (320ms/30ms) to 2048/512 (1280ms/320ms)
  // This gives BLE a 25% duty cycle instead of 9%, reducing TCP traffic by ~72%
  scan_params.itvl = 2048;  // 1280ms interval (N * 0.625ms)
  scan_params.window = 512;  // 320ms window
  scan_params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  scan_params.limited = 0;  // General discovery
  scan_params.passive = 0;  // Active scanning
  scan_params.filter_duplicates = 0;  // Report all advertisements

  ESP_LOGI(TAG, "Starting BLE scan...");
  int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &scan_params,
                        NimBLEProxy::scan_callback_, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error starting scan: %d", rc);
    return;
  }

  this->scanning_ = true;
  ESP_LOGI(TAG, "BLE scan started");

  // Notify Home Assistant that we're now scanning
  this->send_scanner_state_();
}

void NimBLEProxy::stop_scan_() {
  if (!this->scanning_) {
    return;
  }

  int rc = ble_gap_disc_cancel();
  if (rc != 0) {
    ESP_LOGE(TAG, "Error stopping scan: %d", rc);
    return;
  }

  this->scanning_ = false;
  ESP_LOGI(TAG, "BLE scan stopped");

  // Notify Home Assistant that we've stopped scanning
  this->send_scanner_state_();
}

int NimBLEProxy::scan_callback_(struct ble_gap_event *event, void *arg) {
  if (event->type != BLE_GAP_EVENT_DISC) {
    return 0;
  }

  struct ble_gap_disc_desc *disc = &event->disc;

  // Log discovered device
  ESP_LOGV(TAG, "Device found: %02x:%02x:%02x:%02x:%02x:%02x RSSI=%d",
           disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
           disc->addr.val[2], disc->addr.val[1], disc->addr.val[0],
           disc->rssi);

  // Add to buffer and send to Home Assistant
  if (global_nimble_proxy != nullptr) {
    global_nimble_proxy->add_advertisement_(disc);
  }

  return 0;
}

void NimBLEProxy::start_advertising_() {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields fields;
  struct ble_hs_adv_fields rsp_fields;

  // Configure advertising parameters
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  // Undirected connectable
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // General discoverable

  // Configure main advertising data (flags + name)
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  // Build device name for BLE advertising
  // IMPORTANT: Use static storage so the pointer remains valid after function returns
  // IMPORTANT: Do NOT include MAC address suffix in BLE name to avoid HA "metadevice" errors
  //            Home Assistant extracts MAC from both the advertisement packet (BLE MAC) and
  //            device name (WiFi MAC suffix), creating a mismatch that triggers metadevice bug
  static char device_name[32];
  std::string name_base = App.get_name();

  // If we have service UUIDs (like Improv), we need a shorter name to fit in 31 bytes
  // Calculation: Flags(3) + Name(2+N) + UUID128(18) = 23+N bytes
  // Maximum name length: 31 - 23 = 8 characters
  auto &advertising_uuids = nimble_base::NimBLEBase::get_advertising_service_uuids();
  static ble_uuid128_t uuid_storage[1];
  if (!advertising_uuids.empty()) {
    // Use shortened base name without MAC suffix: "halo" becomes "h" (fits in 8 chars)
    std::string short_name = name_base;
    size_t dash_pos = short_name.find('-');
    if (dash_pos != std::string::npos) {
      short_name = short_name.substr(0, dash_pos);  // Keep only base part (e.g., "halo")
    }
    // Use just first character or short form (no MAC needed - BLE MAC is in advertisement)
    snprintf(device_name, sizeof(device_name), "%c", short_name[0]);
  } else {
    // No service UUIDs - use full base name without MAC suffix
    // Extract base name before any MAC suffix (e.g., "halo-v1-79e384" -> "halo-v1")
    std::string clean_name = name_base;
    size_t last_dash = clean_name.rfind('-');
    if (last_dash != std::string::npos) {
      // Check if the part after last dash looks like a MAC (6 hex chars)
      std::string potential_mac = clean_name.substr(last_dash + 1);
      if (potential_mac.length() == 6) {
        bool is_hex = true;
        for (char c : potential_mac) {
          if (!isxdigit(c)) {
            is_hex = false;
            break;
          }
        }
        if (is_hex) {
          // Remove the MAC suffix
          clean_name = clean_name.substr(0, last_dash);
        }
      }
    }
    snprintf(device_name, sizeof(device_name), "%s", clean_name.c_str());
  }

  // If we have service UUIDs, include them in MAIN advertising packet
  if (!advertising_uuids.empty()) {

    uuid_storage[0] = *advertising_uuids[0];
    fields.uuids128 = uuid_storage;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    ESP_LOGI(TAG, "Including Improv UUID in main advertisement (name shortened to '%s')", device_name);
  }

  fields.name = (uint8_t *) device_name;
  fields.name_len = strlen(device_name);
  fields.name_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error setting advertising fields: %d", rc);
    return;
  }

  // Start advertising
  rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                         &adv_params, NimBLEProxy::gap_event_handler_, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "Error starting advertising: %d", rc);
    return;
  }

  // Log MAC addresses for debugging metadevice issues
  uint8_t bt_addr[6], wifi_addr[6];
  if (esp_read_mac(bt_addr, ESP_MAC_BT) == 0 && esp_read_mac(wifi_addr, ESP_MAC_WIFI_STA) == 0) {
    ESP_LOGI(TAG, "Advertising started with device name: '%s'", device_name);
    ESP_LOGI(TAG, "BT MAC (advertised): %02X:%02X:%02X:%02X:%02X:%02X",
             bt_addr[0], bt_addr[1], bt_addr[2], bt_addr[3], bt_addr[4], bt_addr[5]);
    ESP_LOGI(TAG, "WiFi MAC (ESPHome): %02X:%02X:%02X:%02X:%02X:%02X",
             wifi_addr[0], wifi_addr[1], wifi_addr[2], wifi_addr[3], wifi_addr[4], wifi_addr[5]);
  } else {
    ESP_LOGI(TAG, "Advertising started");
  }
}

int NimBLEProxy::gap_event_handler_(struct ble_gap_event *event, void *arg) {
  // arg is the NimBLEConnection* pointer passed to ble_gap_connect()
  auto *conn = static_cast<NimBLEConnection *>(arg);

  if (global_nimble_proxy == nullptr) {
    ESP_LOGW(TAG, "global_nimble_proxy is null in gap_event_handler_");
    return 0;
  }

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      global_nimble_proxy->handle_gap_connect_(event, conn);
      break;

    case BLE_GAP_EVENT_DISCONNECT:
      global_nimble_proxy->handle_gap_disconnect_(event, conn);
      break;

    case BLE_GAP_EVENT_NOTIFY_RX:
      global_nimble_proxy->handle_gap_notify_(event, conn);
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

    case BLE_GAP_EVENT_ADV_COMPLETE:
      ESP_LOGV(TAG, "Advertising complete");
      break;

    case BLE_GAP_EVENT_CONN_UPDATE:
      ESP_LOGV(TAG, "Connection updated; status=%d", event->conn_update.status);
      // Connection parameters are available in the conn_desc structure
      // We can query them if needed, but for now just log the update
      break;

    default:
      break;
  }

  return 0;
}

void NimBLEProxy::setup_services_() {
  // Services will be added here for full proxy functionality
  // For now, just basic GAP/GATT services from ble_svc_gap/gatt_init()
}

void NimBLEProxy::add_advertisement_(const ble_gap_disc_desc *disc) {
#ifdef USE_API
  if (!this->adv_buffer_allocated_ || this->adv_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Advertisement buffer not allocated, skipping advertisement");
    return;
  }

  // Thread-safe check for API connection
  void *conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    conn = this->api_connection_;
  }

  // Periodically log statistics for debugging
  static int adv_count = 0;
  if (++adv_count % 100 == 0) {
    ESP_LOGV(TAG, "Advertisement stats: count=%d, api_connection=%p", adv_count, conn);
  }

  if (conn == nullptr) {
    ESP_LOGV(TAG, "No API connection, buffering advertisement (buffer has %d)", this->adv_buffer_count_);
  }

  // Cast the opaque buffer to the correct type
  auto *buffer = static_cast<esphome::api::BluetoothLERawAdvertisement *>(this->adv_buffer_);

  // Bounds check: prevent buffer overflow BEFORE writing
  if (this->adv_buffer_count_ >= BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE) {
    ESP_LOGW(TAG, "Advertisement buffer full, forcing send before adding new advertisement");
    this->send_advertisements_();
  }

  // Build 64-bit MAC address from 6-byte array
  uint64_t address = 0;
  for (int i = 0; i < 6; i++) {
    address |= ((uint64_t) disc->addr.val[i]) << (i * 8);
  }

  // Add to buffer (now safe due to bounds check above)
  auto &adv = buffer[this->adv_buffer_count_];
  adv.address = address;
  adv.rssi = disc->rssi;
  adv.address_type = disc->addr.type;

  // Copy advertisement data (limited to 62 bytes per ESPHome API)
  adv.data_len = std::min((size_t) disc->length_data, sizeof(adv.data));
  if (adv.data_len > 0 && disc->data != nullptr) {
    memcpy(adv.data, disc->data, adv.data_len);
  }

  this->adv_buffer_count_++;

  // Send when buffer is full or after 100ms timeout
  uint32_t now = millis();
  if (this->adv_buffer_count_ >= BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE ||
      (this->adv_buffer_count_ > 0 && (now - this->last_send_time_) >= 100)) {
    this->send_advertisements_();
  }
#else
  // API not available, just log
  (void) disc;
#endif
}

void NimBLEProxy::send_advertisements_() {
#ifdef USE_API
  if (this->adv_buffer_count_ == 0 || !this->adv_buffer_allocated_ || this->adv_buffer_ == nullptr) {
    return;
  }

  // Thread-safe access to api_connection_
  void *conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    conn = this->api_connection_;
  }

  ESP_LOGV(TAG, "Attempting to send %d advertisements (api_connection=%p)",
           this->adv_buffer_count_, conn);

  // Cast the opaque buffer to the correct type
  auto *buffer = static_cast<esphome::api::BluetoothLERawAdvertisement *>(this->adv_buffer_);

  esphome::api::BluetoothLERawAdvertisementsResponse resp;
  resp.advertisements_len = this->adv_buffer_count_;

  // Copy buffered advertisements
  for (uint16_t i = 0; i < this->adv_buffer_count_; i++) {
    resp.advertisements[i] = buffer[i];
  }

  // Send to the connected Home Assistant API client
  send_bluetooth_advertisements_to_client(conn, resp);

  ESP_LOGV(TAG, "Sent %d advertisements to Home Assistant", this->adv_buffer_count_);

  // Reset buffer
  this->adv_buffer_count_ = 0;
  this->last_send_time_ = millis();
#endif
}

void NimBLEProxy::loop() {
  // Send any pending advertisements periodically
  if (this->adv_buffer_count_ > 0) {
    uint32_t now = millis();
    if ((now - this->last_send_time_) >= 100) {
      this->send_advertisements_();
    }
  }
}

void NimBLEProxy::bluetooth_scanner_set_mode(bool mode) {
  // mode: true = scanning enabled, false = scanning disabled
  ESP_LOGI(TAG, "Home Assistant requested scanner mode: %s", mode ? "enabled" : "disabled");

  if (mode) {
    // Start scanning if not already scanning
    if (!this->scanning_ && this->initialized_) {
      this->start_scan_();
      // start_scan_() will call send_scanner_state_()
    }
  } else {
    // Stop scanning if currently scanning
    if (this->scanning_) {
      this->stop_scan_();
      // stop_scan_() will call send_scanner_state_()
    }
  }
}

uint32_t NimBLEProxy::get_feature_flags() {
  // Define feature flags (matching bluetooth_proxy enums)
  const uint32_t FEATURE_PASSIVE_SCAN = 1 << 0;
  const uint32_t FEATURE_ACTIVE_CONNECTIONS = 1 << 1;
  const uint32_t FEATURE_REMOTE_CACHING = 1 << 2;
  const uint32_t FEATURE_PAIRING = 1 << 3;
  const uint32_t FEATURE_CACHE_CLEARING = 1 << 4;
  const uint32_t FEATURE_RAW_ADVERTISEMENTS = 1 << 5;
  const uint32_t FEATURE_STATE_AND_MODE = 1 << 6;

  // We support passive scanning, active connections, raw advertisements, and mode reporting
  return FEATURE_PASSIVE_SCAN | FEATURE_ACTIVE_CONNECTIONS | FEATURE_RAW_ADVERTISEMENTS | FEATURE_STATE_AND_MODE;
}

std::string NimBLEProxy::get_bluetooth_mac_address_pretty() {
  // Get the BLE device address from NimBLE stack
  // The address is available after NimBLE is initialized
  if (!this->initialized_) {
    return "00:00:00:00:00:00";
  }

  // Get the public device address from NimBLE
  uint8_t addr_type;
  ble_addr_t addr;
  int rc = ble_hs_id_infer_auto(0, &addr_type);
  if (rc != 0) {
    ESP_LOGW(TAG, "Failed to determine BLE address type: %d", rc);
    return "00:00:00:00:00:00";
  }

  rc = ble_hs_id_copy_addr(addr_type, addr.val, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "Failed to copy BLE address: %d", rc);
    return "00:00:00:00:00:00";
  }

  // Format as MAC address string (reversed byte order for display)
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
           addr.val[5], addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0]);
  return std::string(mac_str);
}

void NimBLEProxy::send_scanner_state_() {
#ifdef USE_API
  void *conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    conn = this->api_connection_;
  }

  if (conn == nullptr) {
    return;
  }

  esphome::api::BluetoothScannerStateResponse resp;

  // Set scanner state based on whether we're scanning
  // States: IDLE=0, STARTING=1, RUNNING=2, FAILED=3, STOPPING=4, STOPPED=5
  if (this->scanning_) {
    resp.state = esphome::api::enums::BLUETOOTH_SCANNER_STATE_RUNNING;
  } else if (this->initialized_) {
    resp.state = esphome::api::enums::BLUETOOTH_SCANNER_STATE_STOPPED;
  } else {
    resp.state = esphome::api::enums::BLUETOOTH_SCANNER_STATE_STARTING;
  }

  // We're in active scan mode (scan_params.passive = 0 in start_scan_)
  // Modes: PASSIVE=0, ACTIVE=1
  resp.mode = esphome::api::enums::BLUETOOTH_SCANNER_MODE_ACTIVE;
  resp.configured_mode = esphome::api::enums::BLUETOOTH_SCANNER_MODE_ACTIVE;

  // Send to the connected API client
  send_scanner_state_to_client(conn, resp);
#endif
}

void NimBLEProxy::subscribe_api_connection(void *conn, uint32_t flags) {
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Attempted to subscribe null API connection");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    if (this->api_connection_ == conn) {
      ESP_LOGV(TAG, "API connection %p already subscribed", conn);
      return;
    }

    // Store the API connection (only one at a time, like native bluetooth_proxy)
    this->api_connection_ = conn;
  }

  ESP_LOGI(TAG, "API connection %p subscribed (flags=0x%x)", conn, flags);

  // Send current scanner state to the newly subscribed connection
  if (this->initialized_) {
    this->send_scanner_state_();
  }
}

void NimBLEProxy::unsubscribe_api_connection(void *conn) {
  std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
  if (this->api_connection_ == conn) {
    this->api_connection_ = nullptr;
    ESP_LOGI(TAG, "API connection %p unsubscribed", conn);
  }
}

//=============================================================================
// GAP Event Handlers for Connections
//=============================================================================

void NimBLEProxy::handle_gap_connect_(struct ble_gap_event *event, NimBLEConnection *conn) {
  if (event->connect.status == 0) {
    // Connection established successfully
    if (conn != nullptr) {
      conn->conn_handle = event->connect.conn_handle;
      conn->state = NimBLEConnectionState::CONNECTED;
      conn->state_timestamp = millis();

      ESP_LOGI(TAG, "Connection established; conn_handle=%d address=%012llX",
               conn->conn_handle, conn->address);

      // Send success response to Home Assistant
      this->send_connection_response_(conn, true, 0);

      // Start service discovery automatically
      this->start_service_discovery_(conn);
    } else {
      ESP_LOGW(TAG, "Connection established but conn pointer is null");
    }
  } else {
    // Connection failed
    ESP_LOGE(TAG, "Connection failed; status=%d", event->connect.status);

    if (conn != nullptr) {
      this->send_connection_response_(conn, false, event->connect.status);
      this->reset_connection_(conn);
    }

    // Resume advertising if we were advertising
    auto &advertising_uuids = nimble_base::NimBLEBase::get_advertising_service_uuids();
    if (!advertising_uuids.empty()) {
      this->start_advertising_();
    }
  }
}

void NimBLEProxy::handle_gap_disconnect_(struct ble_gap_event *event, NimBLEConnection *conn) {
  ESP_LOGI(TAG, "Disconnect; conn_handle=%d reason=%d",
           event->disconnect.conn.conn_handle, event->disconnect.reason);

  // If conn is null, try to find it by handle
  if (conn == nullptr) {
    conn = this->get_connection_by_handle_(event->disconnect.conn.conn_handle);
  }

  if (conn != nullptr) {
    // Send disconnection response to Home Assistant
    this->send_connection_response_(conn, false, 0);

    // Reset the connection slot
    this->reset_connection_(conn);
  } else {
    ESP_LOGW(TAG, "Disconnect event for unknown connection handle %d",
             event->disconnect.conn.conn_handle);
  }

  // Resume advertising if we were advertising
  auto &advertising_uuids = nimble_base::NimBLEBase::get_advertising_service_uuids();
  if (!advertising_uuids.empty()) {
    this->start_advertising_();
  }
}

void NimBLEProxy::handle_gap_notify_(struct ble_gap_event *event, NimBLEConnection *conn) {
  // TODO: Implement in Phase 2.4
  ESP_LOGV(TAG, "Notification received; conn_handle=%d attr_handle=%d",
           event->notify_rx.conn_handle, event->notify_rx.attr_handle);
}

//=============================================================================
// Connection Management Helper Functions
//=============================================================================

// Helper: Get or reserve connection slot
NimBLEConnection* NimBLEProxy::get_connection_(uint64_t address, bool reserve) {
  std::lock_guard<std::mutex> lock(this->connections_mutex_);

  // Find existing connection by address
  for (auto &conn : this->connections_) {
    if (conn.address == address && conn.state != NimBLEConnectionState::IDLE) {
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

// Helper: Get connection by handle
NimBLEConnection* NimBLEProxy::get_connection_by_handle_(uint16_t conn_handle) {
  std::lock_guard<std::mutex> lock(this->connections_mutex_);

  for (auto &conn : this->connections_) {
    if (conn.conn_handle == conn_handle && conn.state != NimBLEConnectionState::IDLE) {
      return &conn;
    }
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

// Helper: Send connection response to Home Assistant
void NimBLEProxy::send_connection_response_(NimBLEConnection *conn, bool connected, int error) {
#ifdef USE_API
  if (conn == nullptr) return;

  void *api_conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    api_conn = this->api_connection_;
  }

  if (api_conn == nullptr) {
    ESP_LOGW(TAG, "No API connection to send connection response");
    return;
  }

  send_connection_response(api_conn, conn->address, connected, conn->mtu, error);
  ESP_LOGI(TAG, "Sent connection response: address=%012llX connected=%d mtu=%d error=%d",
           conn->address, connected, conn->mtu, error);
#endif
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

// Service discovery helper functions
void NimBLEProxy::start_service_discovery_(NimBLEConnection *conn) {
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot start service discovery with null connection");
    return;
  }

  if (conn->conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    ESP_LOGE(TAG, "Cannot start service discovery with invalid connection handle");
    return;
  }

  // Clear any previous discovery data
  conn->services.clear();
  conn->characteristics.clear();
  conn->descriptors.clear();
  conn->current_service_idx = -1;
  conn->current_char_idx = -1;
  conn->discovery_complete = false;

  conn->state = NimBLEConnectionState::DISCOVERING_SERVICES;
  conn->state_timestamp = millis();

  ESP_LOGI(TAG, "Starting service discovery for conn_handle=%d", conn->conn_handle);

  // Start discovering all services
  int rc = ble_gattc_disc_all_svcs(conn->conn_handle, NimBLEProxy::on_disc_svc_cb_, conn);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to start service discovery; rc=%d", rc);
    conn->state = NimBLEConnectionState::ERROR;
  }
}

void NimBLEProxy::start_char_discovery_(NimBLEConnection *conn) {
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot start characteristic discovery with null connection");
    return;
  }

  if (conn->current_service_idx < 0 || conn->current_service_idx >= (int)conn->services.size()) {
    ESP_LOGE(TAG, "Invalid service index for characteristic discovery: %d", conn->current_service_idx);
    conn->state = NimBLEConnectionState::ERROR;
    return;
  }

  const auto &service = conn->services[conn->current_service_idx];

  ESP_LOGD(TAG, "Discovering characteristics for service %d (handles %d-%d)",
           conn->current_service_idx, service.start_handle, service.end_handle);

  // Discover all characteristics in this service
  int rc = ble_gattc_disc_all_chrs(conn->conn_handle,
                                   service.start_handle,
                                   service.end_handle,
                                   NimBLEProxy::on_disc_chr_cb_,
                                   conn);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to start characteristic discovery; rc=%d", rc);
    conn->state = NimBLEConnectionState::ERROR;
  }
}

void NimBLEProxy::start_dsc_discovery_(NimBLEConnection *conn) {
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot start descriptor discovery with null connection");
    return;
  }

  if (conn->current_char_idx < 0 || conn->current_char_idx >= (int)conn->characteristics.size()) {
    ESP_LOGE(TAG, "Invalid characteristic index for descriptor discovery: %d", conn->current_char_idx);
    conn->state = NimBLEConnectionState::ERROR;
    return;
  }

  const auto &characteristic = conn->characteristics[conn->current_char_idx];

  // Calculate end handle for this characteristic
  // End handle is either the next characteristic's handle - 1, or the service's end handle
  uint16_t end_handle;
  if (conn->current_char_idx + 1 < (int)conn->characteristics.size()) {
    end_handle = conn->characteristics[conn->current_char_idx + 1].def_handle - 1;
  } else {
    // This is the last characteristic, find the service's end handle
    end_handle = 0xFFFF;  // Default to max
    for (const auto &svc : conn->services) {
      if (characteristic.def_handle >= svc.start_handle && characteristic.def_handle <= svc.end_handle) {
        end_handle = svc.end_handle;
        break;
      }
    }
  }

  ESP_LOGV(TAG, "Discovering descriptors for characteristic %d (handles %d-%d)",
           conn->current_char_idx, characteristic.val_handle, end_handle);

  // Discover all descriptors for this characteristic
  int rc = ble_gattc_disc_all_dscs(conn->conn_handle,
                                   characteristic.val_handle,
                                   end_handle,
                                   NimBLEProxy::on_disc_dsc_cb_,
                                   conn);
  if (rc != 0) {
    // BLE_HS_ENOENT means no descriptors to discover - this is OK
    if (rc == BLE_HS_ENOENT) {
      ESP_LOGV(TAG, "No descriptors for characteristic %d", conn->current_char_idx);
      // Simulate completion callback
      struct ble_gatt_error error;
      error.status = BLE_HS_EDONE;
      on_disc_dsc_cb_(conn->conn_handle, &error, characteristic.val_handle, nullptr, conn);
    } else {
      ESP_LOGE(TAG, "Failed to start descriptor discovery; rc=%d", rc);
      conn->state = NimBLEConnectionState::ERROR;
    }
  }
}

void NimBLEProxy::send_service_response_(NimBLEConnection *conn) {
#ifdef USE_API
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot send service response with null connection");
    return;
  }

  void *api_conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    api_conn = this->api_connection_;
  }

  if (api_conn == nullptr) {
    ESP_LOGW(TAG, "No API connection to send service response");
    return;
  }

  ESP_LOGI(TAG, "Sending service discovery results: %d services, %d characteristics, %d descriptors",
           conn->services.size(), conn->characteristics.size(), conn->descriptors.size());

  auto *api_connection = static_cast<esphome::api::APIConnection *>(api_conn);

  // Build the nested service structure
  api::BluetoothGATTGetServicesResponse resp;
  resp.address = conn->address;

  // Iterate through services
  for (const auto &service : conn->services) {
    api::BluetoothGATTService service_resp;
    auto service_uuid = nimble_uuid_to_api_(&service.uuid);
    fill_gatt_uuid(service_resp.uuid, service_resp.short_uuid, service_uuid);
    service_resp.handle = service.start_handle;

    // Find all characteristics belonging to this service
    for (const auto &characteristic : conn->characteristics) {
      if (characteristic.def_handle >= service.start_handle &&
          characteristic.def_handle <= service.end_handle) {

        api::BluetoothGATTCharacteristic char_resp;
        auto char_uuid = nimble_uuid_to_api_(&characteristic.uuid);
        fill_gatt_uuid(char_resp.uuid, char_resp.short_uuid, char_uuid);
        char_resp.handle = characteristic.val_handle;
        char_resp.properties = characteristic.properties;

        // Find all descriptors belonging to this characteristic
        for (const auto &descriptor : conn->descriptors) {
          // Descriptors are between this char's val_handle and the next char's def_handle
          uint16_t char_end_handle = service.end_handle;

          // Find the next characteristic to determine the end boundary
          for (const auto &next_char : conn->characteristics) {
            if (next_char.def_handle > characteristic.def_handle &&
                next_char.def_handle < char_end_handle) {
              char_end_handle = next_char.def_handle - 1;
              break;
            }
          }

          if (descriptor.handle > characteristic.val_handle &&
              descriptor.handle <= char_end_handle) {

            api::BluetoothGATTDescriptor desc_resp;
            auto desc_uuid = nimble_uuid_to_api_(&descriptor.uuid);
            fill_gatt_uuid(desc_resp.uuid, desc_resp.short_uuid, desc_uuid);
            desc_resp.handle = descriptor.handle;

            char_resp.descriptors.push_back(std::move(desc_resp));
          }
        }

        service_resp.characteristics.push_back(std::move(char_resp));
      }
    }

    resp.services.push_back(std::move(service_resp));
  }

  // Send the complete response
  if (!api_connection->send_message(resp, api::BluetoothGATTGetServicesResponse::MESSAGE_TYPE)) {
    ESP_LOGW(TAG, "Failed to send GATT services response");
  }

  // Send completion message
  send_gatt_services_done_response(api_conn, conn->address);

  ESP_LOGI(TAG, "Service discovery complete for address=%012llX", conn->address);
#endif
}

uint16_t NimBLEProxy::find_cccd_handle_(NimBLEConnection *conn, uint16_t char_handle) {
  // TODO: Implement in Phase 2.4
  return 0;
}

int NimBLEProxy::on_read_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
#ifdef USE_API
  if (global_nimble_proxy == nullptr) {
    ESP_LOGW(TAG, "global_nimble_proxy is null in on_read_cb_");
    return 0;
  }

  // arg contains the connection pointer
  auto *conn = static_cast<NimBLEConnection *>(arg);
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Connection pointer is null in on_read_cb_");
    return 0;
  }

  void *api_conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(global_nimble_proxy->api_connection_mutex_);
    api_conn = global_nimble_proxy->api_connection_;
  }

  if (api_conn == nullptr) {
    ESP_LOGW(TAG, "No API connection to send read response");
    return 0;
  }

  // Check for errors
  if (error->status != 0) {
    ESP_LOGE(TAG, "GATT read failed; conn_handle=%d status=%d", conn_handle, error->status);
    send_gatt_error(api_conn, conn->address, attr ? attr->handle : 0, error->status);
    return 0;
  }

  // Extract data from os_mbuf
  if (attr != nullptr && attr->om != nullptr) {
    // Get the data length
    uint16_t data_len = OS_MBUF_PKTLEN(attr->om);

    // Allocate temporary buffer for the data
    uint8_t *data = new uint8_t[data_len];

    // Copy data from mbuf chain
    int rc = os_mbuf_copydata(attr->om, 0, data_len, data);
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to copy data from mbuf; rc=%d", rc);
      delete[] data;
      send_gatt_error(api_conn, conn->address, attr->handle, rc);
      return 0;
    }

    ESP_LOGI(TAG, "GATT read success; conn_handle=%d handle=%d len=%d",
             conn_handle, attr->handle, data_len);

    // Send read response
    send_gatt_read_response(api_conn, conn->address, attr->handle, data, data_len);

    // Clean up
    delete[] data;
  } else {
    ESP_LOGW(TAG, "Read completed but no data available");
    send_gatt_read_response(api_conn, conn->address, attr ? attr->handle : 0, nullptr, 0);
  }
#endif

  return 0;
}

int NimBLEProxy::on_write_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg) {
#ifdef USE_API
  if (global_nimble_proxy == nullptr) {
    ESP_LOGW(TAG, "global_nimble_proxy is null in on_write_cb_");
    return 0;
  }

  // arg contains the connection pointer
  auto *conn = static_cast<NimBLEConnection *>(arg);
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Connection pointer is null in on_write_cb_");
    return 0;
  }

  void *api_conn = nullptr;
  {
    std::lock_guard<std::mutex> lock(global_nimble_proxy->api_connection_mutex_);
    api_conn = global_nimble_proxy->api_connection_;
  }

  if (api_conn == nullptr) {
    ESP_LOGW(TAG, "No API connection to send write response");
    return 0;
  }

  // Check for errors
  if (error->status != 0) {
    ESP_LOGE(TAG, "GATT write failed; conn_handle=%d status=%d", conn_handle, error->status);
    send_gatt_error(api_conn, conn->address, attr ? attr->handle : 0, error->status);
    return 0;
  }

  ESP_LOGI(TAG, "GATT write success; conn_handle=%d handle=%d",
           conn_handle, attr ? attr->handle : 0);

  // For writes, we send an empty read response to indicate success
  // (ESPHome API uses BluetoothGATTReadResponse for write confirmations)
  send_gatt_read_response(api_conn, conn->address, attr ? attr->handle : 0, nullptr, 0);
#endif

  return 0;
}

int NimBLEProxy::on_disc_svc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service, void *arg) {
  if (global_nimble_proxy == nullptr) {
    return 0;
  }

  auto *conn = static_cast<NimBLEConnection *>(arg);
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Service discovery callback with null connection");
    return 0;
  }

  // Check for errors
  if (error->status != 0) {
    if (error->status == BLE_HS_EDONE) {
      // Discovery complete - move to characteristic discovery
      ESP_LOGI(TAG, "Service discovery complete; found %d services", conn->services.size());
      conn->state = NimBLEConnectionState::DISCOVERING_CHARS;
      conn->current_service_idx = 0;
      global_nimble_proxy->start_char_discovery_(conn);
    } else {
      ESP_LOGE(TAG, "Service discovery failed; status=%d", error->status);
      conn->state = NimBLEConnectionState::ERROR;
    }
    return 0;
  }

  // Store the service
  conn->services.push_back(*service);

  ESP_LOGD(TAG, "Service discovered: start_handle=%d end_handle=%d UUID=0x%04X",
           service->start_handle, service->end_handle,
           (service->uuid.u.type == BLE_UUID_TYPE_16) ? service->uuid.u16.value : 0);

  return 0;
}

int NimBLEProxy::on_disc_chr_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_chr *chr, void *arg) {
  if (global_nimble_proxy == nullptr) {
    return 0;
  }

  auto *conn = static_cast<NimBLEConnection *>(arg);
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Characteristic discovery callback with null connection");
    return 0;
  }

  // Check for errors
  if (error->status != 0) {
    if (error->status == BLE_HS_EDONE) {
      // Characteristic discovery complete for this service
      ESP_LOGD(TAG, "Characteristic discovery complete for service %d", conn->current_service_idx);

      // Move to next service
      conn->current_service_idx++;
      if (conn->current_service_idx < (int)conn->services.size()) {
        // More services to discover characteristics for
        global_nimble_proxy->start_char_discovery_(conn);
      } else {
        // All characteristics discovered - move to descriptor discovery
        ESP_LOGI(TAG, "All characteristics discovered; found %d characteristics", conn->characteristics.size());
        conn->state = NimBLEConnectionState::DISCOVERING_DSCS;
        conn->current_char_idx = 0;
        global_nimble_proxy->start_dsc_discovery_(conn);
      }
    } else {
      ESP_LOGE(TAG, "Characteristic discovery failed; status=%d", error->status);
      conn->state = NimBLEConnectionState::ERROR;
    }
    return 0;
  }

  // Store the characteristic
  conn->characteristics.push_back(*chr);

  ESP_LOGV(TAG, "Characteristic discovered: def_handle=%d val_handle=%d properties=0x%02X",
           chr->def_handle, chr->val_handle, chr->properties);

  return 0;
}

int NimBLEProxy::on_disc_dsc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  if (global_nimble_proxy == nullptr) {
    return 0;
  }

  auto *conn = static_cast<NimBLEConnection *>(arg);
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Descriptor discovery callback with null connection");
    return 0;
  }

  // Check for errors
  if (error->status != 0) {
    if (error->status == BLE_HS_EDONE) {
      // Descriptor discovery complete for this characteristic
      ESP_LOGV(TAG, "Descriptor discovery complete for characteristic %d", conn->current_char_idx);

      // Move to next characteristic
      conn->current_char_idx++;
      if (conn->current_char_idx < (int)conn->characteristics.size()) {
        // More characteristics to discover descriptors for
        global_nimble_proxy->start_dsc_discovery_(conn);
      } else {
        // All descriptors discovered - discovery complete!
        ESP_LOGI(TAG, "All descriptors discovered; found %d descriptors", conn->descriptors.size());
        conn->state = NimBLEConnectionState::READY;
        conn->discovery_complete = true;

        // Send all discovered services/characteristics/descriptors to Home Assistant
        global_nimble_proxy->send_service_response_(conn);
      }
    } else {
      ESP_LOGE(TAG, "Descriptor discovery failed; status=%d", error->status);
      conn->state = NimBLEConnectionState::ERROR;
    }
    return 0;
  }

  // Store the descriptor
  conn->descriptors.push_back(*dsc);

  ESP_LOGV(TAG, "Descriptor discovered: handle=%d", dsc->handle);

  return 0;
}

void NimBLEProxy::dump_config() {
  bool has_connection = false;
  {
    std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
    has_connection = (this->api_connection_ != nullptr);
  }

  ESP_LOGCONFIG(TAG, "NimBLE Proxy:");
  ESP_LOGCONFIG(TAG, "  Active: %s", YESNO(this->active_));
  ESP_LOGCONFIG(TAG, "  Connection Slots: %d", this->connection_slots_);
  ESP_LOGCONFIG(TAG, "  Initialized: %s", YESNO(this->initialized_));
  ESP_LOGCONFIG(TAG, "  Host task started: %s", YESNO(this->host_task_started_));
  ESP_LOGCONFIG(TAG, "  API Connection: %s", has_connection ? "connected" : "none");
  ESP_LOGCONFIG(TAG, "  BT controller status: %d", (int) esp_bt_controller_get_status());
}

//=============================================================================
// Bluetooth Proxy API Template Implementations
//=============================================================================

#ifdef USE_API
template<typename T>
void NimBLEProxy::bluetooth_device_request(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_device_request: address=%012llX type=%d",
           msg.address, msg.request_type);

  switch (msg.request_type) {
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT:
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITH_CACHE:
    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITHOUT_CACHE: {
      // Get or reserve connection slot
      NimBLEConnection *conn = this->get_connection_(msg.address, true);
      if (conn == nullptr) {
        ESP_LOGE(TAG, "No available connection slots");
        return;
      }

      // Already connected?
      if (conn->state == NimBLEConnectionState::CONNECTED ||
          conn->state == NimBLEConnectionState::READY) {
        ESP_LOGI(TAG, "Already connected");
        this->send_connection_response_(conn, true, 0);
        return;
      }

      // Already connecting?
      if (conn->state == NimBLEConnectionState::CONNECTING) {
        ESP_LOGW(TAG, "Connection already in progress");
        return;
      }

      // Build BLE address from uint64_t
      ble_addr_t addr;
      addr.type = msg.has_address_type ? msg.address_type : BLE_ADDR_PUBLIC;
      for (int i = 0; i < 6; i++) {
        addr.val[i] = (msg.address >> (i * 8)) & 0xFF;
      }

      conn->address_type = addr.type;
      conn->state = NimBLEConnectionState::CONNECTING;
      conn->state_timestamp = millis();

      ESP_LOGI(TAG, "Initiating connection to %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
               addr.val[5], addr.val[4], addr.val[3], addr.val[2], addr.val[1], addr.val[0],
               addr.type);

      // Initiate connection (30 second timeout)
      int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, NULL,
                              NimBLEProxy::gap_event_handler_, conn);
      if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed; rc=%d", rc);
        this->send_connection_response_(conn, false, rc);
        this->reset_connection_(conn);
      }
      break;
    }

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_DISCONNECT: {
      NimBLEConnection *conn = this->get_connection_(msg.address, false);
      if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
        ESP_LOGW(TAG, "Connection not found for disconnect");
        return;
      }

      ESP_LOGI(TAG, "Disconnecting from address=%012llX conn_handle=%d",
               conn->address, conn->conn_handle);

      conn->state = NimBLEConnectionState::DISCONNECTING;

      int rc = ble_gap_terminate(conn->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_terminate failed; rc=%d", rc);
        // Reset anyway on error
        this->reset_connection_(conn);
      }
      break;
    }

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR:
      // TODO: Implement pairing in Phase 2.6
      ESP_LOGW(TAG, "Pairing not yet implemented");
      break;

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_UNPAIR:
      // TODO: Implement unpairing in Phase 2.6
      ESP_LOGW(TAG, "Unpairing not yet implemented");
      break;

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CLEAR_CACHE:
      // TODO: Implement cache clearing in Phase 2.5
      ESP_LOGW(TAG, "Cache clearing not yet implemented");
      break;

    default:
      ESP_LOGW(TAG, "Unknown device request type: %d", msg.request_type);
      break;
  }
}

// GATT operation implementations (Phase 2.3)
template<typename T>
void NimBLEProxy::bluetooth_gatt_read(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_gatt_read: address=%012llX handle=%d",
           msg.address, msg.handle);

  // Find the connection
  NimBLEConnection *conn = this->get_connection_(msg.address, false);
  if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
    ESP_LOGE(TAG, "Connection not found for address=%012llX", msg.address);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_ENOTCONN);
    }
#endif
    return;
  }

  // Verify connection is ready for operations
  if (conn->state != NimBLEConnectionState::READY &&
      conn->state != NimBLEConnectionState::CONNECTED) {
    ESP_LOGW(TAG, "Connection not ready for read; state=%d", (int)conn->state);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_EBUSY);
    }
#endif
    return;
  }

  // Perform the GATT read
  int rc = ble_gattc_read(conn->conn_handle, msg.handle,
                          NimBLEProxy::on_read_cb_, conn);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gattc_read failed; rc=%d", rc);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, rc);
    }
#endif
  }
}

template<typename T>
void NimBLEProxy::bluetooth_gatt_write(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_gatt_write: address=%012llX handle=%d len=%d response=%d",
           msg.address, msg.handle, msg.data_len_, msg.response);

  // Find the connection
  NimBLEConnection *conn = this->get_connection_(msg.address, false);
  if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
    ESP_LOGE(TAG, "Connection not found for address=%012llX", msg.address);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_ENOTCONN);
    }
#endif
    return;
  }

  // Verify connection is ready for operations
  if (conn->state != NimBLEConnectionState::READY &&
      conn->state != NimBLEConnectionState::CONNECTED) {
    ESP_LOGW(TAG, "Connection not ready for write; state=%d", (int)conn->state);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_EBUSY);
    }
#endif
    return;
  }

  int rc;

  // Choose write method based on response flag
  if (msg.response) {
    // Write with response (ble_gattc_write_flat)
    rc = ble_gattc_write_flat(conn->conn_handle, msg.handle,
                              msg.data_ptr_, msg.data_len_,
                              NimBLEProxy::on_write_cb_, conn);
  } else {
    // Write without response (ble_gattc_write_no_rsp_flat)
    rc = ble_gattc_write_no_rsp_flat(conn->conn_handle, msg.handle,
                                     msg.data_ptr_, msg.data_len_);

    // For write without response, send immediate success
    if (rc == 0) {
#ifdef USE_API
      void *api_conn = nullptr;
      {
        std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
        api_conn = this->api_connection_;
      }
      if (api_conn != nullptr) {
        send_gatt_read_response(api_conn, msg.address, msg.handle, nullptr, 0);
      }
#endif
    }
  }

  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gattc_write failed; rc=%d", rc);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, rc);
    }
#endif
  }
}

template<typename T>
void NimBLEProxy::bluetooth_gatt_read_descriptor(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_gatt_read_descriptor: address=%012llX handle=%d",
           msg.address, msg.handle);

  // Find the connection
  NimBLEConnection *conn = this->get_connection_(msg.address, false);
  if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
    ESP_LOGE(TAG, "Connection not found for address=%012llX", msg.address);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_ENOTCONN);
    }
#endif
    return;
  }

  // Verify connection is ready for operations
  if (conn->state != NimBLEConnectionState::READY &&
      conn->state != NimBLEConnectionState::CONNECTED) {
    ESP_LOGW(TAG, "Connection not ready for descriptor read; state=%d", (int)conn->state);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_EBUSY);
    }
#endif
    return;
  }

  // Perform the GATT descriptor read (uses same function as characteristic read)
  int rc = ble_gattc_read(conn->conn_handle, msg.handle,
                          NimBLEProxy::on_read_cb_, conn);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gattc_read (descriptor) failed; rc=%d", rc);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, rc);
    }
#endif
  }
}

template<typename T>
void NimBLEProxy::bluetooth_gatt_write_descriptor(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_gatt_write_descriptor: address=%012llX handle=%d len=%d",
           msg.address, msg.handle, msg.data_len_);

  // Find the connection
  NimBLEConnection *conn = this->get_connection_(msg.address, false);
  if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
    ESP_LOGE(TAG, "Connection not found for address=%012llX", msg.address);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_ENOTCONN);
    }
#endif
    return;
  }

  // Verify connection is ready for operations
  if (conn->state != NimBLEConnectionState::READY &&
      conn->state != NimBLEConnectionState::CONNECTED) {
    ESP_LOGW(TAG, "Connection not ready for descriptor write; state=%d", (int)conn->state);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_EBUSY);
    }
#endif
    return;
  }

  // Perform the GATT descriptor write (uses same function as characteristic write)
  // Descriptors always use write with response
  int rc = ble_gattc_write_flat(conn->conn_handle, msg.handle,
                                msg.data_ptr_, msg.data_len_,
                                NimBLEProxy::on_write_cb_, conn);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gattc_write_flat (descriptor) failed; rc=%d", rc);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, rc);
    }
#endif
  }
}

template<typename T>
void NimBLEProxy::bluetooth_gatt_send_services(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_gatt_send_services: address=%012llX", msg.address);

  // Find the connection
  NimBLEConnection *conn = this->get_connection_(msg.address, false);
  if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
    ESP_LOGW(TAG, "Connection not found for address=%012llX", msg.address);
    return;
  }

  // If discovery is already complete, resend the cached data
  if (conn->discovery_complete && conn->state == NimBLEConnectionState::READY) {
    ESP_LOGI(TAG, "Resending cached service data for address=%012llX", msg.address);
    this->send_service_response_(conn);
  }
  // If discovery is in progress, just wait for it to complete
  else if (conn->state == NimBLEConnectionState::DISCOVERING_SERVICES ||
           conn->state == NimBLEConnectionState::DISCOVERING_CHARS ||
           conn->state == NimBLEConnectionState::DISCOVERING_DSCS) {
    ESP_LOGI(TAG, "Service discovery already in progress for address=%012llX", msg.address);
  }
  // If connected but discovery hasn't started, start it now
  else if (conn->state == NimBLEConnectionState::CONNECTED) {
    ESP_LOGI(TAG, "Starting service discovery for address=%012llX", msg.address);
    this->start_service_discovery_(conn);
  }
  else {
    ESP_LOGW(TAG, "Connection not in valid state for service discovery: state=%d", (int)conn->state);
  }
}

template<typename T>
void NimBLEProxy::bluetooth_gatt_notify(const T &msg) {
  // TODO: Implement in Phase 2.4
  ESP_LOGW(TAG, "bluetooth_gatt_notify not yet implemented (address=%012llX, handle=%d)",
           msg.address, msg.handle);
}

// Explicit template instantiations for all API message types
template void NimBLEProxy::bluetooth_device_request(const api::BluetoothDeviceRequest &msg);
template void NimBLEProxy::bluetooth_gatt_read(const api::BluetoothGATTReadRequest &msg);
template void NimBLEProxy::bluetooth_gatt_write(const api::BluetoothGATTWriteRequest &msg);
template void NimBLEProxy::bluetooth_gatt_read_descriptor(const api::BluetoothGATTReadDescriptorRequest &msg);
template void NimBLEProxy::bluetooth_gatt_write_descriptor(const api::BluetoothGATTWriteDescriptorRequest &msg);
template void NimBLEProxy::bluetooth_gatt_send_services(const api::BluetoothGATTGetServicesRequest &msg);
template void NimBLEProxy::bluetooth_gatt_notify(const api::BluetoothGATTNotifyRequest &msg);
#endif

}  // namespace nimble_proxy
}  // namespace esphome
