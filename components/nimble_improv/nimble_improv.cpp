#include "nimble_improv.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"

#include "esp_chip_info.h"
#include "sdkconfig.h"

// NimBLE's modlog defines LOG_LEVEL_* macros that collide with ESPHome names.
#undef LOG_LEVEL_NONE
#undef LOG_LEVEL_ERROR
#undef LOG_LEVEL_WARN
#undef LOG_LEVEL_INFO
#undef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_VERBOSE

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#ifdef USE_PROVISIONING
#include "esphome/components/provisioning/provisioning.h"
#endif

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <string>

namespace esphome::nimble_improv {

static const char *const TAG = "nimble_improv";

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
NimBLEImprov *global_nimble_improv = nullptr;

// esp32_improv constants (ESPHome 2026.9.0).
static constexpr char ESPHOME_MY_LINK[] = "https://my.home-assistant.io/redirect/config_flow_start?domain=esphome";
static constexpr size_t RPC_RESPONSE_OVERHEAD = 3;
static constexpr size_t MAX_NEXT_URL_LEN =
    improv::RPC_RESPONSE_MAX_SIZE - RPC_RESPONSE_OVERHEAD - 1 - sizeof(ESPHOME_MY_LINK);
static constexpr uint32_t STOP_ADVERTISING_DELAY = 10000;
static constexpr uint32_t NAME_ADVERTISING_INTERVAL = 60000;
static constexpr uint32_t NAME_ADVERTISING_DURATION = 1000;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT = 30000;
static constexpr uint8_t IMPROV_SERVICE_DATA_SIZE = 8;
static constexpr uint8_t IMPROV_PROTOCOL_ID_1 = 0x77;
static constexpr uint8_t IMPROV_PROTOCOL_ID_2 = 0x46;
static constexpr size_t MAX_INCOMING_BYTES = 512;

// Credentials the previous nimble_improv kept in its own preference slot; moved
// into the WiFi component's store once so an already provisioned unit keeps its
// network (esp32_improv uses wifi::save_wifi_sta()).
static constexpr uint32_t LEGACY_PREF_ID_WIFI = 0x9B5A7C21;
struct LegacyStoredWiFi {
  char ssid[33];
  char password[65];
  uint8_t version;
  uint8_t pad[3];
};

// ---- GATT table (registered with nimble_base before the host starts) ----

// Improv UUIDs, little-endian (00467768-6228-2272-4663-2774782680xx).
#define IMPROV_UUID(last) \
  BLE_UUID128_INIT((last), 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00)
static const ble_uuid128_t IMPROV_SERVICE_UUID = IMPROV_UUID(0x00);
static const ble_uuid128_t IMPROV_STATUS_UUID = IMPROV_UUID(0x01);
static const ble_uuid128_t IMPROV_ERROR_UUID = IMPROV_UUID(0x02);
static const ble_uuid128_t IMPROV_RPC_COMMAND_UUID = IMPROV_UUID(0x03);
static const ble_uuid128_t IMPROV_RPC_RESULT_UUID = IMPROV_UUID(0x04);
static const ble_uuid128_t IMPROV_CAPABILITIES_UUID = IMPROV_UUID(0x05);
#undef IMPROV_UUID

// Device Information Service: not in esp32_improv, kept from the previous
// nimble_improv (read-only, harmless for Improv clients).
static const ble_uuid16_t DIS_UUID = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t DIS_MODEL_UUID = BLE_UUID16_INIT(0x2A24);
static const ble_uuid16_t DIS_SERIAL_UUID = BLE_UUID16_INIT(0x2A25);
static const ble_uuid16_t DIS_FIRMWARE_UUID = BLE_UUID16_INIT(0x2A26);
static const ble_uuid16_t DIS_HARDWARE_UUID = BLE_UUID16_INIT(0x2A27);
static const ble_uuid16_t DIS_SOFTWARE_UUID = BLE_UUID16_INIT(0x2A28);
static const ble_uuid16_t DIS_MANUFACTURER_UUID = BLE_UUID16_INIT(0x2A29);

enum AttrId : uintptr_t {
  ATTR_STATUS = 1,
  ATTR_ERROR,
  ATTR_RPC_COMMAND,
  ATTR_RPC_RESULT,
  ATTR_CAPABILITIES,
  ATTR_DIS_MODEL,
  ATTR_DIS_SERIAL,
  ATTR_DIS_HARDWARE,
  ATTR_DIS_FIRMWARE,
  ATTR_DIS_SOFTWARE,
  ATTR_DIS_MANUFACTURER,
};

static uint16_t status_handle = 0;
static uint16_t error_handle = 0;
static uint16_t rpc_result_handle = 0;

#define ATTR_ARG(id) reinterpret_cast<void *>(static_cast<uintptr_t>(id))

static const struct ble_gatt_chr_def IMPROV_CHARACTERISTICS[] = {
    {.uuid = &IMPROV_STATUS_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_STATUS),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &status_handle},
    {.uuid = &IMPROV_ERROR_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_ERROR),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &error_handle},
    {.uuid = &IMPROV_RPC_COMMAND_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_RPC_COMMAND),
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = &IMPROV_RPC_RESULT_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_RPC_RESULT),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &rpc_result_handle},
    {.uuid = &IMPROV_CAPABILITIES_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_CAPABILITIES),
     .flags = BLE_GATT_CHR_F_READ},
    {},
};

static const struct ble_gatt_chr_def DIS_CHARACTERISTICS[] = {
    {.uuid = &DIS_MANUFACTURER_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_DIS_MANUFACTURER),
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &DIS_MODEL_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_DIS_MODEL),
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &DIS_SERIAL_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_DIS_SERIAL),
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &DIS_HARDWARE_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_DIS_HARDWARE),
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &DIS_FIRMWARE_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_DIS_FIRMWARE),
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &DIS_SOFTWARE_UUID.u,
     .access_cb = NimBLEImprov::gatt_access_,
     .arg = ATTR_ARG(ATTR_DIS_SOFTWARE),
     .flags = BLE_GATT_CHR_F_READ},
    {},
};

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &DIS_UUID.u, .characteristics = DIS_CHARACTERISTICS},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &IMPROV_SERVICE_UUID.u, .characteristics = IMPROV_CHARACTERISTICS},
    {},
};

#undef ATTR_ARG

// ---- lifecycle ----

NimBLEImprov::NimBLEImprov() {
  global_nimble_improv = this;
  // NimBLE only accepts services before the host starts (nimble_base::setup()).
  nimble_base::NimBLEBase::register_gatt_services(GATT_SERVICES);
}

void NimBLEImprov::setup() {
#ifdef USE_BINARY_SENSOR
  if (this->authorizer_ != nullptr) {
    this->authorizer_->add_on_state_callback([this](bool state) {
      if (state) {
        this->authorized_start_ = millis();
        this->identify_start_ = 0;
      }
    });
  }
#endif
  nimble_base::global_nimble_base->add_listener(this);
  // The WiFi component's "last connected" clock starts at boot too.
  this->last_wifi_connected_ = millis();
}

void NimBLEImprov::migrate_legacy_credentials_() {
  auto pref = global_preferences->make_preference<LegacyStoredWiFi>(LEGACY_PREF_ID_WIFI);
  LegacyStoredWiFi rec{};
  if (!pref.load(&rec) || rec.version != 1 || rec.ssid[0] == '\0')
    return;
  rec.ssid[sizeof(rec.ssid) - 1] = '\0';
  rec.password[sizeof(rec.password) - 1] = '\0';
  ESP_LOGI(TAG, "Moving previously provisioned network '%s' into the WiFi component", rec.ssid);
  wifi::global_wifi_component->save_wifi_sta(rec.ssid, rec.password);
  LegacyStoredWiFi cleared{};
  pref.save(&cleared);
}

void NimBLEImprov::dump_config() {
  ESP_LOGCONFIG(TAG, "NimBLE Improv:");
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Authorizer", this->authorizer_);
#endif
#ifdef USE_OUTPUT
  ESP_LOGCONFIG(TAG, "  Status Indicator: '%s'", YESNO(this->status_indicator_ != nullptr));
#endif
  ESP_LOGCONFIG(TAG,
                "  Authorized duration: %" PRIu32 " ms\n"
                "  WiFi timeout: %" PRIu32 " ms",
                this->authorized_duration_, this->wifi_timeout_);
}

void NimBLEImprov::on_nimble_synced() {
  this->service_running_ = true;
  this->advertising_ = false;
}

void NimBLEImprov::on_nimble_reset(int reason) {
  this->service_running_ = false;
  this->advertising_ = false;
  this->conn_handle_.store(BLE_HS_CONN_HANDLE_NONE);
}

void NimBLEImprov::on_nimble_disabling() {
  this->stop_advertising_();
  this->service_running_ = false;
  this->conn_handle_.store(BLE_HS_CONN_HANDLE_NONE);
}

void NimBLEImprov::start() {
  if (this->should_start_ || this->state_ != improv::STATE_STOPPED)
    return;
#ifdef USE_PROVISIONING
  if (provisioning::global_provisioning_manager != nullptr && provisioning::global_provisioning_manager->closed()) {
    ESP_LOGD(TAG, "Provisioning window closed; not starting Improv");
    return;
  }
#endif
  ESP_LOGD(TAG, "Setting Improv to start");
  this->should_start_ = true;
  this->stop_at_ = 0;
}

void NimBLEImprov::stop() {
  this->should_start_ = false;
  // Let connected clients read the final state before the service goes away
  // (esp32_improv's "end-service" delay).
  if (this->stop_at_ == 0)
    this->stop_at_ = millis() + STOP_ADVERTISING_DELAY;
}

// ---- main loop ----

void NimBLEImprov::watch_wifi_() {
  // esp32_improv is started by the WiFi component (no stored network at setup,
  // or no connection for wifi_timeout) and there is no equivalent hook for
  // other components, so the same two triggers are applied here.
  auto *wifi = wifi::global_wifi_component;
  if (wifi == nullptr)
    return;
  const uint32_t now = millis();
  if (wifi->is_connected())
    this->last_wifi_connected_ = now;
  if (this->is_active() || this->should_start_ || this->stop_at_ != 0)
    return;
  if (!wifi->has_sta() || (!wifi->is_connected() && now - this->last_wifi_connected_ > this->wifi_timeout_)) {
    this->start();
  }
}

void NimBLEImprov::loop() {
  if (!this->legacy_checked_) {
    // Not in setup(): save_wifi_sta() writes through a preference the WiFi
    // component only creates in its own setup(), which runs after this one.
    this->legacy_checked_ = true;
    this->migrate_legacy_credentials_();
  }
  this->watch_wifi_();
  const uint32_t now = millis();

  if (this->stop_at_ != 0 && static_cast<int32_t>(now - this->stop_at_) >= 0) {
    this->stop_at_ = 0;
    if (this->state_ != improv::STATE_STOPPED) {
      this->stop_advertising_();
      this->set_state_(improv::STATE_STOPPED);
    }
  }

  if (!this->service_running_ || !nimble_base::global_nimble_base->is_active()) {
    if (this->state_ != improv::STATE_STOPPED) {
      this->state_ = improv::STATE_STOPPED;
      this->state_byte_.store(this->state_);
#ifdef USE_NIMBLE_IMPROV_STATE_CALLBACK
      this->state_callback_.call(this->state_, this->error_state_);
#endif
    }
    this->advertising_ = false;
    std::lock_guard<std::mutex> lock(this->incoming_mutex_);
    this->incoming_data_.clear();
    return;
  }

  if (this->disconnected_.exchange(false)) {
    // esp32_improv clears the error when a client disconnects.
    this->set_error_(improv::ERROR_NONE);
  }

  {
    std::lock_guard<std::mutex> lock(this->incoming_mutex_);
    if (this->state_ == improv::STATE_STOPPED) {
      this->incoming_data_.clear();
    } else if (!this->incoming_data_.empty()) {
      this->pending_data_.insert(this->pending_data_.end(), this->incoming_data_.begin(), this->incoming_data_.end());
      this->incoming_data_.clear();
    }
  }
  if (!this->pending_data_.empty())
    this->process_incoming_data_();

  if (this->wifi_connect_deadline_ != 0 && static_cast<int32_t>(now - this->wifi_connect_deadline_) >= 0) {
    this->wifi_connect_deadline_ = 0;
    this->on_wifi_connect_timeout_();
  }

  if (this->state_ != improv::STATE_STOPPED && this->state_ != improv::STATE_PROVISIONED) {
    this->update_advertising_type_();
    // A connection stops advertising; resume once the client has gone.
    if (this->conn_handle_.load() == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active())
      this->start_advertising_();
  }

  switch (this->state_) {
    case improv::STATE_STOPPED:
      this->set_status_indicator_state_(false);
      if (this->should_start_) {
        // Device name first, before any state (esp32_improv parity).
        this->advertising_device_name_ = true;
        this->last_name_adv_time_ = now;
        this->set_state_(this->get_initial_state_(), false);
        this->apply_advertising_();
        this->start_advertising_();
        this->set_error_(improv::ERROR_NONE);
        this->should_start_ = false;
        ESP_LOGD(TAG, "Service started!");
      }
      break;
    case improv::STATE_AWAITING_AUTHORIZATION: {
#ifdef USE_BINARY_SENSOR
      if (this->authorizer_ == nullptr ||
          (this->authorized_start_ != 0 && ((now - this->authorized_start_) < this->authorized_duration_))) {
        this->set_state_(improv::STATE_AUTHORIZED);
      } else if (!this->check_identify_()) {
        this->set_status_indicator_state_(true);
      }
#else
      this->set_state_(improv::STATE_AUTHORIZED);
#endif
      this->check_wifi_connection_();
      break;
    }
    case improv::STATE_AUTHORIZED: {
#ifdef USE_BINARY_SENSOR
      if (this->authorizer_ != nullptr && now - this->authorized_start_ > this->authorized_duration_) {
        ESP_LOGD(TAG, "Authorization timeout");
        this->set_state_(improv::STATE_AWAITING_AUTHORIZATION);
        return;
      }
#endif
      if (!this->check_identify_())
        this->set_status_indicator_state_((now % 1000) < 500);
      this->check_wifi_connection_();
      break;
    }
    case improv::STATE_PROVISIONING:
      this->set_status_indicator_state_((now % 200) < 100);
      this->check_wifi_connection_();
      break;
    case improv::STATE_PROVISIONED: {
      std::lock_guard<std::mutex> lock(this->incoming_mutex_);
      this->incoming_data_.clear();
      this->pending_data_.clear();
      this->set_status_indicator_state_(false);
      break;
    }
  }
}

void NimBLEImprov::set_status_indicator_state_(bool state) {
#ifdef USE_OUTPUT
  if (this->status_indicator_ == nullptr || this->status_indicator_state_ == state)
    return;
  this->status_indicator_state_ = state;
  if (state) {
    this->status_indicator_->turn_on();
  } else {
    this->status_indicator_->turn_off();
  }
#endif
}

bool NimBLEImprov::check_identify_() {
  const uint32_t now = millis();
  bool identify = this->identify_start_ != 0 && now - this->identify_start_ <= this->identify_duration_;
  if (identify) {
    uint32_t time = now % 1000;
    this->set_status_indicator_state_(time < 600 && time % 200 < 100);
  }
  return identify;
}

uint8_t NimBLEImprov::capabilities_() const {
  uint8_t capabilities = 0x00;
#ifdef USE_OUTPUT
  // Identify is only offered when there is something to identify with.
  if (this->status_indicator_ != nullptr)
    capabilities |= improv::CAPABILITY_IDENTIFY;
#endif
  return capabilities;
}

improv::State NimBLEImprov::get_initial_state_() const {
#ifdef USE_BINARY_SENSOR
  return this->authorizer_ == nullptr ? improv::STATE_AUTHORIZED : improv::STATE_AWAITING_AUTHORIZATION;
#else
  return improv::STATE_AUTHORIZED;
#endif
}

void NimBLEImprov::set_state_(improv::State state, bool update_advertising) {
  if (this->state_ == state)
    return;
  ESP_LOGD(TAG, "State transition: 0x%02X -> 0x%02X", this->state_, state);
  this->state_ = state;
  this->state_byte_.store(state);
  if (state != improv::STATE_STOPPED && this->service_running_ && status_handle != 0) {
    // Notifies subscribed clients; the value is read back through gatt_access_.
    ble_gatts_chr_updated(status_handle);
  }
  // STATE_STOPPED is internal and never advertised (esp32_improv parity).
  if (state != improv::STATE_STOPPED && update_advertising) {
    this->advertising_device_name_ = false;
    this->last_name_adv_time_ = millis();
    this->apply_advertising_();
  }
#ifdef USE_NIMBLE_IMPROV_STATE_CALLBACK
  this->state_callback_.call(this->state_, this->error_state_);
#endif
}

void NimBLEImprov::set_error_(improv::Error error) {
  if (error != improv::ERROR_NONE) {
    ESP_LOGE(TAG, "Error: %d", error);
  }
  if (this->error_state_ == error)
    return;
  this->error_state_ = error;
  this->error_byte_.store(error);
  if (this->state_ != improv::STATE_STOPPED && this->service_running_ && error_handle != 0) {
    ble_gatts_chr_updated(error_handle);
  }
}

void NimBLEImprov::send_response_(std::span<const uint8_t> response) {
  {
    std::lock_guard<std::mutex> lock(this->response_mutex_);
    this->response_len_ = std::min<size_t>(response.size(), this->response_.size());
    memcpy(this->response_.data(), response.data(), this->response_len_);
  }
  if (this->state_ != improv::STATE_STOPPED && this->service_running_ && rpc_result_handle != 0) {
    ble_gatts_chr_updated(rpc_result_handle);
  }
}

void NimBLEImprov::process_incoming_data_() {
  auto &data = this->pending_data_;
  if (data.size() < 3)
    return;
  uint8_t length = data[1];
  if (data.size() - 3 == length) {
    this->set_error_(improv::ERROR_NONE);
    improv::ImprovCommand command = improv::parse_improv_data(data);
    data.clear();
    switch (command.command) {
      case improv::BAD_CHECKSUM:
        ESP_LOGW(TAG, "Error decoding Improv payload");
        this->set_error_(improv::ERROR_INVALID_RPC);
        break;
      case improv::WIFI_SETTINGS: {
        if (this->state_ != improv::STATE_AUTHORIZED) {
          ESP_LOGW(TAG, "Settings received, but not authorized");
          this->set_error_(improv::ERROR_NOT_AUTHORIZED);
          return;
        }
#ifdef USE_PROVISIONING
        if (provisioning::global_provisioning_manager != nullptr &&
            provisioning::global_provisioning_manager->closed()) {
          ESP_LOGW(TAG, "Provisioning window closed; refusing settings");
          this->set_error_(improv::ERROR_NOT_AUTHORIZED);
          return;
        }
#endif
        if (wifi::global_wifi_component->is_disabled()) {
          ESP_LOGW(TAG, "Wi-Fi is disabled; cannot provision");
          this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
          return;
        }
        wifi::WiFiAP sta{};
        sta.set_ssid(command.ssid.c_str());
        sta.set_password(command.password.c_str());
        this->connecting_sta_ = sta;
        wifi::global_wifi_component->set_sta(sta);
        wifi::global_wifi_component->start_connecting(sta);
        this->set_state_(improv::STATE_PROVISIONING);
        ESP_LOGD(TAG, "Received Improv Wi-Fi settings ssid=%s, password=" LOG_SECRET("%s"), command.ssid.c_str(),
                 command.password.c_str());
        this->wifi_connect_deadline_ = millis() + WIFI_CONNECT_TIMEOUT;
        break;
      }
      case improv::IDENTIFY:
        this->identify_start_ = millis();
        break;
      default:
        ESP_LOGW(TAG, "Unknown Improv payload");
        this->set_error_(improv::ERROR_UNKNOWN_RPC);
        break;
    }
  } else if (data.size() - 2 > length) {
    ESP_LOGV(TAG, "Too much data received or data malformed; resetting buffer");
    data.clear();
  } else {
    ESP_LOGV(TAG, "Waiting for split data packets");
  }
}

void NimBLEImprov::on_wifi_connect_timeout_() {
  this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
  this->set_state_(improv::STATE_AUTHORIZED);
#ifdef USE_BINARY_SENSOR
  if (this->authorizer_ != nullptr)
    this->authorized_start_ = millis();
#endif
  ESP_LOGW(TAG, "Timed out while connecting to Wi-Fi network");
  wifi::global_wifi_component->clear_sta();
}

void NimBLEImprov::check_wifi_connection_() {
  if (!wifi::global_wifi_component->is_connected())
    return;

  if (this->state_ == improv::STATE_PROVISIONING) {
    wifi::global_wifi_component->save_wifi_sta(this->connecting_sta_.get_ssid(), this->connecting_sta_.get_password());
    this->connecting_sta_ = {};
    this->wifi_connect_deadline_ = 0;

    std::array<uint8_t, improv::RPC_RESPONSE_MAX_SIZE> buf;
    improv::RpcResponseBuilder builder(buf, improv::WIFI_SETTINGS);
#ifdef USE_IMPROV_NEXT_URL
    // improv_base emits this define when any Improv transport sets next_url:
    // (ESPHome 2026.10.0; see __init__.py).
    this->add_next_url_(builder, MAX_NEXT_URL_LEN);
#endif
    builder.add_string(ESPHOME_MY_LINK, sizeof(ESPHOME_MY_LINK) - 1);
#ifdef USE_WEBSERVER
    for (auto &ip : wifi::global_wifi_component->wifi_sta_ip_addresses()) {
      if (ip.is_ip4()) {
        char ip_buf[network::IP_ADDRESS_BUFFER_SIZE];
        ip.str_to(ip_buf);
        char webserver_url[7 + network::IP_ADDRESS_BUFFER_SIZE + 1 + 5 + 1];
        size_t len =
            buf_append_printf(webserver_url, sizeof(webserver_url), 0, "http://%s:%u", ip_buf, USE_WEBSERVER_PORT);
        if (!builder.add_string(webserver_url, len)) {
          ESP_LOGW(TAG, "Response full; URL dropped");
        }
        break;
      }
    }
#endif
    this->send_response_(builder.finish());
  } else if (this->is_active() && this->state_ != improv::STATE_PROVISIONED) {
    ESP_LOGD(TAG, "WiFi provisioned externally");
  }

  this->set_state_(improv::STATE_PROVISIONED);
  this->stop();
}

// ---- advertising ----

void NimBLEImprov::update_advertising_type_() {
  const uint32_t now = millis();
  if (this->advertising_device_name_) {
    if (now - this->last_name_adv_time_ >= NAME_ADVERTISING_DURATION) {
      this->advertising_device_name_ = false;
      this->apply_advertising_();
    }
    return;
  }
  if (now - this->last_name_adv_time_ >= NAME_ADVERTISING_INTERVAL) {
    this->advertising_device_name_ = true;
    this->last_name_adv_time_ = now;
    this->apply_advertising_();
  }
}

void NimBLEImprov::apply_advertising_() {
  if (!this->service_running_)
    return;
  // Advertisement: flags + the Improv service UUID + either the Improv service
  // data (state, capabilities) or, for one second a minute, the device name -
  // the payload esp32_ble builds for esp32_improv. 3 + 18 + 10 = 31 bytes.
  struct ble_hs_adv_fields fields {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &IMPROV_SERVICE_UUID;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;

  const char *name = ble_svc_gap_device_name();
  const size_t name_len = strlen(name);
  uint8_t service_data[IMPROV_SERVICE_DATA_SIZE] = {};
  if (this->advertising_device_name_) {
    // Only 8 bytes remain next to the flags and 128-bit UUID.
    fields.name = reinterpret_cast<const uint8_t *>(name);
    fields.name_len = std::min<size_t>(name_len, 8);
    fields.name_is_complete = name_len <= 8 ? 1 : 0;
  } else {
    service_data[0] = IMPROV_PROTOCOL_ID_1;
    service_data[1] = IMPROV_PROTOCOL_ID_2;
    service_data[2] = static_cast<uint8_t>(this->state_);
    service_data[3] = this->capabilities_();
    fields.svc_data_uuid16 = service_data;
    fields.svc_data_uuid16_len = IMPROV_SERVICE_DATA_SIZE;
  }
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGW(TAG, "Setting advertising data failed: %d", rc);
  }

  // Scan response: the full name, so active scanners always see it.
  struct ble_hs_adv_fields rsp {};
  rsp.name = reinterpret_cast<const uint8_t *>(name);
  rsp.name_len = std::min<size_t>(name_len, BLE_HS_ADV_MAX_SZ - 2);
  rsp.name_is_complete = name_len <= BLE_HS_ADV_MAX_SZ - 2 ? 1 : 0;
  rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc != 0) {
    ESP_LOGW(TAG, "Setting scan response failed: %d", rc);
  }
}

void NimBLEImprov::start_advertising_() {
  if (!this->service_running_ || ble_gap_adv_active())
    return;
  this->apply_advertising_();
  struct ble_gap_adv_params params {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &params, NimBLEImprov::gap_event_, this);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "Starting advertising failed: %d", rc);
    return;
  }
  this->advertising_ = true;
}

void NimBLEImprov::stop_advertising_() {
  if (ble_gap_adv_active())
    ble_gap_adv_stop();
  this->advertising_ = false;
}

// ---- host task ----

int NimBLEImprov::gap_event_(struct ble_gap_event *event, void *arg) {
  auto *self = static_cast<NimBLEImprov *>(arg);
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0)
        self->conn_handle_.store(event->connect.conn_handle);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      self->conn_handle_.store(BLE_HS_CONN_HANDLE_NONE);
      self->disconnected_.store(true);
      break;
    default:
      break;
  }
  return 0;
}

static int append_string(struct os_mbuf *om, const char *value) {
  return os_mbuf_append(om, value, strlen(value)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

int NimBLEImprov::gatt_access_(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt,
                               void *arg) {
  auto *self = global_nimble_improv;
  if (self == nullptr)
    return BLE_ATT_ERR_UNLIKELY;
  const auto id = static_cast<AttrId>(reinterpret_cast<uintptr_t>(arg));

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    if (id != ATTR_RPC_COMMAND)
      return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > MAX_INCOMING_BYTES)
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    std::lock_guard<std::mutex> lock(self->incoming_mutex_);
    if (self->incoming_data_.size() + len > MAX_INCOMING_BYTES)
      self->incoming_data_.clear();  // malformed stream; the loop resyncs on the next frame
    // Copy straight from the mbuf: no staging buffer on the host task's stack.
    const size_t offset = self->incoming_data_.size();
    self->incoming_data_.resize(offset + len);
    if (os_mbuf_copydata(ctxt->om, 0, len, self->incoming_data_.data() + offset) != 0) {
      self->incoming_data_.resize(offset);
      return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
  }

  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
    return BLE_ATT_ERR_UNLIKELY;

  switch (id) {
    case ATTR_STATUS: {
      uint8_t value = self->state_byte_.load();
      return os_mbuf_append(ctxt->om, &value, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case ATTR_ERROR: {
      uint8_t value = self->error_byte_.load();
      return os_mbuf_append(ctxt->om, &value, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case ATTR_RPC_RESULT: {
      std::lock_guard<std::mutex> lock(self->response_mutex_);
      return os_mbuf_append(ctxt->om, self->response_.data(), self->response_len_) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case ATTR_CAPABILITIES: {
      uint8_t value = self->capabilities_();
      return os_mbuf_append(ctxt->om, &value, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case ATTR_DIS_MANUFACTURER:
#ifdef ESPHOME_PROJECT_NAME
      return append_string(ctxt->om, ESPHOME_PROJECT_NAME);
#else
      return append_string(ctxt->om, "ESPHome");
#endif
    case ATTR_DIS_MODEL:
      return append_string(ctxt->om, App.get_friendly_name().empty() ? App.get_name().c_str()
                                                                      : App.get_friendly_name().c_str());
    case ATTR_DIS_SERIAL:
      return append_string(ctxt->om, App.get_name().c_str());
    case ATTR_DIS_HARDWARE: {
      esp_chip_info_t chip;
      esp_chip_info(&chip);
      char hardware[48];
      snprintf(hardware, sizeof(hardware), "%s rev %u.%u (%u cores)", CONFIG_IDF_TARGET, chip.revision / 100,
               chip.revision % 100, chip.cores);
      return append_string(ctxt->om, hardware);
    }
    case ATTR_DIS_FIRMWARE: {
      char build_time[Application::BUILD_TIME_STR_SIZE];
      App.get_build_time_string(build_time);
      return append_string(ctxt->om, build_time);
    }
    case ATTR_DIS_SOFTWARE:
#ifdef ESPHOME_PROJECT_VERSION
      return append_string(ctxt->om, ESPHOME_PROJECT_VERSION);
#else
      return append_string(ctxt->om, "ESPHome " ESPHOME_VERSION);
#endif
    default:
      return BLE_ATT_ERR_UNLIKELY;
  }
}

}  // namespace esphome::nimble_improv
