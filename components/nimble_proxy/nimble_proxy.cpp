#include "nimble_proxy.h"
#include "esphome/components/nimble_base/nimble_base.h"
#include "store/config/ble_store_config.h"
#include "freertos/FreeRTOS.h"  // for vTaskDelay in setup() diagnostic
#include "freertos/task.h"

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
#include "esphome/core/preferences.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include <cstring>
#include <algorithm>
#include <cctype>

#ifdef USE_API
#include "esphome/components/api/api_server.h"
#include "esphome/components/api/api_pb2.h"
#include "api_server_extensions.h"
#endif

extern "C" void ble_store_config_init(void);

namespace esphome {
namespace nimble_proxy {

std::mutex api_send_mutex;

static const char *const TAG = "nimble_proxy";

static bool looks_like_mac_suffix_(const std::string &value) {
  if (value.length() != 6) {
    return false;
  }
  for (char c : value) {
    if (!isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

static std::string sanitize_base_name_(const std::string &value) {
  std::string clean = value;
  size_t last_dash = clean.rfind('-');
  if (last_dash != std::string::npos) {
    std::string suffix = clean.substr(last_dash + 1);
    if (looks_like_mac_suffix_(suffix)) {
      // Drop full 6-hex MAC-like suffix to avoid HA metadevice confusion.
      clean = clean.substr(0, last_dash);
    }
  }
  return clean;
}

static std::string compact_short_name_(const std::string &value) {
  // Compact to <=8 chars and preserve uniqueness for names ending in -XXXXXX
  // by keeping a 3-hex suffix fragment without exposing a full MAC pattern.
  std::string base = sanitize_base_name_(value);
  std::string source = value;
  std::string suffix3;

  size_t last_dash = source.rfind('-');
  if (last_dash != std::string::npos) {
    std::string maybe_suffix = source.substr(last_dash + 1);
    if (looks_like_mac_suffix_(maybe_suffix)) {
      suffix3 = maybe_suffix.substr(3, 3);
      source = source.substr(0, last_dash);
      base = sanitize_base_name_(source);
    }
  }

  std::string compact;
  compact.reserve(8);
  for (char c : base) {
    if (isalnum(static_cast<unsigned char>(c))) {
      compact.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
      if (compact.length() >= 8) {
        break;
      }
    }
  }

  if (!suffix3.empty()) {
    while (compact.length() > 5) {
      compact.pop_back();
    }
    compact += suffix3;
  }

  if (compact.empty()) {
    compact = "halo";
  }
  if (compact.length() > 8) {
    compact.resize(8);
  }
  return compact;
}

static uint32_t advertisement_signature_(const ble_gap_disc_desc *disc) {
  // FNV-1a hash over source address metadata and raw payload bytes.
  uint32_t hash = 2166136261u;
  auto hash_byte = [&hash](uint8_t b) {
    hash ^= b;
    hash *= 16777619u;
  };

  hash_byte(static_cast<uint8_t>(disc->addr.type));
  for (size_t i = 0; i < sizeof(disc->addr.val); i++) {
    hash_byte(disc->addr.val[i]);
  }
  hash_byte(static_cast<uint8_t>(disc->length_data & 0xFF));
  hash_byte(static_cast<uint8_t>((disc->length_data >> 8) & 0xFF));
  if (disc->length_data > 0 && disc->data != nullptr) {
    for (size_t i = 0; i < disc->length_data; i++) {
      hash_byte(disc->data[i]);
    }
  }

  return hash;
}

// Static pointer for callbacks
NimBLEProxy *global_nimble_proxy = nullptr;

}  // namespace nimble_proxy

// Create the global_bluetooth_proxy pointer in the bluetooth_proxy namespace
// This allows the API component to access nimble_proxy via bluetooth_proxy::global_bluetooth_proxy
namespace bluetooth_proxy {
  // Define the actual storage for the pointer here
  nimble_proxy::NimBLEProxy *global_bluetooth_proxy = nullptr;
}

namespace nimble_proxy {

NimBLEProxy::NimBLEProxy() {
  ESP_LOGI(TAG, "[DIAG] NimBLEProxy::NimBLEProxy() ENTRY");
  // Set global pointers IMMEDIATELY in constructor (before App.setup() runs).
  // ESPHome 2026.x may access bluetooth_proxy::global_bluetooth_proxy between
  // component setups — setting here avoids a null-dereference crash.
  global_nimble_proxy = this;
  bluetooth_proxy::global_bluetooth_proxy = this;
  // Register sync callback with nimble_base so we get notified when BLE host is ready
  nimble_base::NimBLEBase::register_sync_callback(&NimBLEProxy::on_sync_);
  ESP_LOGI(TAG, "[DIAG] NimBLEProxy::NimBLEProxy() DONE");
}

void NimBLEProxy::setup() {
  // Delay to let USB CDC flush any pending log data before we proceed.
  // If [DIAG] below still doesn't appear in the log, the crash is in
  // virtual dispatch to setup() itself (corrupted object or vtable).
  vTaskDelay(pdMS_TO_TICKS(200));
  ESP_LOGI(TAG, "[DIAG] NimBLEProxy::setup() - post-delay sentinel");
  vTaskDelay(pdMS_TO_TICKS(200));
  ESP_LOGI(TAG, "[DIAG] NimBLEProxy::setup() - second sentinel");
#ifdef USE_API
  // Allocate API objects before NimBLE starts scanning so scan callbacks never
  // perform heap allocation or run API object constructors.
  this->adv_buffer_ = new esphome::api::BluetoothLERawAdvertisement[BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE];
  this->adv_buffer_allocated_ = (this->adv_buffer_ != nullptr);
  if (!this->adv_buffer_allocated_) {
    ESP_LOGE(TAG, "Failed to allocate advertisement buffer");
  }
#endif
  if (this->scan_duplicate_cache_size_ > 0) {
    this->recent_adv_signatures_.reserve(this->scan_duplicate_cache_size_);
  }
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

  ESP_LOGI(TAG, "Security manager configured before host startup (bonding=%d, MITM=%d, SC=%d, IO=%d)",
           ble_hs_cfg.sm_bonding, ble_hs_cfg.sm_mitm, ble_hs_cfg.sm_sc, ble_hs_cfg.sm_io_cap);

  ESP_LOGI(TAG, "Setting initialized flag and starting BLE operations");
  global_nimble_proxy->initialized_ = true;

  // If we have advertising service UUIDs (e.g., Improv), start advertising
  // Otherwise just scan (normal BLE proxy behavior)
  auto &advertising_uuids = nimble_base::NimBLEBase::get_advertising_service_uuids();
  if (!advertising_uuids.empty()) {
    ESP_LOGI(TAG, "Starting advertising (%d registered service UUIDs detected)", advertising_uuids.size());
    global_nimble_proxy->start_advertising_();
  }

  if (global_nimble_proxy->scanner_enabled_) {
    ESP_LOGI(TAG, "Starting BLE scan");
    global_nimble_proxy->start_scan_();
  } else {
    ESP_LOGI(TAG, "Scanner is disabled; skipping BLE scan start");
    global_nimble_proxy->send_scanner_state_();
  }
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

  // Configure scan parameters.
  // Values are tunable from YAML to balance discovery quality vs API load.
  scan_params.itvl = this->scan_interval_;
  scan_params.window = this->scan_window_;
  scan_params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  scan_params.limited = 0;  // General discovery
  scan_params.passive = this->scan_active_ ? 0 : 1;
  // Let NimBLE suppress repeated advertisements from the same source.
  // This reduces API traffic and drop pressure when many nearby devices spam beacons.
  scan_params.filter_duplicates = this->scan_duplicate_filter_ ? 1 : 0;

  ESP_LOGI(TAG,
           "Scan config: mode=%s interval=%u window=%u duplicate_filter=%s duplicate_cache=%u",
           this->scan_active_ ? "active" : "passive", this->scan_interval_,
           this->scan_window_, YESNO(this->scan_duplicate_filter_), this->scan_duplicate_cache_size_);

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

  if (global_nimble_proxy == nullptr || !global_nimble_proxy->scanner_enabled_) {
    return 0;
  }

  global_nimble_proxy->discovered_count_++;

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
  std::string configured_name = this->advertising_name_;
  std::string app_name = App.get_name();
  std::string name_base = configured_name.empty() ? app_name : configured_name;

  // If we have service UUIDs (like Improv), we need a shorter name to fit in 31 bytes
  // Calculation: Flags(3) + Name(2+N) + UUID128(18) = 23+N bytes
  // Maximum name length: 31 - 23 = 8 characters
  auto &advertising_uuids = nimble_base::NimBLEBase::get_advertising_service_uuids();
  static ble_uuid128_t uuid_storage[1];
  if (!advertising_uuids.empty()) {
    std::string short_name = compact_short_name_(name_base);
    snprintf(device_name, sizeof(device_name), "%s", short_name.c_str());
  } else {
    // No service UUIDs - use full base name without MAC suffix
    // Extract base name before any MAC suffix (e.g., "halo-v1-79e384" -> "halo-v1").
    std::string clean_name = sanitize_base_name_(name_base);
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
        conn->bonded = (event->enc_change.status == 0);
        ESP_LOGI(TAG, "Encryption changed; conn_handle=%d status=%d encrypted=%d",
                 conn->conn_handle, event->enc_change.status, conn->encrypted);

        // If we were in PAIRING state, send pairing response
        if (conn->state == NimBLEConnectionState::PAIRING) {
          if (global_nimble_proxy != nullptr) {
            global_nimble_proxy->send_pairing_response_(conn->address, conn->encrypted,
                                                        event->enc_change.status);
          }
          // Return to READY state
          conn->state = NimBLEConnectionState::READY;
        }
      }
      break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
      global_nimble_proxy->handle_gap_passkey_action_(event, conn);
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
  std::lock_guard<std::mutex> buffer_lock(this->adv_buffer_mutex_);

  if (!this->scanner_enabled_) {
    this->dropped_scanner_disabled_count_++;
    return;
  }

  if (this->scan_duplicate_cache_size_ > 0) {
    uint32_t signature = advertisement_signature_(disc);
    bool is_duplicate = false;

    for (uint32_t seen_signature : this->recent_adv_signatures_) {
      if (seen_signature == signature) {
        is_duplicate = true;
        break;
      }
    }

    if (is_duplicate) {
      this->duplicate_seen_count_++;
      this->duplicate_dropped_count_++;
      return;
    }

    if (this->recent_adv_signatures_.size() < this->scan_duplicate_cache_size_ &&
        this->recent_adv_signatures_.size() < this->recent_adv_signatures_.capacity()) {
      this->recent_adv_signatures_.push_back(signature);
    } else if (this->recent_adv_signatures_.size() >= this->scan_duplicate_cache_size_) {
      this->recent_adv_signatures_[this->recent_adv_signature_index_] = signature;
      this->recent_adv_signature_index_ =
          (this->recent_adv_signature_index_ + 1) % this->scan_duplicate_cache_size_;
    } else {
      ESP_LOGW(TAG, "Duplicate cache unavailable; dropping advertisement signature");
      return;
    }
  }

  if (!this->adv_buffer_allocated_ || this->adv_buffer_ == nullptr) {
    return;  // Allocation failed or not yet ready
  }

  // Cast the opaque buffer to the correct type
  auto *buffer = static_cast<esphome::api::BluetoothLERawAdvertisement *>(this->adv_buffer_);

  // Check if buffer is full - if so, drop this advertisement
  if (this->adv_buffer_count_ >= BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE) {
    this->dropped_buffer_full_count_++;
    return;  // Drop silently - main thread will drain buffer soon
  }

  // Build 64-bit MAC address from 6-byte array
  uint64_t address = 0;
  for (int i = 0; i < 6; i++) {
    address |= ((uint64_t) disc->addr.val[i]) << (i * 8);
  }

  // Add to buffer at current write position
  auto &adv = buffer[this->adv_buffer_count_];
  adv.address = address;
  adv.rssi = disc->rssi;
  adv.address_type = disc->addr.type;

  // Copy advertisement data (limited to 62 bytes per ESPHome API)
  adv.data_len = std::min((size_t) disc->length_data, sizeof(adv.data));
  if (adv.data_len > 0 && disc->data != nullptr) {
    memcpy(adv.data, disc->data, adv.data_len);
  }

  // Increment counter LAST - signals to main thread that new data is ready
  this->adv_buffer_count_++;
#else
  (void) disc;
#endif
}

void NimBLEProxy::send_advertisements_() {
#ifdef USE_API
  // Called ONLY from main thread (loop()) - no mutex needed

  std::unique_lock<std::mutex> buffer_lock(this->adv_buffer_mutex_);

  if (this->adv_buffer_count_ == 0 || !this->adv_buffer_allocated_ || this->adv_buffer_ == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> api_send_lock(api_send_mutex);
  std::lock_guard<std::mutex> api_connection_lock(this->api_connection_mutex_);

  // Get API connection while the send/lifetime lock is held.
  void *conn = this->api_connection_;
  if (conn == nullptr) {
    // No API connection - discard buffer so the same packets are not re-counted
    // on every subsequent loop() call (which would inflate dropped_no_api).
    this->dropped_no_api_count_ += this->adv_buffer_count_;
    this->adv_buffer_count_ = 0;
    return;
  }

  // Cast the opaque buffer to the correct type
  auto *buffer = static_cast<esphome::api::BluetoothLERawAdvertisement *>(this->adv_buffer_);

  esphome::api::BluetoothLERawAdvertisementsResponse resp;
  resp.advertisements_len = this->adv_buffer_count_;

  // Copy buffered advertisements
  for (uint16_t i = 0; i < this->adv_buffer_count_; i++) {
    resp.advertisements[i] = buffer[i];
  }

  // Reset buffer BEFORE sending so the NimBLE thread can write new ads. The
  // lock remains held through send_message(): this prevents the API send from
  // racing with a producer that might otherwise reuse the buffer.
  uint16_t count = this->adv_buffer_count_;
  this->adv_buffer_count_ = 0;
  this->last_send_time_ = millis();

  // Send to HA; only count as forwarded if the API output buffer accepted it.
  auto *api_connection = static_cast<esphome::api::APIConnection *>(conn);
  if (api_connection->send_message(resp)) {
    this->forwarded_count_ += count;
  } else {
    this->send_failed_count_ += count;
  }
#endif
}

void NimBLEProxy::loop() {
  // CRITICAL: ALL API sending must happen on main thread (this function)
  // ESPHome API is not thread-safe - NimBLE thread only buffers, never sends
  // No mutex needed - volatile variable provides visibility

  this->drain_api_events_();
  this->process_pending_service_response_();

  bool should_send = false;
  {
    std::lock_guard<std::mutex> buffer_lock(this->adv_buffer_mutex_);
    should_send = this->adv_buffer_count_ > 0;
  }
  if (should_send) {
    uint32_t now = millis();
    uint16_t buffered = 0;
    {
      std::lock_guard<std::mutex> buffer_lock(this->adv_buffer_mutex_);
      buffered = this->adv_buffer_count_;
    }
    // Send when buffer is full OR after configured timeout.
    if (buffered >= BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE ||
        (now - this->last_send_time_) >= this->advertisement_send_interval_ms_) {
      this->send_advertisements_();  // ONLY called from main thread - SAFE
    }
  }

  uint32_t now = millis();
  if ((now - this->last_diag_log_ms_) >= 10000) {
    bool has_api_conn = (this->api_connection_ != nullptr);
    ESP_LOGI(TAG,
             "Diag counters: scanner_enabled=%s scanning=%s api=%s discovered=%u forwarded=%u send_failed=%u dropped_full=%u dropped_disabled=%u dropped_no_api=%u dropped_api_events=%u dup_seen=%u dup_dropped=%u dup_cache=%u send_ms=%u batch=%u buffered=%u",
             YESNO(this->scanner_enabled_), YESNO(this->scanning_), YESNO(has_api_conn),
             this->discovered_count_, this->forwarded_count_, this->send_failed_count_,
             this->dropped_buffer_full_count_,
             this->dropped_scanner_disabled_count_, this->dropped_no_api_count_,
             this->dropped_api_event_count_,
             this->duplicate_seen_count_, this->duplicate_dropped_count_,
             this->scan_duplicate_cache_size_, this->advertisement_send_interval_ms_,
             BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE,
             this->adv_buffer_count_);
    this->last_diag_log_ms_ = now;
  }
}

bool NimBLEProxy::enqueue_api_event_(ApiEventType type, uint64_t address, uint16_t handle,
                                     const uint8_t *data, uint16_t data_len, int error) {
  if (data_len > BLUETOOTH_PROXY_MAX_GATT_DATA) {
    this->dropped_api_event_count_++;
    return false;
  }

  std::lock_guard<std::mutex> lock(this->api_event_mutex_);
  if (this->api_event_count_ >= BLUETOOTH_PROXY_API_EVENT_QUEUE_SIZE) {
    this->dropped_api_event_count_++;
    return false;
  }

  auto &event = this->api_events_[this->api_event_tail_];
  event.type = type;
  event.address = address;
  event.handle = handle;
  event.error = error;
  event.data_len = data_len;
  if (data_len != 0 && data != nullptr) {
    memcpy(event.data.data(), data, data_len);
  }
  this->api_event_tail_ = (this->api_event_tail_ + 1) % BLUETOOTH_PROXY_API_EVENT_QUEUE_SIZE;
  this->api_event_count_++;
  return true;
}

void NimBLEProxy::drain_api_events_() {
#ifdef USE_API
  while (true) {
    ApiEvent event;
    {
      std::lock_guard<std::mutex> lock(this->api_event_mutex_);
      if (this->api_event_count_ == 0) {
        return;
      }
      event = this->api_events_[this->api_event_head_];
      this->api_event_head_ = (this->api_event_head_ + 1) % BLUETOOTH_PROXY_API_EVENT_QUEUE_SIZE;
      this->api_event_count_--;
    }

    if (event.type == ApiEventType::NOTIFICATION) {
      send_gatt_notification(this, event.address, event.handle,
                             event.data_len ? event.data.data() : nullptr, event.data_len);
    } else if (event.type == ApiEventType::READ_RESPONSE) {
      send_gatt_read_response(this, event.address, event.handle,
                              event.data_len ? event.data.data() : nullptr, event.data_len);
    } else {
      send_gatt_error(this, event.address, event.handle, event.error);
    }
  }
#endif
}

void NimBLEProxy::process_pending_service_response_() {
  uint16_t conn_handle = this->pending_service_response_handle_;
  if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }

  this->pending_service_response_handle_ = BLE_HS_CONN_HANDLE_NONE;
  NimBLEConnection *conn = this->get_connection_by_handle_(conn_handle);
  if (conn == nullptr || !conn->discovery_complete) {
    this->pending_service_cache_save_ = false;
    return;
  }

  if (this->pending_service_cache_save_) {
    this->pending_service_cache_save_ = false;
    if (conn->use_cache && !conn->cache_loaded) {
      this->save_service_cache_(conn);
    }
  }
  this->send_service_response_(conn);
}

void NimBLEProxy::bluetooth_scanner_set_mode(bool mode) {
  // mode: true = scanning enabled, false = scanning disabled
  ESP_LOGI(TAG, "Home Assistant requested scanner mode: %s", mode ? "enabled" : "disabled");
  this->scanner_enabled_ = mode;

  if (mode) {
    // Start scanning if not already scanning
    if (!this->scanning_ && this->initialized_) {
      this->start_scan_();
      // start_scan_() will call send_scanner_state_()
    } else {
      this->send_scanner_state_();
    }
  } else {
    // Drop any buffered advertisements immediately when scanner is disabled.
    std::lock_guard<std::mutex> buffer_lock(this->adv_buffer_mutex_);
    this->adv_buffer_count_ = 0;

    // Stop scanning if currently scanning
    if (this->scanning_) {
      this->stop_scan_();
      // stop_scan_() will call send_scanner_state_()
    } else {
      this->send_scanner_state_();
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
  const uint32_t FEATURE_CONNECTION_PARAMS_SETTING = 1 << 7;

  // We support all features including pairing/bonding and connection params
  return FEATURE_PASSIVE_SCAN | FEATURE_ACTIVE_CONNECTIONS | FEATURE_REMOTE_CACHING |
         FEATURE_PAIRING | FEATURE_CACHE_CLEARING | FEATURE_RAW_ADVERTISEMENTS |
         FEATURE_STATE_AND_MODE | FEATURE_CONNECTION_PARAMS_SETTING;
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

// Backwards-compatible overload: fill provided buffer with NUL-terminated MAC string
void NimBLEProxy::get_bluetooth_mac_address_pretty(char out[18]) {
  std::string s = this->get_bluetooth_mac_address_pretty();
  // Ensure buffer is NUL-terminated and sized for "AA:BB:CC:DD:EE:FF" + NUL
  if (out == nullptr) return;
  // Copy up to 17 characters and NUL-terminate
  strncpy(out, s.c_str(), 17);
  out[17] = '\0';
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

void NimBLEProxy::send_connections_free(void *api_conn) {
#ifdef USE_API
  if (api_conn == nullptr) return;

  // Count slots currently in IDLE state
  uint8_t free_count = 0;
  {
    std::lock_guard<std::mutex> lock(this->connections_mutex_);
    for (const auto &c : this->connections_) {
      if (c.state == NimBLEConnectionState::IDLE) {
        free_count++;
      }
    }
  }

  send_bluetooth_connections_free(api_conn, free_count, this->connection_slots_);
  ESP_LOGD(TAG, "Sent connections free: %d/%d", free_count, this->connection_slots_);
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

  // Clear any stale buffered payloads captured before API subscription.
  {
    std::lock_guard<std::mutex> buffer_lock(this->adv_buffer_mutex_);
    this->adv_buffer_count_ = 0;
  }

  // Restart scanning to ensure HA receives a fresh stream immediately after
  // subscribing, even if the scanner had been running before API connected.
  if (this->initialized_ && this->scanner_enabled_) {
    if (this->scanning_) {
      this->stop_scan_();
    }
    this->start_scan_();
  }

  // Send current scanner state to the newly subscribed connection
  if (this->initialized_) {
    this->send_scanner_state_();
  }
  // Send current connection slot availability to the newly subscribed connection
  this->send_connections_free(conn);
}

void NimBLEProxy::unsubscribe_api_connection(void *conn) {
  std::lock_guard<std::mutex> send_lock(api_send_mutex);
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

      // Tell HA how many connection slots remain
      {
        void *api_conn = nullptr;
        {
          std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
          api_conn = this->api_connection_;
        }
        this->send_connections_free(api_conn);
      }
    } else {
      ESP_LOGW(TAG, "Connection established but conn pointer is null");
    }
  } else {
    // Connection failed
    ESP_LOGE(TAG, "Connection failed; status=%d", event->connect.status);

    if (conn != nullptr) {
      this->send_connection_response_(conn, false, event->connect.status);
      this->reset_connection_(conn);

      // Tell HA the freed slot is available again
      void *api_conn = nullptr;
      {
        std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
        api_conn = this->api_connection_;
      }
      this->send_connections_free(api_conn);
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

    // Tell HA the freed slot is available again
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    this->send_connections_free(api_conn);
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
  ESP_LOGV(TAG, "Notification received; conn_handle=%d attr_handle=%d indication=%d",
           event->notify_rx.conn_handle, event->notify_rx.attr_handle,
           event->notify_rx.indication);

  // If conn is null, try to find it by handle
  if (conn == nullptr) {
    conn = this->get_connection_by_handle_(event->notify_rx.conn_handle);
  }

  if (conn == nullptr) {
    ESP_LOGW(TAG, "Notification received for unknown connection handle %d",
             event->notify_rx.conn_handle);
    return;
  }

  // Check if we're subscribed to this handle
  if (conn->subscribed_handles.find(event->notify_rx.attr_handle) == conn->subscribed_handles.end()) {
    ESP_LOGV(TAG, "Received notification for unsubscribed handle %d", event->notify_rx.attr_handle);
    // Still forward it - Home Assistant might want it
  }

#ifdef USE_API
  // Extract notification data from os_mbuf
  if (event->notify_rx.om != nullptr) {
    uint16_t data_len = OS_MBUF_PKTLEN(event->notify_rx.om);

    if (data_len > BLUETOOTH_PROXY_MAX_GATT_DATA) {
      ESP_LOGW(TAG, "Dropping oversized notification: len=%u max=%u", data_len,
               BLUETOOTH_PROXY_MAX_GATT_DATA);
      return;
    }

    std::array<uint8_t, BLUETOOTH_PROXY_MAX_GATT_DATA> data{};

    // Copy data from mbuf chain
    int rc = os_mbuf_copydata(event->notify_rx.om, 0, data_len, data.data());
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to copy notification data from mbuf; rc=%d", rc);
      return;
    }

    ESP_LOGD(TAG, "Forwarding notification: handle=%d len=%d",
             event->notify_rx.attr_handle, data_len);

    // Send notification to Home Assistant
    this->enqueue_api_event_(ApiEventType::NOTIFICATION, conn->address,
                 event->notify_rx.attr_handle, data.data(), data_len);
  } else {
    ESP_LOGV(TAG, "Notification with no data");
    this->enqueue_api_event_(ApiEventType::NOTIFICATION, conn->address,
                 event->notify_rx.attr_handle, nullptr, 0);
  }
#endif
}

void NimBLEProxy::handle_gap_passkey_action_(struct ble_gap_event *event, NimBLEConnection *conn) {
  ESP_LOGI(TAG, "Passkey action event; conn_handle=%d action=%d",
           event->passkey.conn_handle, event->passkey.params.action);

  // If conn is null, try to find it by handle
  if (conn == nullptr) {
    conn = this->get_connection_by_handle_(event->passkey.conn_handle);
  }

  if (conn == nullptr) {
    ESP_LOGW(TAG, "Passkey action for unknown connection handle %d",
             event->passkey.conn_handle);
    return;
  }

  switch (event->passkey.params.action) {
    case BLE_SM_IOACT_NONE:
      ESP_LOGI(TAG, "No passkey action required (Just Works pairing)");
      break;

    case BLE_SM_IOACT_OOB:
      ESP_LOGI(TAG, "Out-of-band pairing not supported - rejecting");
      // OOB pairing is not supported, so we don't inject any IO
      // The pairing will fail without our response
      break;

    case BLE_SM_IOACT_INPUT:
      ESP_LOGW(TAG, "Passkey input requested but no trusted input channel is configured; rejecting pairing");
      break;

    case BLE_SM_IOACT_DISP:
      ESP_LOGW(TAG, "Passkey display requested but no trusted display channel is configured; rejecting pairing");
      break;

    case BLE_SM_IOACT_NUMCMP:
      ESP_LOGW(TAG, "Numeric comparison requested but no trusted confirmation channel is configured; rejecting pairing");
      break;

    default:
      ESP_LOGW(TAG, "Unknown passkey action: %d", event->passkey.params.action);
      break;
  }
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

  // Try to load from cache first if caching is enabled
  if (conn->use_cache && this->load_service_cache_(conn)) {
    ESP_LOGI(TAG, "Loaded services from cache, skipping discovery");
    conn->state = NimBLEConnectionState::READY;
    // Send cached service data to Home Assistant
    this->send_service_response_(conn);
    return;
  }

  // Clear any previous discovery data
  conn->services.clear();
  conn->characteristics.clear();
  conn->descriptors.clear();
  conn->services.reserve(BLUETOOTH_PROXY_MAX_SERVICES);
  conn->characteristics.reserve(BLUETOOTH_PROXY_MAX_CHARACTERISTICS);
  conn->descriptors.reserve(BLUETOOTH_PROXY_MAX_DESCRIPTORS);
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

  ESP_LOGI(TAG, "Sending service discovery results: %d services, %d characteristics, %d descriptors",
           conn->services.size(), conn->characteristics.size(), conn->descriptors.size());

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
  if (!this->send_api_message(resp)) {
    ESP_LOGW(TAG, "Failed to send GATT services response");
  }

  // Send completion message
  send_gatt_services_done_response(this, conn->address);

  ESP_LOGI(TAG, "Service discovery complete for address=%012llX", conn->address);
#endif
}

uint16_t NimBLEProxy::find_cccd_handle_(NimBLEConnection *conn, uint16_t char_handle) {
  if (conn == nullptr) {
    return 0;
  }

  // CCCD UUID: 0x2902 (Client Characteristic Configuration Descriptor)
  const uint16_t CCCD_UUID = 0x2902;

  // Find the characteristic with this handle
  int char_idx = -1;
  for (size_t i = 0; i < conn->characteristics.size(); i++) {
    if (conn->characteristics[i].val_handle == char_handle) {
      char_idx = i;
      break;
    }
  }

  if (char_idx < 0) {
    ESP_LOGW(TAG, "Characteristic with handle %d not found", char_handle);
    return 0;
  }

  const auto &characteristic = conn->characteristics[char_idx];

  // Calculate the end handle for this characteristic
  uint16_t char_end_handle;
  if (char_idx + 1 < (int)conn->characteristics.size()) {
    char_end_handle = conn->characteristics[char_idx + 1].def_handle - 1;
  } else {
    // This is the last characteristic, find the service's end handle
    char_end_handle = 0xFFFF;
    for (const auto &svc : conn->services) {
      if (characteristic.def_handle >= svc.start_handle && characteristic.def_handle <= svc.end_handle) {
        char_end_handle = svc.end_handle;
        break;
      }
    }
  }

  // Search for CCCD descriptor within this characteristic's range
  for (const auto &descriptor : conn->descriptors) {
    if (descriptor.handle > characteristic.val_handle && descriptor.handle <= char_end_handle) {
      // Check if this is the CCCD descriptor
      if (descriptor.uuid.u.type == BLE_UUID_TYPE_16 && descriptor.uuid.u16.value == CCCD_UUID) {
        ESP_LOGD(TAG, "Found CCCD handle %d for characteristic handle %d", descriptor.handle, char_handle);
        return descriptor.handle;
      }
    }
  }

  ESP_LOGW(TAG, "CCCD not found for characteristic handle %d", char_handle);
  return 0;
}

//=============================================================================
// Service Cache Helper Functions (Phase 2.5)
//=============================================================================

std::string NimBLEProxy::get_cache_key_(uint64_t address) {
  char key[32];
  snprintf(key, sizeof(key), "ble_cache_%012llX", address);
  return std::string(key);
}

bool NimBLEProxy::save_service_cache_(NimBLEConnection *conn) {
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot save cache with null connection");
    return false;
  }

  if (!conn->discovery_complete) {
    ESP_LOGW(TAG, "Cannot save incomplete service discovery to cache");
    return false;
  }

  ESP_LOGI(TAG, "Saving service cache for address=%012llX (%d services, %d chars, %d descs)",
           conn->address, conn->services.size(), conn->characteristics.size(), conn->descriptors.size());

  // Build a serialized cache structure
  // Format: [num_services][service_data...][num_chars][char_data...][num_descs][desc_data...]

  // Calculate required size
  size_t cache_size = 0;
  cache_size += 2;  // num_services (uint16_t)
  cache_size += conn->services.size() * sizeof(ble_gatt_svc);
  cache_size += 2;  // num_characteristics (uint16_t)
  cache_size += conn->characteristics.size() * sizeof(ble_gatt_chr);
  cache_size += 2;  // num_descriptors (uint16_t)
  cache_size += conn->descriptors.size() * sizeof(ble_gatt_dsc);

  // Allocate buffer
  uint8_t *cache_buffer = new uint8_t[cache_size];
  uint8_t *ptr = cache_buffer;

  // Serialize services
  uint16_t num_services = conn->services.size();
  memcpy(ptr, &num_services, 2);
  ptr += 2;
  for (const auto &svc : conn->services) {
    memcpy(ptr, &svc, sizeof(ble_gatt_svc));
    ptr += sizeof(ble_gatt_svc);
  }

  // Serialize characteristics
  uint16_t num_chars = conn->characteristics.size();
  memcpy(ptr, &num_chars, 2);
  ptr += 2;
  for (const auto &chr : conn->characteristics) {
    memcpy(ptr, &chr, sizeof(ble_gatt_chr));
    ptr += sizeof(ble_gatt_chr);
  }

  // Serialize descriptors
  uint16_t num_descs = conn->descriptors.size();
  memcpy(ptr, &num_descs, 2);
  ptr += 2;
  for (const auto &dsc : conn->descriptors) {
    memcpy(ptr, &dsc, sizeof(ble_gatt_dsc));
    ptr += sizeof(ble_gatt_dsc);
  }

  // Save to NVS using ESPHome preferences
  // Use a struct wrapper to store the cache data
  struct CacheData {
    size_t size;
    uint8_t data[8192];  // Max 8KB cache
  };

  if (cache_size > sizeof(CacheData::data)) {
    ESP_LOGE(TAG, "Cache size (%d bytes) exceeds maximum (%d bytes)", cache_size, sizeof(CacheData::data));
    delete[] cache_buffer;
    return false;
  }

  CacheData cache_struct;
  cache_struct.size = cache_size;
  memcpy(cache_struct.data, cache_buffer, cache_size);

  delete[] cache_buffer;

  std::string cache_key = this->get_cache_key_(conn->address);
  auto pref = global_preferences->make_preference<CacheData>(fnv1_hash(cache_key));
  bool success = pref.save(&cache_struct);

  if (success) {
    ESP_LOGI(TAG, "Service cache saved successfully (%d bytes)", cache_size);
  } else {
    ESP_LOGE(TAG, "Failed to save service cache");
  }

  return success;
}

bool NimBLEProxy::load_service_cache_(NimBLEConnection *conn) {
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot load cache with null connection");
    return false;
  }

  if (!conn->use_cache) {
    ESP_LOGI(TAG, "Cache disabled for this connection");
    return false;
  }

  ESP_LOGI(TAG, "Loading service cache for address=%012llX", conn->address);

  // Try to load from NVS
  std::string cache_key = this->get_cache_key_(conn->address);

  struct CacheData {
    size_t size;
    uint8_t data[8192];  // Max 8KB cache
  };

  CacheData cache_struct;
  auto pref = global_preferences->make_preference<CacheData>(fnv1_hash(cache_key));

  if (!pref.load(&cache_struct)) {
    ESP_LOGI(TAG, "No cached service data found for address=%012llX", conn->address);
    return false;
  }

  // Validate cache size
  if (cache_struct.size == 0 || cache_struct.size > sizeof(cache_struct.data)) {
    ESP_LOGW(TAG, "Invalid cache size: %d bytes", cache_struct.size);
    return false;
  }

  // Deserialize the cache
  uint8_t *ptr = cache_struct.data;
  uint8_t *end = cache_struct.data + cache_struct.size;

  // Validate we have enough data for service count
  if (ptr + 2 > end) {
    ESP_LOGE(TAG, "Cache data truncated (service count)");
    return false;
  }

  // Load services
  uint16_t num_services;
  memcpy(&num_services, ptr, 2);
  ptr += 2;

  // Validate service data size
  if (ptr + (num_services * sizeof(ble_gatt_svc)) > end) {
    ESP_LOGE(TAG, "Cache data truncated (services)");
    return false;
  }

  conn->services.clear();
  for (uint16_t i = 0; i < num_services; i++) {
    ble_gatt_svc svc;
    memcpy(&svc, ptr, sizeof(ble_gatt_svc));
    ptr += sizeof(ble_gatt_svc);
    conn->services.push_back(svc);
  }

  // Validate we have enough data for characteristic count
  if (ptr + 2 > end) {
    ESP_LOGE(TAG, "Cache data truncated (characteristic count)");
    return false;
  }

  // Load characteristics
  uint16_t num_chars;
  memcpy(&num_chars, ptr, 2);
  ptr += 2;

  // Validate characteristic data size
  if (ptr + (num_chars * sizeof(ble_gatt_chr)) > end) {
    ESP_LOGE(TAG, "Cache data truncated (characteristics)");
    return false;
  }

  conn->characteristics.clear();
  for (uint16_t i = 0; i < num_chars; i++) {
    ble_gatt_chr chr;
    memcpy(&chr, ptr, sizeof(ble_gatt_chr));
    ptr += sizeof(ble_gatt_chr);
    conn->characteristics.push_back(chr);
  }

  // Validate we have enough data for descriptor count
  if (ptr + 2 > end) {
    ESP_LOGE(TAG, "Cache data truncated (descriptor count)");
    return false;
  }

  // Load descriptors
  uint16_t num_descs;
  memcpy(&num_descs, ptr, 2);
  ptr += 2;

  // Validate descriptor data size
  if (ptr + (num_descs * sizeof(ble_gatt_dsc)) > end) {
    ESP_LOGE(TAG, "Cache data truncated (descriptors)");
    return false;
  }

  conn->descriptors.clear();
  for (uint16_t i = 0; i < num_descs; i++) {
    ble_gatt_dsc dsc;
    memcpy(&dsc, ptr, sizeof(ble_gatt_dsc));
    ptr += sizeof(ble_gatt_dsc);
    conn->descriptors.push_back(dsc);
  }

  conn->discovery_complete = true;
  conn->cache_loaded = true;

  ESP_LOGI(TAG, "Service cache loaded successfully (%d services, %d chars, %d descs)",
           conn->services.size(), conn->characteristics.size(), conn->descriptors.size());

  return true;
}

bool NimBLEProxy::clear_service_cache_(uint64_t address) {
  ESP_LOGI(TAG, "Clearing service cache for address=%012llX", address);

  std::string cache_key = this->get_cache_key_(address);

  struct CacheData {
    size_t size;
    uint8_t data[8192];  // Max 8KB cache
  };

  auto pref = global_preferences->make_preference<CacheData>(fnv1_hash(cache_key));

  // ESPHome doesn't have a direct delete, but we can save empty data
  CacheData empty_cache;
  empty_cache.size = 0;
  bool success = pref.save(&empty_cache);

  if (success) {
    ESP_LOGI(TAG, "Service cache cleared successfully");
  } else {
    ESP_LOGE(TAG, "Failed to clear service cache");
  }

  return success;
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

  // Check for errors
  if (error->status != 0) {
    ESP_LOGE(TAG, "GATT read failed; conn_handle=%d status=%d", conn_handle, error->status);
    global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::ERROR_RESPONSE,
                                             conn->address, attr ? attr->handle : 0,
                                             nullptr, 0, error->status);
    return 0;
  }

  // Extract data from os_mbuf
  if (attr != nullptr && attr->om != nullptr) {
    // Get the data length
    uint16_t data_len = OS_MBUF_PKTLEN(attr->om);

    if (data_len > BLUETOOTH_PROXY_MAX_GATT_DATA) {
      ESP_LOGW(TAG, "Rejecting oversized GATT read: len=%u max=%u", data_len,
               BLUETOOTH_PROXY_MAX_GATT_DATA);
      global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::ERROR_RESPONSE,
                       conn->address, attr->handle, nullptr, 0,
                       BLE_HS_EMSGSIZE);
      return 0;
    }

    std::array<uint8_t, BLUETOOTH_PROXY_MAX_GATT_DATA> data{};

    // Copy data from mbuf chain
    int rc = os_mbuf_copydata(attr->om, 0, data_len, data.data());
    if (rc != 0) {
      ESP_LOGE(TAG, "Failed to copy data from mbuf; rc=%d", rc);
      global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::ERROR_RESPONSE,
                       conn->address, attr->handle, nullptr, 0, rc);
      return 0;
    }

    ESP_LOGI(TAG, "GATT read success; conn_handle=%d handle=%d len=%d",
             conn_handle, attr->handle, data_len);

    // Send read response
    global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::READ_RESPONSE,
                         conn->address, attr->handle, data.data(), data_len);
  } else {
    ESP_LOGW(TAG, "Read completed but no data available");
    global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::READ_RESPONSE,
                         conn->address, attr ? attr->handle : 0, nullptr, 0);
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

  // Check for errors
  if (error->status != 0) {
    ESP_LOGE(TAG, "GATT write failed; conn_handle=%d status=%d", conn_handle, error->status);
    global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::ERROR_RESPONSE,
                                             conn->address, attr ? attr->handle : 0,
                                             nullptr, 0, error->status);
    return 0;
  }

  ESP_LOGI(TAG, "GATT write success; conn_handle=%d handle=%d",
           conn_handle, attr ? attr->handle : 0);

  // For writes, we send an empty read response to indicate success
  // (ESPHome API uses BluetoothGATTReadResponse for write confirmations)
  global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::READ_RESPONSE,
                                           conn->address, attr ? attr->handle : 0, nullptr, 0);
#endif

  return 0;
}

int NimBLEProxy::on_subscribe_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr, void *arg) {
#ifdef USE_API
  if (global_nimble_proxy == nullptr) {
    ESP_LOGW(TAG, "global_nimble_proxy is null in on_subscribe_cb_");
    return 0;
  }

  // arg contains the connection pointer
  auto *conn = static_cast<NimBLEConnection *>(arg);
  if (conn == nullptr) {
    ESP_LOGW(TAG, "Connection pointer is null in on_subscribe_cb_");
    return 0;
  }

  // Check for errors
  if (error->status != 0) {
    ESP_LOGE(TAG, "CCCD write failed; conn_handle=%d status=%d", conn_handle, error->status);
    global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::ERROR_RESPONSE,
                                             conn->address, attr ? attr->handle : 0,
                                             nullptr, 0, error->status);
    return 0;
  }

  ESP_LOGI(TAG, "CCCD write success; conn_handle=%d handle=%d",
           conn_handle, attr ? attr->handle : 0);

  // For CCCD writes (subscribe/unsubscribe), we send an empty read response to indicate success
  // (ESPHome API uses BluetoothGATTReadResponse for write confirmations)
  global_nimble_proxy->enqueue_api_event_(NimBLEProxy::ApiEventType::READ_RESPONSE,
                                           conn->address, attr ? attr->handle : 0, nullptr, 0);
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

  if (conn->services.size() >= conn->services.capacity()) {
    ESP_LOGE(TAG, "Service discovery exceeded maximum of %u services", BLUETOOTH_PROXY_MAX_SERVICES);
    conn->state = NimBLEConnectionState::ERROR;
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

  if (conn->characteristics.size() >= conn->characteristics.capacity()) {
    ESP_LOGE(TAG, "Characteristic discovery exceeded maximum of %u characteristics",
             BLUETOOTH_PROXY_MAX_CHARACTERISTICS);
    conn->state = NimBLEConnectionState::ERROR;
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

        // Defer cache persistence and API response construction to loop().
        global_nimble_proxy->pending_service_cache_save_ = conn->use_cache && !conn->cache_loaded;
        global_nimble_proxy->pending_service_response_handle_ = conn->conn_handle;
      }
    } else {
      ESP_LOGE(TAG, "Descriptor discovery failed; status=%d", error->status);
      conn->state = NimBLEConnectionState::ERROR;
    }
    return 0;
  }

  if (conn->descriptors.size() >= conn->descriptors.capacity()) {
    ESP_LOGE(TAG, "Descriptor discovery exceeded maximum of %u descriptors", BLUETOOTH_PROXY_MAX_DESCRIPTORS);
    conn->state = NimBLEConnectionState::ERROR;
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
  // NOTE: esp_bt_controller_get_status() omitted - may fault before BT init
}

//=============================================================================
// Pairing/Bonding Helper Functions (Phase 2.6)
//=============================================================================

void NimBLEProxy::initiate_pairing_(NimBLEConnection *conn) {
  if (conn == nullptr) {
    ESP_LOGE(TAG, "Cannot initiate pairing with null connection");
    return;
  }

  if (conn->conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    ESP_LOGE(TAG, "Cannot initiate pairing with invalid connection handle");
    this->send_pairing_response_(conn->address, false, BLE_HS_ENOTCONN);
    return;
  }

  ESP_LOGI(TAG, "Initiating pairing for conn_handle=%d address=%012llX",
           conn->conn_handle, conn->address);

  // Set connection to PAIRING state
  conn->state = NimBLEConnectionState::PAIRING;
  conn->state_timestamp = millis();

  // Initiate security procedure
  // This will trigger the security manager to start pairing
  int rc = ble_gap_security_initiate(conn->conn_handle);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_security_initiate failed; rc=%d", rc);
    conn->state = NimBLEConnectionState::READY;  // Return to READY state
    this->send_pairing_response_(conn->address, false, rc);
    return;
  }

  ESP_LOGI(TAG, "Pairing initiated successfully");
}

void NimBLEProxy::delete_bond_(uint64_t address) {
  ESP_LOGI(TAG, "Deleting bond for address=%012llX", address);

  // Convert uint64_t address to ble_addr_t
  ble_addr_t peer_addr;
  peer_addr.type = BLE_ADDR_PUBLIC;  // Try public address first
  for (int i = 0; i < 6; i++) {
    peer_addr.val[i] = (address >> (i * 8)) & 0xFF;
  }

  // Delete the bond from NimBLE's bond store
  int rc = ble_store_util_delete_peer(&peer_addr);
  if (rc != 0) {
    // Try again with random address type
    peer_addr.type = BLE_ADDR_RANDOM;
    rc = ble_store_util_delete_peer(&peer_addr);
    if (rc != 0) {
      ESP_LOGW(TAG, "Failed to delete bond (may not exist); rc=%d", rc);
    } else {
      ESP_LOGI(TAG, "Bond deleted successfully (random address)");
    }
  } else {
    ESP_LOGI(TAG, "Bond deleted successfully (public address)");
  }
}

void NimBLEProxy::send_pairing_response_(uint64_t address, bool paired, int error) {
#ifdef USE_API
  // Use the same response structure as connection responses
  // Home Assistant treats pairing responses similarly to connection responses
  esphome::api::BluetoothDeviceConnectionResponse resp;
  resp.address = address;
  resp.connected = paired;  // Use connected field to indicate paired status
  resp.mtu = 23;  // Default MTU
  resp.error = error;

  if (!this->send_api_message(resp)) {
    ESP_LOGW(TAG, "Failed to send pairing response");
  } else {
    ESP_LOGI(TAG, "Sent pairing response: address=%012llX paired=%d error=%d",
             address, paired, error);
  }
#endif
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

      // Set cache mode based on request type
      if (msg.request_type == api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CONNECT_V3_WITHOUT_CACHE) {
        conn->use_cache = false;
        ESP_LOGI(TAG, "Cache disabled for this connection (CONNECT_V3_WITHOUT_CACHE)");
      } else {
        conn->use_cache = true;
        ESP_LOGI(TAG, "Cache enabled for this connection");
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

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_PAIR: {
      ESP_LOGI(TAG, "Pairing request for address=%012llX", msg.address);

      NimBLEConnection *conn = this->get_connection_(msg.address, false);
      if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE) {
        ESP_LOGE(TAG, "Connection not found for pairing");
        this->send_pairing_response_(msg.address, false, BLE_HS_ENOTCONN);
        return;
      }

      // Check if already paired/bonded
      if (conn->bonded && conn->encrypted) {
        ESP_LOGI(TAG, "Device already paired and bonded");
        this->send_pairing_response_(msg.address, true, 0);
        return;
      }

      // Initiate pairing
      this->initiate_pairing_(conn);
      break;
    }

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_UNPAIR: {
      ESP_LOGI(TAG, "Unpair request for address=%012llX", msg.address);

      // Delete the bond from NVS
      this->delete_bond_(msg.address);

      // If there's an active connection, mark it as unbonded
      NimBLEConnection *conn = this->get_connection_(msg.address, false);
      if (conn != nullptr && conn->state != NimBLEConnectionState::IDLE) {
        conn->bonded = false;
        conn->encrypted = false;
      }

      // Send success response
      this->send_pairing_response_(msg.address, false, 0);
      break;
    }

    case api::enums::BLUETOOTH_DEVICE_REQUEST_TYPE_CLEAR_CACHE:
      ESP_LOGI(TAG, "Clearing cache for address=%012llX", msg.address);
      this->clear_service_cache_(msg.address);
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
           msg.address, msg.handle, msg.data_len, msg.response);

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
                              msg.data, msg.data_len,
                              NimBLEProxy::on_write_cb_, conn);
  } else {
    // Write without response (ble_gattc_write_no_rsp_flat)
    rc = ble_gattc_write_no_rsp_flat(conn->conn_handle, msg.handle,
                                     msg.data, msg.data_len);

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
           msg.address, msg.handle, msg.data_len);

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
                                msg.data, msg.data_len,
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
  ESP_LOGI(TAG, "bluetooth_gatt_notify: address=%012llX handle=%d enable=%d",
           msg.address, msg.handle, msg.enable);

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
    ESP_LOGW(TAG, "Connection not ready for notification subscribe; state=%d", (int)conn->state);
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

  // Find the CCCD handle for this characteristic
  uint16_t cccd_handle = this->find_cccd_handle_(conn, msg.handle);
  if (cccd_handle == 0) {
    ESP_LOGE(TAG, "CCCD not found for characteristic handle %d", msg.handle);
#ifdef USE_API
    void *api_conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(this->api_connection_mutex_);
      api_conn = this->api_connection_;
    }
    if (api_conn != nullptr) {
      send_gatt_error(api_conn, msg.address, msg.handle, BLE_HS_ENOENT);
    }
#endif
    return;
  }

  // Prepare CCCD value (2 bytes)
  // Bit 0: Notifications enabled (0x0001)
  // Bit 1: Indications enabled (0x0002)
  uint8_t cccd_value[2];
  if (msg.enable) {
    // Enable notifications (0x0001)
    cccd_value[0] = 0x01;
    cccd_value[1] = 0x00;
  } else {
    // Disable notifications (0x0000)
    cccd_value[0] = 0x00;
    cccd_value[1] = 0x00;
  }

  ESP_LOGI(TAG, "Writing CCCD handle %d with value 0x%02X%02X for characteristic %d",
           cccd_handle, cccd_value[1], cccd_value[0], msg.handle);

  // Write to CCCD descriptor to enable/disable notifications
  int rc = ble_gattc_write_flat(conn->conn_handle, cccd_handle,
                                cccd_value, sizeof(cccd_value),
                                NimBLEProxy::on_subscribe_cb_, conn);
  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to write CCCD; rc=%d", rc);
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
    return;
  }

  // Track subscription state
  if (msg.enable) {
    conn->subscribed_handles.insert(msg.handle);
  } else {
    conn->subscribed_handles.erase(msg.handle);
  }

  ESP_LOGI(TAG, "Notification %s for handle %d (total subscriptions: %d)",
           msg.enable ? "enabled" : "disabled", msg.handle, conn->subscribed_handles.size());
}

template<typename T>
void NimBLEProxy::bluetooth_set_connection_params(const T &msg) {
  ESP_LOGI(TAG, "bluetooth_set_connection_params: address=%012llX min=%d max=%d latency=%d timeout=%d",
           msg.address, msg.min_interval, msg.max_interval, msg.latency, msg.timeout);

  // Find existing connection
  NimBLEConnection *conn = this->get_connection_(msg.address, false);
  if (conn == nullptr || conn->state == NimBLEConnectionState::IDLE ||
      conn->conn_handle == BLE_HS_CONN_HANDLE_NONE) {
    ESP_LOGW(TAG, "bluetooth_set_connection_params: no active connection for address=%012llX", msg.address);
    esphome::api::BluetoothSetConnectionParamsResponse resp;
    resp.address = msg.address;
    resp.error = BLE_HS_ENOTCONN;
    this->send_api_message(resp);
    return;
  }

  struct ble_gap_upd_params params;
  memset(&params, 0, sizeof(params));
  params.itvl_min = (uint16_t) msg.min_interval;
  params.itvl_max = (uint16_t) msg.max_interval;
  params.latency  = (uint16_t) msg.latency;
  params.supervision_timeout = (uint16_t) msg.timeout;
  params.min_ce_len = 0;
  params.max_ce_len = 0;

  int rc = ble_gap_update_params(conn->conn_handle, &params);

  esphome::api::BluetoothSetConnectionParamsResponse resp;
  resp.address = msg.address;
  resp.error = (rc == 0) ? 0 : rc;
  this->send_api_message(resp);

  if (rc != 0) {
    ESP_LOGW(TAG, "ble_gap_update_params failed; rc=%d", rc);
  } else {
    ESP_LOGI(TAG, "Connection params update initiated for address=%012llX", msg.address);
  }
}

// Explicit template instantiations for all API message types
template void NimBLEProxy::bluetooth_device_request(const api::BluetoothDeviceRequest &msg);
template void NimBLEProxy::bluetooth_gatt_read(const api::BluetoothGATTReadRequest &msg);
template void NimBLEProxy::bluetooth_gatt_write(const api::BluetoothGATTWriteRequest &msg);
template void NimBLEProxy::bluetooth_gatt_read_descriptor(const api::BluetoothGATTReadDescriptorRequest &msg);
template void NimBLEProxy::bluetooth_gatt_write_descriptor(const api::BluetoothGATTWriteDescriptorRequest &msg);
template void NimBLEProxy::bluetooth_gatt_send_services(const api::BluetoothGATTGetServicesRequest &msg);
template void NimBLEProxy::bluetooth_gatt_notify(const api::BluetoothGATTNotifyRequest &msg);
template void NimBLEProxy::bluetooth_set_connection_params(const api::BluetoothSetConnectionParamsRequest &msg);
#endif

}  // namespace nimble_proxy
}  // namespace esphome
