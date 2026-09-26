// [nimble] Not part of upstream ESPHome; added by truffshuff/esphome-components.
// See bluetooth_connection_nimble.h.

#include "bluetooth_connection_nimble.h"

#include "bluetooth_connection.h"

#if defined(USE_NIMBLE_BLE) && defined(USE_BLE_GATT_CLIENT)

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"

// NimBLE's modlog defines LOG_LEVEL_* macros that collide with ESPHome names.
#undef LOG_LEVEL_NONE
#undef LOG_LEVEL_ERROR
#undef LOG_LEVEL_WARN
#undef LOG_LEVEL_INFO
#undef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_VERBOSE

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>

namespace esphome::bluetooth_connection {

static const char *const TAG = "bluetooth_connection";

using ble_device_base::ESPBTUUID;
using ble_device_base::FAST_CONN_TIMEOUT;
using ble_device_base::FAST_MAX_CONN_INTERVAL;
using ble_device_base::FAST_MIN_CONN_INTERVAL;
using ble_device_base::GATT_ERR_NO_MEMORY;
using ble_device_base::GATT_ERR_NOT_CONNECTED;
using ble_device_base::MEDIUM_CONN_TIMEOUT;
using ble_device_base::MEDIUM_MAX_CONN_INTERVAL;
using ble_device_base::MEDIUM_MIN_CONN_INTERVAL;

// ---- error domain ----
//
// The API forwards these integers to Home Assistant untranslated, and the
// client side (aioesphomeapi / bleak-esphome) interprets them in Bluedroid's
// code space: esp_gatt_status_t for GATT operations (ATT codes 0x01-0x11 plus
// the 0x8x stack codes), HCI reasons for connection events, and -1 for "not
// connected" (bleak-esphome marks the device disconnected on exactly -1).
// NimBLE reports BLE_HS_* codes with ATT/HCI/SM errors offset by 0x100/0x200/
// 0x400/0x500, so everything is mapped at the backend boundary.
static constexpr int GATT_STATUS_INVALID_ATTR_LEN = 0x0D;  // ESP_GATT_INVALID_ATTR_LEN
static constexpr int GATT_STATUS_INSUF_RESOURCE = 0x11;    // ESP_GATT_INSUF_RESOURCE
static constexpr int GATT_STATUS_BUSY = 0x84;              // ESP_GATT_BUSY
static constexpr int GATT_STATUS_ERROR = 0x85;             // ESP_GATT_ERROR
static constexpr int GATT_STATUS_ILLEGAL_PARAMETER = 0x87; // ESP_GATT_ILLEGAL_PARAMETER
static constexpr int ATT_ERR_INVALID_OFFSET = 0x07;
static constexpr int ATT_ERR_ATTR_NOT_LONG = 0x0B;

// HCI reasons used for synthesized connection events.
static constexpr int HCI_REASON_CONNECTION_TIMEOUT = 0x08;
static constexpr int HCI_REASON_LOCAL_HOST_TERMINATED = 0x16;
static constexpr int HCI_REASON_FAILED_TO_ESTABLISH = 0x3E;

static int map_gatt_error(int rc) {
  if (rc == 0)
    return 0;
  if (rc > BLE_HS_ERR_ATT_BASE && rc < BLE_HS_ERR_HCI_BASE)
    return rc - BLE_HS_ERR_ATT_BASE;
  if (rc > BLE_HS_ERR_HCI_BASE && rc < BLE_HS_ERR_HCI_BASE + 0x100)
    return rc - BLE_HS_ERR_HCI_BASE;
  switch (rc) {
    case BLE_HS_ENOTCONN:
      return GATT_ERR_NOT_CONNECTED;
    case BLE_HS_ENOMEM:
    case BLE_HS_ENOMEM_EVT:
      return GATT_ERR_NO_MEMORY;
    case BLE_HS_EBUSY:
    case BLE_HS_EALREADY:
      return GATT_STATUS_BUSY;
    case BLE_HS_EMSGSIZE:
      return GATT_STATUS_INVALID_ATTR_LEN;
    case BLE_HS_EINVAL:
      return GATT_STATUS_ILLEGAL_PARAMETER;
    default:
      return GATT_STATUS_ERROR;
  }
}

// Connection-event code: HCI reason (Bluedroid's esp_gatt_conn_reason_t space).
static int map_connection_reason(int rc) {
  if (rc > BLE_HS_ERR_HCI_BASE && rc < BLE_HS_ERR_HCI_BASE + 0x100)
    return rc - BLE_HS_ERR_HCI_BASE;
  switch (rc) {
    case 0:
      return 0;
    case BLE_HS_ETIMEOUT:
      return HCI_REASON_CONNECTION_TIMEOUT;
    case BLE_HS_EAPP:  // our own ble_gap_conn_cancel()
      return HCI_REASON_LOCAL_HOST_TERMINATED;
    case BLE_HS_ENOTCONN:
      return HCI_REASON_FAILED_TO_ESTABLISH;
    default:
      return map_gatt_error(rc);
  }
}

// Pairing result: the SMP reason code (NimBLE offsets local failures by 0x400,
// peer-reported ones by 0x500).
static int map_pairing_status(int rc) {
  if (rc == 0)
    return 0;
  if (rc > BLE_HS_ERR_SM_PEER_BASE && rc < BLE_HS_ERR_SM_PEER_BASE + 0x100)
    return rc - BLE_HS_ERR_SM_PEER_BASE;
  if (rc > BLE_HS_ERR_SM_US_BASE && rc < BLE_HS_ERR_SM_US_BASE + 0x100)
    return rc - BLE_HS_ERR_SM_US_BASE;
  return map_connection_reason(rc);
}

static ESPBTUUID uuid_from_nimble(const ble_uuid_any_t &uuid) {
  switch (uuid.u.type) {
    case BLE_UUID_TYPE_16:
      return ESPBTUUID::from_uint16(uuid.u16.value);
    case BLE_UUID_TYPE_32:
      return ESPBTUUID::from_uint32(uuid.u32.value);
    default:
      // NimBLE stores 128-bit UUIDs little-endian, ESPBTUUID's raw form; the
      // hub wrapper's fill_128bit_uuid_array() then produces the API packing.
      return ESPBTUUID::from_raw(uuid.u128.value);
  }
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<NimBLEGattClient *> NimBLEGattClient::connect_owner{nullptr};
uint8_t NimBLEGattClient::instance_count = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// Initiating-scan window for outgoing connects (Bluedroid's fast direct
// connection values, BTM_BLE_SCAN_FAST_INT / _WIN): 60 ms / 30 ms.
static constexpr uint16_t CONN_SCAN_INTERVAL = 96;
static constexpr uint16_t CONN_SCAN_WINDOW = 48;
static constexpr uint16_t CONN_CE_MIN = 16;
static constexpr uint16_t CONN_CE_MAX = 48;
// Pending engines re-attempt the stack-wide connect on this cadence.
static constexpr uint32_t CONNECT_RETRY_INTERVAL_MS = 50;
// Past NimBLE's own connect timeout: a lost completion is forced down here.
static constexpr uint32_t CONNECT_BACKSTOP_MS = 5000;

// ---- setup ----

void NimBLEGattClient::setup() {
  this->engine_index_ = instance_count++;
  if (!this->events_.init()) {
    ESP_LOGE(TAG, "[%u] Event queue allocation failed", this->engine_index_);
    this->mark_failed();
    return;
  }
  RAMAllocator<ReadContext> allocator;
  this->reads_ = allocator.allocate(NIMBLE_GATT_MAX_READS);
  if (this->reads_ == nullptr) {
    ESP_LOGE(TAG, "[%u] Read buffer allocation failed", this->engine_index_);
    this->mark_failed();
    return;
  }
  for (uint8_t i = 0; i < NIMBLE_GATT_MAX_READS; i++) {
    new (&this->reads_[i]) ReadContext();
    this->reads_[i].owner = this;
  }
  nimble_base::global_nimble_base->add_listener(this);
#ifdef USE_OTA_STATE_LISTENER
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
  this->disable_loop();
}

void NimBLEGattClient::dump_config() {
  ESP_LOGCONFIG(TAG, "NimBLE GATT client %u", this->engine_index_);
  if (this->dropped_notifications_ != 0) {
    ESP_LOGCONFIG(TAG, "  Notifications dropped (queue full): %" PRIu32, this->dropped_notifications_);
  }
}

// ---- host task: copy and queue ----

void NimBLEGattClient::push_event_(NimBLEGattEvent::Type type, int status, uint16_t handle, uint16_t value,
                                   const uint8_t *data, uint16_t len) {
  bool queued = this->events_.push([&](NimBLEGattEvent &e) {
    e.type = type;
    e.status = status;
    e.handle = handle;
    e.value = value;
    e.len = std::min<uint16_t>(len, NIMBLE_GATT_MAX_ATTR_LEN);
    if (e.len != 0 && data != nullptr)
      memcpy(e.data, data, e.len);
  });
  if (!queued && type != NimBLEGattEvent::NOTIFY) {
    // A lost control event leaves the engine state untrustworthy; loop()
    // recovers with a forced teardown (rp2 parity).
    this->control_lost_.store(true);
  }
  this->enable_loop_soon_any_context();
}

int NimBLEGattClient::gap_event_(struct ble_gap_event *event, void *arg) {
  auto *self = static_cast<NimBLEGattClient *>(arg);
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      // The stack-wide connect procedure is over either way.
      NimBLEGattClient *expected = self;
      connect_owner.compare_exchange_strong(expected, nullptr);
      if (event->connect.status == 0) {
        // Stamp here: a disconnect racing the queued CONNECTED routes by handle.
        self->conn_handle_atomic_.store(event->connect.conn_handle);
        self->push_event_(NimBLEGattEvent::CONNECTED, 0, 0, event->connect.conn_handle);
      } else {
        self->push_event_(NimBLEGattEvent::CONNECT_FAILED, event->connect.status);
      }
      break;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      self->push_event_(NimBLEGattEvent::DISCONNECTED, event->disconnect.reason);
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
      // Notifications and indications both land here; NimBLE confirms
      // indications itself (Bluedroid parity). The host frees the mbuf after
      // this callback, so copy it now - straight into the queue slot, keeping
      // 512 bytes off the host task's stack.
      const auto &rx = event->notify_rx;
      // A full ring counts the drop; notification data is lossy by nature.
      self->events_.push([&rx](NimBLEGattEvent &e) {
        e.type = NimBLEGattEvent::NOTIFY;
        e.status = 0;
        e.handle = rx.attr_handle;
        e.value = 0;
        e.len = std::min<uint16_t>(OS_MBUF_PKTLEN(rx.om), NIMBLE_GATT_MAX_ATTR_LEN);
        if (os_mbuf_copydata(rx.om, 0, e.len, e.data) != 0)
          e.len = 0;
      });
      self->enable_loop_soon_any_context();
      break;
    }
    case BLE_GAP_EVENT_MTU:
      // Peer-initiated exchange (or ours completing); writes size against it.
      self->mtu_.store(event->mtu.value);
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      self->push_event_(NimBLEGattEvent::ENC_CHANGE, event->enc_change.status);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The peer lost its bond but we still hold one: drop ours and pair
      // again, which is what Bluedroid does on its own.
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      // No trusted IO channel (the proxy has none, like the Bluedroid backend,
      // which only accepts security requests): passkey/numeric-comparison
      // pairing is not answered and fails on the peer's timeout.
      ESP_LOGW(TAG, "Passkey action %d requested; not supported", event->passkey.params.action);
      break;
    default:
      break;
  }
  return 0;
}

int NimBLEGattClient::mtu_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg) {
  auto *self = static_cast<NimBLEGattClient *>(arg);
  self->push_event_(NimBLEGattEvent::MTU_EXCHANGED, error->status, 0, mtu);
  return 0;
}

// ---- host task: discovery (chained inside the callbacks) ----

void NimBLEGattClient::finish_discovery_(int error) {
  this->push_event_(NimBLEGattEvent::DISCOVERY_DONE, error);
}

int NimBLEGattClient::issue_characteristic_query_() {
  auto &service = this->arena_->services[this->disc_service_cursor_];
  service.first_characteristic = this->char_count_;
  return ble_gattc_disc_all_chrs(this->conn_handle_(), service.start_handle, service.end_handle,
                                 NimBLEGattClient::disc_chr_cb_, this);
}

int NimBLEGattClient::issue_descriptor_query_() {
  // Characteristics whose value handle is their last handle own no
  // descriptors; NimBLE's search starts at value_handle + 1, so skip them.
  while (this->disc_char_cursor_ < this->char_count_) {
    auto &chr = this->arena_->characteristics[this->disc_char_cursor_];
    chr.first_descriptor = this->desc_count_;
    chr.descriptor_count = 0;
    if (chr.value_handle < chr.end_handle) {
      return ble_gattc_disc_all_dscs(this->conn_handle_(), chr.value_handle, chr.end_handle,
                                     NimBLEGattClient::disc_dsc_cb_, this);
    }
    this->disc_char_cursor_++;
  }
  this->finish_discovery_(0);
  return 0;
}

int NimBLEGattClient::disc_svc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                   const struct ble_gatt_svc *service, void *arg) {
  auto *self = static_cast<NimBLEGattClient *>(arg);
  std::lock_guard<std::recursive_mutex> lock(self->arena_mutex_);
  if (self->arena_ == nullptr) {
    self->finish_discovery_(GATT_ERR_NOT_CONNECTED);
    return BLE_HS_EDONE;
  }
  if (error->status == 0) {
    if (self->service_count_ >= NIMBLE_GATT_MAX_SERVICES) {
      self->finish_discovery_(GATT_STATUS_INSUF_RESOURCE);
      return BLE_HS_EDONE;  // nonzero ends the procedure
    }
    auto &dst = self->arena_->services[self->service_count_++];
    dst.uuid = uuid_from_nimble(service->uuid);
    dst.start_handle = service->start_handle;
    dst.end_handle = service->end_handle;
    dst.first_characteristic = 0;
    dst.characteristic_count = 0;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    self->finish_discovery_(map_gatt_error(error->status));
    return 0;
  }
  if (self->service_count_ == 0) {
    self->finish_discovery_(0);
    return 0;
  }
  self->disc_service_cursor_ = 0;
  int rc = self->issue_characteristic_query_();
  if (rc != 0)
    self->finish_discovery_(map_gatt_error(rc));
  return 0;
}

int NimBLEGattClient::disc_chr_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                   const struct ble_gatt_chr *chr, void *arg) {
  auto *self = static_cast<NimBLEGattClient *>(arg);
  std::lock_guard<std::recursive_mutex> lock(self->arena_mutex_);
  if (self->arena_ == nullptr) {
    self->finish_discovery_(GATT_ERR_NOT_CONNECTED);
    return BLE_HS_EDONE;
  }
  auto &service = self->arena_->services[self->disc_service_cursor_];
  if (error->status == 0) {
    if (self->char_count_ >= NIMBLE_GATT_MAX_CHARACTERISTICS) {
      self->finish_discovery_(GATT_STATUS_INSUF_RESOURCE);
      return BLE_HS_EDONE;
    }
    // The previous characteristic of this service ends right before this one.
    if (self->char_count_ > service.first_characteristic) {
      self->arena_->characteristics[self->char_count_ - 1].end_handle = chr->def_handle - 1;
    }
    auto &dst = self->arena_->characteristics[self->char_count_++];
    dst.uuid = uuid_from_nimble(chr->uuid);
    dst.value_handle = chr->val_handle;
    dst.end_handle = service.end_handle;
    dst.properties = chr->properties;
    dst.first_descriptor = 0;
    dst.descriptor_count = 0;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    self->finish_discovery_(map_gatt_error(error->status));
    return 0;
  }
  service.characteristic_count = self->char_count_ - service.first_characteristic;
  self->disc_service_cursor_++;
  int rc;
  if (self->disc_service_cursor_ < self->service_count_) {
    rc = self->issue_characteristic_query_();
  } else if (self->char_count_ == 0) {
    self->finish_discovery_(0);
    return 0;
  } else {
    self->disc_char_cursor_ = 0;
    rc = self->issue_descriptor_query_();
  }
  if (rc != 0)
    self->finish_discovery_(map_gatt_error(rc));
  return 0;
}

int NimBLEGattClient::disc_dsc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                                   const struct ble_gatt_dsc *dsc, void *arg) {
  auto *self = static_cast<NimBLEGattClient *>(arg);
  std::lock_guard<std::recursive_mutex> lock(self->arena_mutex_);
  if (self->arena_ == nullptr) {
    self->finish_discovery_(GATT_ERR_NOT_CONNECTED);
    return BLE_HS_EDONE;
  }
  auto &chr = self->arena_->characteristics[self->disc_char_cursor_];
  if (error->status == 0) {
    if (self->desc_count_ >= NIMBLE_GATT_MAX_DESCRIPTORS) {
      self->finish_discovery_(GATT_STATUS_INSUF_RESOURCE);
      return BLE_HS_EDONE;
    }
    auto &dst = self->arena_->descriptors[self->desc_count_++];
    dst.uuid = uuid_from_nimble(dsc->uuid);
    dst.handle = dsc->handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE) {
    self->finish_discovery_(map_gatt_error(error->status));
    return 0;
  }
  chr.descriptor_count = self->desc_count_ - chr.first_descriptor;
  self->disc_char_cursor_++;
  int rc = self->issue_descriptor_query_();
  if (rc != 0)
    self->finish_discovery_(map_gatt_error(rc));
  return 0;
}

// ---- host task: reads and writes ----

int NimBLEGattClient::read_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                               void *arg) {
  auto *ctx = static_cast<ReadContext *>(arg);
  NimBLEGattClient *self = ctx->owner;
  if (error->status == 0 && attr != nullptr) {
    // One fragment per callback at attr->offset (read-long reassembly).
    uint16_t offset = attr->offset;
    if (offset < NIMBLE_GATT_MAX_ATTR_LEN) {
      uint16_t frag = std::min<uint16_t>(OS_MBUF_PKTLEN(attr->om), NIMBLE_GATT_MAX_ATTR_LEN - offset);
      if (os_mbuf_copydata(attr->om, 0, frag, ctx->buffer + offset) == 0)
        ctx->len = std::max<uint16_t>(ctx->len, offset + frag);
    }
    return 0;
  }
  int status = error->status == BLE_HS_EDONE ? 0 : map_gatt_error(error->status);
  // A value that is an exact multiple of MTU - 1 ends with a blob request some
  // peers refuse; the read is complete, not failed (rp2 parity).
  if ((status == ATT_ERR_INVALID_OFFSET || status == ATT_ERR_ATTR_NOT_LONG) && ctx->len > 0)
    status = 0;
  self->push_event_(NimBLEGattEvent::READ_DONE, status, ctx->handle, 0, status == 0 ? ctx->buffer : nullptr,
                    status == 0 ? ctx->len : 0);
  ctx->in_use = false;
  return 0;
}

int NimBLEGattClient::write_cb_(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                                void *arg) {
  auto *self = static_cast<NimBLEGattClient *>(arg);
  uint16_t handle = attr != nullptr ? attr->handle : error->att_handle;
  self->push_event_(NimBLEGattEvent::WRITE_DONE, map_gatt_error(error->status), handle);
  return 0;
}

// ---- main loop ----

void NimBLEGattClient::loop() {
  NimBLEGattEvent *event;
  while ((event = this->events_.front()) != nullptr) {
    this->handle_event_(*event);
    this->events_.pop_front();
  }
  uint32_t dropped = this->events_.take_dropped();
  if (this->control_lost_.exchange(false)) {
    ESP_LOGE(TAG, "[%u] GATT control event lost, disconnecting", this->engine_index_);
    this->gatt_disconnect();
  } else if (dropped != 0) {
    this->dropped_notifications_ += dropped;
    ESP_LOGW(TAG, "[%u] Dropped %" PRIu32 " notifications (queue full)", this->engine_index_, dropped);
  }

  const uint32_t now = millis();
  const uint32_t timeout = nimble_base::global_nimble_base->get_connection_timeout();
  switch (this->state_) {
    case EngineState::CONNECT_PENDING:
      if (now - this->connect_started_ > timeout) {
        ESP_LOGW(TAG, "[%u] Connect timeout (queued)", this->engine_index_);
        this->fail_connection_(HCI_REASON_CONNECTION_TIMEOUT);
      } else if (now - this->connect_retry_ms_ >= CONNECT_RETRY_INTERVAL_MS) {
        this->connect_retry_ms_ = now;
        if (int err = this->try_gap_connect_(); err != 0) {
          this->fail_connection_(err);
        }
      }
      break;
    case EngineState::CONNECTING:
      // NimBLE's own duration timer normally reports the timeout; this is the
      // backstop for a lost completion.
      if (now - this->connect_started_ > timeout + CONNECT_BACKSTOP_MS) {
        ESP_LOGW(TAG, "[%u] Connect never completed, giving up", this->engine_index_);
        if (connect_owner.load() == this)
          ble_gap_conn_cancel();
        this->fail_connection_(HCI_REASON_CONNECTION_TIMEOUT);
      }
      break;
    case EngineState::MTU_EXCHANGE:
      if (now - this->connect_started_ > timeout) {
        ESP_LOGW(TAG, "[%u] MTU exchange timeout", this->engine_index_);
        this->gatt_disconnect();
      }
      break;
    case EngineState::DISCONNECTING:
      if (now - this->disconnecting_started_ > ble_device_base::GATT_DISCONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "[%u] Disconnect timeout, forcing idle", this->engine_index_);
        this->fail_connection_(HCI_REASON_CONNECTION_TIMEOUT);
      }
      break;
    case EngineState::IDLE:
      if (this->events_.empty())
        this->disable_loop();
      break;
    case EngineState::READY:
      break;
  }
}

void NimBLEGattClient::handle_event_(const NimBLEGattEvent &event) {
  switch (event.type) {
    case NimBLEGattEvent::CONNECTED:
      this->handle_connected_(event.value);
      break;
    case NimBLEGattEvent::CONNECT_FAILED:
      if (this->state_ == EngineState::CONNECTING) {
        // fail_connection_ logs the outcome.
        this->fail_connection_(map_connection_reason(event.status));
      }
      break;
    case NimBLEGattEvent::DISCONNECTED:
      if (this->state_ != EngineState::IDLE) {
        ESP_LOGV(TAG, "[%u] Disconnected, reason=0x%x", this->engine_index_, event.status);
        this->fail_connection_(map_connection_reason(event.status));
      }
      break;
    case NimBLEGattEvent::MTU_EXCHANGED: {
      if (this->state_ != EngineState::MTU_EXCHANGE)
        break;
      uint16_t mtu = event.status == 0 ? event.value : ble_att_mtu(this->conn_handle_());
      if (mtu < ble_device_base::DEFAULT_ATT_MTU)
        mtu = ble_device_base::DEFAULT_ATT_MTU;
      this->mtu_.store(mtu);
      this->state_ = EngineState::READY;
      // Link up before the inhibit is released, so the coexistence preference
      // stays on Bluetooth instead of dropping to balanced for one call.
      this->link_up_ = true;
      this->parent_->on_link_up();
      // Scanning resumes and runs alongside the established link.
      this->release_scan_inhibit_();
      // INFO, not DEBUG: halo-style builds run the logger at INFO, and the
      // upstream wrapper reports connect/disconnect at DEBUG only.
      [[maybe_unused]] char peer[18];  // unused when the logger level is above INFO
      ESP_LOGI(TAG, "[%u] [%s] Connected, MTU %u", this->engine_index_, this->peer_str_(peer), mtu);
      this->listener_->on_connection_state(true, mtu, 0);
      break;
    }
    case NimBLEGattEvent::DISCOVERY_DONE: {
      if (!this->discovery_in_progress_)
        break;
      this->discovery_in_progress_ = false;
      int error = event.status;
      ESP_LOGV(TAG, "[%u] Discovery done (err=%d): %u services, %u characteristics, %u descriptors",
               this->engine_index_, error, this->service_count_, this->char_count_, this->desc_count_);
      if (error == GATT_STATUS_INSUF_RESOURCE) {
        ESP_LOGE(TAG, "[%u] Service table full (device exceeds %u services / %u characteristics / %u descriptors)",
                 this->engine_index_, NIMBLE_GATT_MAX_SERVICES, NIMBLE_GATT_MAX_CHARACTERISTICS,
                 NIMBLE_GATT_MAX_DESCRIPTORS);
      }
      if (error == 0 && this->state_ == EngineState::READY) {
        // Discovery no longer needs the fast interval (Bluedroid steps down at
        // SEARCH_CMPL).
        struct ble_gap_upd_params params {};
        params.itvl_min = MEDIUM_MIN_CONN_INTERVAL;
        params.itvl_max = MEDIUM_MAX_CONN_INTERVAL;
        params.latency = 0;
        params.supervision_timeout = MEDIUM_CONN_TIMEOUT;
        ble_gap_update_params(this->conn_handle_(), &params);
      }
      if (error != 0)
        this->release_services();
      this->listener_->on_service_discovery_done(error);
      break;
    }
    case NimBLEGattEvent::READ_DONE:
      if (this->state_ == EngineState::READY)
        this->listener_->on_read_result(event.handle, event.len != 0 ? event.data : nullptr, event.len, event.status);
      break;
    case NimBLEGattEvent::WRITE_DONE:
      if (this->state_ == EngineState::READY)
        this->listener_->on_write_result(event.handle, event.status);
      break;
    case NimBLEGattEvent::NOTIFY:
      // esp32 parity: only characteristics the client registered for.
      if (this->state_ == EngineState::READY && this->notify_subscribed_(event.handle))
        this->listener_->on_notify_data(event.handle, event.data, event.len);
      break;
    case NimBLEGattEvent::ENC_CHANGE:
      if (this->state_ == EngineState::READY)
        this->listener_->on_pairing_result(map_pairing_status(event.status));
      break;
  }
}

void NimBLEGattClient::handle_connected_(uint16_t conn_handle) {
  if (this->state_ != EngineState::CONNECTING) {
    // A late link nobody is waiting for: close it so it does not leak.
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return;
  }
  if (this->cancel_requested_) {
    // The disconnect request lost the race against the connect: finish the
    // teardown instead of reporting a connection nobody wants.
    this->state_ = EngineState::DISCONNECTING;
    this->disconnecting_started_ = millis();
    this->release_scan_inhibit_();
    if (ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM) != 0) {
      this->push_event_(NimBLEGattEvent::DISCONNECTED, BLE_HS_HCI_ERR(HCI_REASON_LOCAL_HOST_TERMINATED));
    }
    return;
  }
  this->state_ = EngineState::MTU_EXCHANGE;
  // Bluedroid requests the MTU at CONNECT_EVT and reports the negotiated value
  // with the connection; NimBLE only exchanges when asked.
  int rc = ble_gattc_exchange_mtu(conn_handle, NimBLEGattClient::mtu_cb_, this);
  if (rc != 0) {
    // Already exchanged (peer-initiated) or refused: report what the link has.
    ESP_LOGV(TAG, "[%u] MTU exchange not started (%d)", this->engine_index_, rc);
    this->push_event_(NimBLEGattEvent::MTU_EXCHANGED, rc, 0, ble_att_mtu(conn_handle));
  }
}

void NimBLEGattClient::release_scan_inhibit_() {
  if (this->holds_scan_inhibit_) {
    this->holds_scan_inhibit_ = false;
    this->parent_->release_scan_inhibit();
  }
}

void NimBLEGattClient::cleanup_link_state_() {
  this->conn_handle_atomic_.store(BLE_HS_CONN_HANDLE_NONE);
  this->notify_subscription_count_ = 0;
  this->cancel_requested_ = false;
  this->discovery_in_progress_ = false;
  this->mtu_.store(ble_device_base::DEFAULT_ATT_MTU);
  for (uint8_t i = 0; i < NIMBLE_GATT_MAX_READS; i++)
    this->reads_[i].in_use = false;
  // Events queued behind the teardown belong to the old link.
  this->events_.clear();
  this->release_services();
}

const char *NimBLEGattClient::peer_str_(char *buf) const {
  const uint8_t *a = this->peer_addr_;
  snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
  return buf;
}

void NimBLEGattClient::fail_connection_(int reason) {
  {
    [[maybe_unused]] char peer[18];
    if (this->link_up_) {
      ESP_LOGI(TAG, "[%u] [%s] Disconnected, reason 0x%02x", this->engine_index_, this->peer_str_(peer), reason);
    } else if (this->cancel_requested_ || this->state_ == EngineState::DISCONNECTING) {
      ESP_LOGI(TAG, "[%u] [%s] Connect cancelled", this->engine_index_, this->peer_str_(peer));
    } else {
      ESP_LOGW(TAG, "[%u] [%s] Connect failed, reason 0x%02x", this->engine_index_, this->peer_str_(peer), reason);
    }
  }
  NimBLEGattClient *expected = this;
  connect_owner.compare_exchange_strong(expected, nullptr);
  if (this->state_ == EngineState::CONNECTING && this->conn_handle_() != BLE_HS_CONN_HANDLE_NONE) {
    // A success stamped the handle after the failure decision: close it.
    ble_gap_terminate(this->conn_handle_(), BLE_ERR_REM_USER_CONN_TERM);
  }
  this->cleanup_link_state_();
  this->release_scan_inhibit_();
  if (this->link_up_) {
    this->link_up_ = false;
    this->parent_->on_link_down();
  }
  this->state_ = EngineState::IDLE;
  this->listener_->on_connection_state(false, 0, reason);
}

// ---- connection control ----

int NimBLEGattClient::connect(uint64_t address, uint8_t addr_type) {
  if (this->is_failed() || !nimble_base::global_nimble_base->is_active())
    return GATT_ERR_NOT_CONNECTED;
  if (this->state_ != EngineState::IDLE)
    return GATT_STATUS_BUSY;
  // uint64 address is MSB-first; NimBLE wants controller order (LSB first).
  for (uint8_t i = 0; i < MAC_ADDRESS_SIZE; i++)
    this->peer_addr_[i] = (address >> (8 * i)) & 0xFF;
  this->peer_addr_type_ = addr_type;
  this->cancel_requested_ = false;
  this->connect_started_ = millis();
  this->connect_retry_ms_ = this->connect_started_;
  // Stop the scan for the connect attempt (NimBLE refuses to initiate while
  // discovery runs; esp32_ble_tracker stops it for the same reason).
  this->holds_scan_inhibit_ = true;
  this->parent_->inhibit_scan();
  if (int err = this->try_gap_connect_(); err != 0) {
    this->release_scan_inhibit_();
    this->state_ = EngineState::IDLE;
    return err;
  }
  this->enable_loop();
  return 0;
}

int NimBLEGattClient::try_gap_connect_() {
  NimBLEGattClient *expected = nullptr;
  if (!connect_owner.compare_exchange_strong(expected, this) && expected != this) {
    this->state_ = EngineState::CONNECT_PENDING;
    return 0;
  }
  ble_addr_t peer{};
  // BLE_ADDR_TYPE_* code space: bit 0 distinguishes public from random.
  peer.type = (this->peer_addr_type_ & 1) != 0 ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
  memcpy(peer.val, this->peer_addr_, MAC_ADDRESS_SIZE);

  // Cached connections come up at MEDIUM parameters (nothing uses the fast
  // interval without a discovery phase); uncached ones run FAST for discovery
  // and step down when it completes. Bluedroid's prefer-params, same values.
  const bool cached = this->connection_type_ == ble_device_base::ConnectionType::V3_WITH_CACHE;
  struct ble_gap_conn_params params {};
  params.scan_itvl = CONN_SCAN_INTERVAL;
  params.scan_window = CONN_SCAN_WINDOW;
  params.itvl_min = cached ? MEDIUM_MIN_CONN_INTERVAL : FAST_MIN_CONN_INTERVAL;
  params.itvl_max = cached ? MEDIUM_MAX_CONN_INTERVAL : FAST_MAX_CONN_INTERVAL;
  params.latency = 0;
  params.supervision_timeout = cached ? MEDIUM_CONN_TIMEOUT : FAST_CONN_TIMEOUT;
  params.min_ce_len = CONN_CE_MIN;
  params.max_ce_len = CONN_CE_MAX;

  const int32_t timeout = static_cast<int32_t>(nimble_base::global_nimble_base->get_connection_timeout());
  int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, timeout, &params, NimBLEGattClient::gap_event_, this);
  if (rc == 0) {
    this->state_ = EngineState::CONNECTING;
    return 0;
  }
  connect_owner.store(nullptr);
  if (rc == BLE_HS_EALREADY || rc == BLE_HS_EBUSY) {
    // Another connect or a discovery still holds the radio; retried from loop().
    this->state_ = EngineState::CONNECT_PENDING;
    return 0;
  }
  if (rc == BLE_HS_EDONE) {
    // Already linked to this peer on a connection this engine does not own
    // (its events go elsewhere): close it and connect fresh on the retry.
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find_by_addr(&peer, &desc) == 0) {
      ble_gap_terminate(desc.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    this->state_ = EngineState::CONNECT_PENDING;
    return 0;
  }
  ESP_LOGW(TAG, "[%u] ble_gap_connect failed: %d", this->engine_index_, rc);
  return map_connection_reason(rc);
}

int NimBLEGattClient::gatt_disconnect() {
  switch (this->state_) {
    case EngineState::IDLE:
      return GATT_ERR_NOT_CONNECTED;
    case EngineState::DISCONNECTING:
      return 0;
    case EngineState::CONNECT_PENDING:
      // Nothing issued stack-side; complete through the queue so the listener
      // is not re-entered from inside this call.
      this->push_event_(NimBLEGattEvent::DISCONNECTED, BLE_HS_HCI_ERR(HCI_REASON_LOCAL_HOST_TERMINATED));
      break;
    case EngineState::CONNECTING:
      if (this->conn_handle_() == BLE_HS_CONN_HANDLE_NONE) {
        // The cancel completes as a failed connect; a success that wins the
        // race is torn down in handle_connected_.
        this->cancel_requested_ = true;
        this->connect_started_ = millis();
        if (connect_owner.load() == this)
          ble_gap_conn_cancel();
        this->enable_loop();
        return 0;
      }
      [[fallthrough]];
    default:
      if (ble_gap_terminate(this->conn_handle_(), BLE_ERR_REM_USER_CONN_TERM) != 0) {
        this->push_event_(NimBLEGattEvent::DISCONNECTED, BLE_HS_HCI_ERR(HCI_REASON_LOCAL_HOST_TERMINATED));
      }
      break;
  }
  this->state_ = EngineState::DISCONNECTING;
  this->disconnecting_started_ = millis();
  this->release_scan_inhibit_();
  this->enable_loop();
  return 0;
}

// ---- GATT operations ----

int NimBLEGattClient::discover_services() {
  if (this->state_ != EngineState::READY)
    return GATT_ERR_NOT_CONNECTED;
  if (this->discovery_in_progress_)
    return GATT_STATUS_BUSY;
  {
    std::lock_guard<std::recursive_mutex> lock(this->arena_mutex_);
    if (this->arena_ == nullptr) {
      // Transient, freed right after streaming. PSRAM first: this is ~10 KB.
      RAMAllocator<ServiceArena> allocator;
      this->arena_ = allocator.allocate(1);
      if (this->arena_ == nullptr) {
        ESP_LOGE(TAG, "[%u] Service table allocation failed", this->engine_index_);
        return GATT_ERR_NO_MEMORY;
      }
      new (this->arena_) ServiceArena();
    }
    this->service_count_ = 0;
    this->char_count_ = 0;
    this->desc_count_ = 0;
    this->disc_service_cursor_ = 0;
    this->disc_char_cursor_ = 0;
  }
  this->discovery_in_progress_ = true;
  int rc = ble_gattc_disc_all_svcs(this->conn_handle_(), NimBLEGattClient::disc_svc_cb_, this);
  if (rc != 0) {
    this->discovery_in_progress_ = false;
    this->release_services();
    return map_gatt_error(rc);
  }
  return 0;
}

ble_device_base::GattServiceTable NimBLEGattClient::get_service_table() {
  ble_device_base::GattServiceTable table;
  std::lock_guard<std::recursive_mutex> lock(this->arena_mutex_);
  if (this->arena_ != nullptr && !this->discovery_in_progress_) {
    table.services = this->arena_->services;
    table.characteristics = this->arena_->characteristics;
    table.descriptors = this->arena_->descriptors;
    table.service_count = this->service_count_;
    table.characteristic_count = this->char_count_;
    table.descriptor_count = this->desc_count_;
  }
  return table;
}

void NimBLEGattClient::release_services() {
  std::lock_guard<std::recursive_mutex> lock(this->arena_mutex_);
  if (this->arena_ != nullptr) {
    RAMAllocator<ServiceArena> allocator;
    this->arena_->~ServiceArena();
    allocator.deallocate(this->arena_, 1);
    this->arena_ = nullptr;
  }
  this->service_count_ = 0;
  this->char_count_ = 0;
  this->desc_count_ = 0;
}

int NimBLEGattClient::start_read_(uint16_t handle) {
  if (this->state_ != EngineState::READY)
    return GATT_ERR_NOT_CONNECTED;
  ReadContext *ctx = nullptr;
  for (uint8_t i = 0; i < NIMBLE_GATT_MAX_READS; i++) {
    if (!this->reads_[i].in_use) {
      ctx = &this->reads_[i];
      break;
    }
  }
  if (ctx == nullptr)
    return GATT_STATUS_BUSY;
  ctx->in_use = true;
  ctx->handle = handle;
  ctx->len = 0;
  // Long read: a plain read first, blob continuations only past MTU - 1
  // (Bluedroid continues long reads the same way).
  int rc = ble_gattc_read_long(this->conn_handle_(), handle, 0, NimBLEGattClient::read_cb_, ctx);
  if (rc != 0) {
    ctx->in_use = false;
    return map_gatt_error(rc);
  }
  return 0;
}

int NimBLEGattClient::write_characteristic(uint16_t handle, const uint8_t *data, uint16_t len, bool response) {
  if (this->state_ != EngineState::READY)
    return GATT_ERR_NOT_CONNECTED;
  if (len > NIMBLE_GATT_MAX_ATTR_LEN)
    return GATT_STATUS_INVALID_ATTR_LEN;
  const uint16_t payload_max = this->mtu_.load() - 3;
  if (!response) {
    // NimBLE would silently truncate an oversized command to the MTU; refuse
    // it instead so the client never sees a short write acknowledged.
    if (len > payload_max)
      return GATT_STATUS_INVALID_ATTR_LEN;
    int rc = ble_gattc_write_no_rsp_flat(this->conn_handle_(), handle, data, len);
    if (rc != 0)
      return map_gatt_error(rc);
    // No completion exists for a command; report it like Bluedroid (which
    // raises WRITE_CHAR_EVT for write-no-response too).
    this->listener_->on_write_result(handle, 0);
    return 0;
  }
  int rc;
  if (len <= payload_max) {
    rc = ble_gattc_write_flat(this->conn_handle_(), handle, data, len, NimBLEGattClient::write_cb_, this);
  } else {
    // Prepared (long) write, which Bluedroid also switches to on its own.
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == nullptr)
      return GATT_ERR_NO_MEMORY;
    rc = ble_gattc_write_long(this->conn_handle_(), handle, 0, om, NimBLEGattClient::write_cb_, this);
  }
  return map_gatt_error(rc);
}

int NimBLEGattClient::notify_characteristic(uint16_t handle, bool enable) {
  if (this->state_ != EngineState::READY)
    return GATT_ERR_NOT_CONNECTED;
  // Local registration only: the API client writes the CCCD itself as a
  // descriptor write (V3 semantics), exactly as with Bluedroid.
  if (enable) {
    if (!this->notify_subscribed_(handle)) {
      if (this->notify_subscription_count_ >= NIMBLE_GATT_MAX_NOTIFY_SUBSCRIPTIONS)
        return GATT_ERR_NO_MEMORY;
      this->notify_subscriptions_[this->notify_subscription_count_++] = handle;
    }
  } else {
    for (uint8_t i = 0; i < this->notify_subscription_count_; i++) {
      if (this->notify_subscriptions_[i] == handle) {
        this->notify_subscriptions_[i] = this->notify_subscriptions_[--this->notify_subscription_count_];
        break;
      }
    }
  }
  this->listener_->on_notify_state(handle, enable, 0);
  return 0;
}

bool NimBLEGattClient::notify_subscribed_(uint16_t handle) const {
  for (uint8_t i = 0; i < this->notify_subscription_count_; i++) {
    if (this->notify_subscriptions_[i] == handle)
      return true;
  }
  return false;
}

int NimBLEGattClient::pair() {
  if (this->state_ != EngineState::READY)
    return GATT_ERR_NOT_CONNECTED;
  int rc = ble_gap_security_initiate(this->conn_handle_());
  if (rc == BLE_HS_EALREADY)
    return 0;  // already in progress; ENC_CHANGE answers it
  return map_gatt_error(rc);
}

int NimBLEGattClient::update_connection_params(uint16_t min_interval, uint16_t max_interval, uint16_t latency,
                                               uint16_t timeout) {
  if (this->state_ != EngineState::READY)
    return GATT_ERR_NOT_CONNECTED;
  struct ble_gap_upd_params params {};
  params.itvl_min = min_interval;
  params.itvl_max = max_interval;
  params.latency = latency;
  params.supervision_timeout = timeout;
  return map_gatt_error(ble_gap_update_params(this->conn_handle_(), &params));
}

// ---- lifecycle ----

#ifdef USE_OTA_STATE_LISTENER
void NimBLEGattClient::on_ota_global_state(ota::OTAState state, float progress, uint8_t error,
                                           ota::OTAComponent *comp) {
  // esp32_ble_tracker parity: every client is disconnected when an OTA starts.
  if (state == ota::OTA_STARTED && this->state_ != EngineState::IDLE) {
    this->gatt_disconnect();
  }
}
#endif

void NimBLEGattClient::on_nimble_disabling() {
  // No completion events follow once the stack stops: settle the slot now
  // (Bluedroid backend's ble_before_disabled_event_handler parity).
  if (this->state_ == EngineState::IDLE)
    return;
  NimBLEGattClient *expected = this;
  connect_owner.compare_exchange_strong(expected, nullptr);
  this->fail_connection_(GATT_ERR_NOT_CONNECTED);
}

// ---- address-scoped maintenance (declared in bluetooth_connection.h) ----

conn_err_t unpair_device(uint64_t address) {
  uint8_t mac[MAC_ADDRESS_SIZE];
  for (uint8_t i = 0; i < MAC_ADDRESS_SIZE; i++)
    mac[i] = (address >> (8 * i)) & 0xFF;
  ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
  int count = 0;
  if (ble_store_util_bonded_peers(peers, &count, MYNEWT_VAL(BLE_STORE_MAX_BONDS)) != 0)
    return GATT_NOT_CONNECTED;
  bool found = false;
  // Every address type: stale entries can share the address bytes.
  for (int i = 0; i < count; i++) {
    if (memcmp(peers[i].val, mac, MAC_ADDRESS_SIZE) == 0 && ble_store_util_delete_peer(&peers[i]) == 0) {
      found = true;
    }
  }
  // Unknown address: an error, like Bluedroid's remove-bond call.
  return found ? CONN_OK : GATT_NOT_CONNECTED;
}

conn_err_t clear_gatt_cache(uint64_t address) {
  // NimBLE keeps no stack-side GATT database here: discovery tables are
  // transient and freed after streaming, and the client caches the database
  // itself. Nothing to clear, so the request succeeds (Bluedroid clears its
  // per-device cache and reports the same).
  return CONN_OK;
}

}  // namespace esphome::bluetooth_connection

#endif  // USE_NIMBLE_BLE && USE_BLE_GATT_CLIENT
