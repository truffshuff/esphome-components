#include "nimble_tracker.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"

#ifdef USE_NIMBLE_SOFTWARE_COEXISTENCE
#include <esp_coexist.h>
#endif

// NimBLE's modlog defines LOG_LEVEL_* macros that collide with ESPHome names.
#undef LOG_LEVEL_NONE
#undef LOG_LEVEL_ERROR
#undef LOG_LEVEL_WARN
#undef LOG_LEVEL_INFO
#undef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_VERBOSE

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::nimble_tracker {

static const char *const TAG = "nimble_tracker";

using ble_device_base::ScannerState;

// Floor between controller start attempts after a failure.
static constexpr uint32_t SCAN_START_RETRY_MS = 1000;
// Grace past two scan periods before a scan that never completed is forced
// down. esp32_ble_tracker reboots here; this restarts the scan instead (halo
// runs reboot_timeout: 0s to keep in-RAM history, see its ARCHITECTURE.md).
static constexpr uint32_t SCAN_STUCK_GRACE_MS = 10000;
static constexpr float BLE_SCAN_UNIT_MS = 0.625f;

// HCI LE advertising report event types (Core spec). ADV_IND and ADV_SCAN_IND
// are the scannable ones whose scan response gets merged.
static constexpr uint8_t ADV_EVENT_TYPE_ADV_IND = 0;
static constexpr uint8_t ADV_EVENT_TYPE_ADV_SCAN_IND = 2;
static constexpr uint8_t ADV_EVENT_TYPE_SCAN_RSP = 4;

// [[maybe_unused]]: only ESP_LOGCONFIG / the stats line use it, which the logger can compile out.
[[maybe_unused]] static const char *scanner_state_to_string(ScannerState state) {
  switch (state) {
    case ScannerState::IDLE:
      return "IDLE";
    case ScannerState::STARTING:
      return "STARTING";
    case ScannerState::RUNNING:
      return "RUNNING";
    case ScannerState::FAILED:
      return "FAILED";
    case ScannerState::STOPPING:
      return "STOPPING";
    case ScannerState::STOPPED:
      return "STOPPED";
  }
  return "UNKNOWN";
}

void NimBLETracker::setup() {
  if (this->parent_->is_failed()) {
    ESP_LOGE(TAG, "NimBLE stack failed; tracker disabled");
    this->mark_failed();
    return;
  }
  if (!this->scan_events_.init()) {
    ESP_LOGE(TAG, "Scan event queue allocation failed");
    this->mark_failed();
    return;
  }
  this->parent_->add_listener(this);
  this->merger_.bind(&this->dispatcher_, &this->scan_continuous_, TAG);
#ifdef USE_OTA_STATE_LISTENER
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
}

// ---- host task ----

int NimBLETracker::gap_event_(struct ble_gap_event *event, void *arg) {
  auto *self = static_cast<NimBLETracker *>(arg);
  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      const auto &disc = event->disc;
      self->scan_events_.push([&disc](ScanEvent &e) {
        e.kind = ScanEvent::REPORT;
        e.event_type = disc.event_type;
        e.addr_type = disc.addr.type;
        e.rssi = disc.rssi;
        memcpy(e.mac, disc.addr.val, MAC_ADDRESS_SIZE);
        e.data_len = std::min<uint8_t>(disc.length_data, sizeof(e.data));
        if (e.data_len != 0 && disc.data != nullptr)
          memcpy(e.data, disc.data, e.data_len);
      });
      break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
      int reason = event->disc_complete.reason;
      self->scan_events_.push([reason](ScanEvent &e) {
        e.kind = ScanEvent::COMPLETE;
        e.reason = reason;
      });
      break;
    }
    default:
      break;
  }
  return 0;
}

// ---- main loop ----

void NimBLETracker::loop() {
  ScanEvent *event;
  while ((event = this->scan_events_.front()) != nullptr) {
    this->handle_event_(*event);
    this->scan_events_.pop_front();
  }
  uint32_t dropped = this->scan_events_.take_dropped();
  if (dropped != 0) {
    this->dropped_reports_ += dropped;
    ESP_LOGV(TAG, "Scan queue full, %" PRIu32 " reports dropped", dropped);
  }

  const uint32_t now = App.get_loop_component_start_time();
  if (!this->merger_.empty())
    this->merger_.sweep(now);
  if (this->stats_interval_ms_ != 0 && now - this->stats_last_ms_ >= this->stats_interval_ms_)
    this->log_stats_(now);

#ifdef USE_OTA_STATE_LISTENER
  if (this->ota_in_progress_)
    return;
#endif
  if (!this->parent_->is_active())
    return;

  if (this->pending_start_) {
    this->pending_start_ = false;
    this->start_scan_();
  }

  if (!this->scan_running_) {
    if (this->scan_continuous_ && this->inhibit_count_ == 0 &&
        (!this->last_start_failed_ || now - this->last_scan_start_attempt_ >= SCAN_START_RETRY_MS)) {
      this->start_scan_();
    }
    return;
  }

  // A link came up or went away: reprogram the window now (esp32 parity),
  // without ending the scan period.
  if (this->scan_continuous_ && this->programmed_window_ != this->desired_window_()) {
    this->stop_scan_(false);
    this->start_scan_();
    return;
  }

  if (now - this->scan_period_start_ > 2 * this->scan_duration_ + SCAN_STUCK_GRACE_MS) {
    ESP_LOGE(TAG, "Scan never completed, restarting it");
    this->stop_scan_(true);
  }
}

void NimBLETracker::handle_event_(const ScanEvent &event) {
  if (event.kind == ScanEvent::COMPLETE) {
    // A cancelled scan posts no completion, but a scan that ended on its own
    // can still have one queued after a new scan started; only a completion
    // with no discovery running is current.
    if (!this->scan_running_ || ble_gap_disc_active()) {
      return;
    }
    this->scan_running_ = false;
    if (event.reason != 0) {
      ESP_LOGW(TAG, "Scan ended unexpectedly, reason=%d", event.reason);
    }
    this->fire_scan_end_();
    this->set_scanner_state_(ScannerState::IDLE);
    return;
  }

  this->reports_received_++;
  if (event.event_type == ADV_EVENT_TYPE_SCAN_RSP) {
    this->scan_responses_received_++;
    this->merger_.submit_scan_rsp(event.mac, event.rssi, event.addr_type, event.data, event.data_len);
    return;
  }
  // Merge only while an active scan runs: nothing sweeps the merger after a stop.
  if (this->scan_running_ && this->scan_active_ &&
      (event.event_type == ADV_EVENT_TYPE_ADV_IND || event.event_type == ADV_EVENT_TYPE_ADV_SCAN_IND)) {
    this->merger_.stash_adv(event.mac, event.rssi, event.addr_type, event.data, event.data_len,
                            App.get_loop_component_start_time());
    return;
  }
  this->dispatcher_.dispatch(event.mac, event.rssi, event.addr_type, event.data, event.data_len,
                             /*raw_only=*/false, this->scan_continuous_ ? nullptr : TAG);
}

// ---- scan control ----

bool NimBLETracker::controller_scan_start_() {
  this->last_scan_start_attempt_ = App.get_loop_component_start_time();
  struct ble_gap_disc_params params {};
  params.itvl = static_cast<uint16_t>(this->scan_interval_);
  params.window = static_cast<uint16_t>(this->desired_window_());
  params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  params.limited = 0;
  params.passive = this->scan_active_ ? 0 : 1;
  // esp32_ble_tracker scans with duplicate filtering disabled: Home Assistant
  // relies on every advertisement for RSSI and for payloads that change
  // under the same address.
  params.filter_duplicates = 0;

  int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, static_cast<int32_t>(this->scan_duration_), &params,
                        NimBLETracker::gap_event_, this);
  if (rc == BLE_HS_EALREADY) {
    // Already discovering (a completion is still in flight); adopt it.
    rc = 0;
  }
  this->last_start_failed_ = rc != 0;
  if (rc == BLE_HS_EBUSY) {
    // A connect is being initiated; not a failure, the loop retries.
    ESP_LOGV(TAG, "Scan start deferred, radio busy");
    return false;
  }
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
    return false;
  }
  this->programmed_window_ = params.window;
  return true;
}

void NimBLETracker::start_scan_() {
  if (this->scan_running_)
    return;
  if (this->inhibit_count_ != 0) {
    // A connect holds the radio. Continuous scanning resumes from loop() on
    // its own; a one-shot request waits here until the inhibit clears.
    this->pending_start_ = !this->scan_continuous_;
    return;
  }
  if (!this->parent_->is_active()) {
    this->pending_start_ = true;
    return;
  }
  this->set_scanner_state_(ScannerState::STARTING);
  if (!this->controller_scan_start_()) {
    this->set_scanner_state_(this->last_start_failed_ ? ScannerState::FAILED : ScannerState::IDLE);
    return;
  }
  this->scan_running_ = true;
  this->scan_period_start_ = App.get_loop_component_start_time();
  this->set_scanner_state_(ScannerState::RUNNING);
  ESP_LOGD(TAG, "Scan started (%s, window=%.0fms, interval=%.0fms)", this->scan_active_ ? "active" : "passive",
           this->programmed_window_ * BLE_SCAN_UNIT_MS, this->scan_interval_ * BLE_SCAN_UNIT_MS);
}

void NimBLETracker::stop_scan_(bool end_period) {
  if (!this->scan_running_)
    return;
  this->set_scanner_state_(ScannerState::STOPPING);
  int rc = ble_gap_disc_cancel();
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGW(TAG, "ble_gap_disc_cancel failed: %d", rc);
  }
  this->scan_running_ = false;
  if (end_period) {
    this->fire_scan_end_();
  }
  this->set_scanner_state_(ScannerState::STOPPED);
  ESP_LOGV(TAG, "Scan stopped");
}

void NimBLETracker::fire_scan_end_() {
  // Held advertisements whose scan response never came go out before on_scan_end.
  this->merger_.flush();
  this->dispatcher_.on_scan_end();
}

void NimBLETracker::set_scanner_state_(ScannerState state) {
  this->scanner_state_ = state;
  // Every call reports, unchanged states included: a push hub owes the proxy a
  // report for each mode request, and the mode rides along with the state.
  if (this->scanner_state_callback_.is_set()) {
    this->scanner_state_callback_.invoke(state);
  }
}

void NimBLETracker::start_scan() {
#ifdef USE_OTA_STATE_LISTENER
  if (this->ota_in_progress_) {
    this->scan_continuous_before_ota_ = this->scan_continuous_;
    return;
  }
#endif
  if (!this->is_ready() || !this->parent_->is_active()) {
    this->pending_start_ = true;
    return;
  }
  this->start_scan_();
}

void NimBLETracker::stop_scan() {
  this->pending_start_ = false;
  this->scan_continuous_ = false;
  this->stop_scan_(true);
}

void NimBLETracker::restart_scan_duration() {
  // The controller owns the duration timer, so re-anchoring means restarting.
  if (!this->scan_running_)
    return;
  this->stop_scan_(false);
  this->start_scan_();
}

bool NimBLETracker::request_scan_mode(bool active) {
  if (this->scan_active_ != active) {
    this->scan_active_ = active;
    ESP_LOGV(TAG, "Scan mode %s", active ? "active" : "passive");
    if (this->scan_running_) {
      // The scan logically continues: no on_scan_end, just reprogram.
      this->stop_scan_(false);
      this->start_scan_();
      return true;
    }
  }
  // Report even when nothing restarted so the subscriber learns the mode.
  this->set_scanner_state_(this->scanner_state_);
  return true;
}

// ---- GATT backend coordination ----

void NimBLETracker::inhibit_scan() {
  this->connect_attempts_++;
  this->inhibit_count_++;
  this->update_coex_preference_();
  if (this->scan_running_) {
    ESP_LOGD(TAG, "Stopping scan to make connection");
    // A connect ends the scan period (esp32_ble_tracker parity).
    this->stop_scan_(true);
  }
}

void NimBLETracker::release_scan_inhibit() {
  if (this->inhibit_count_ > 0)
    this->inhibit_count_--;
  this->update_coex_preference_();
}

void NimBLETracker::on_link_up() {
  this->links_established_++;
  this->links_up_++;
  this->update_coex_preference_();
}

void NimBLETracker::on_link_down() {
  if (this->links_up_ > 0)
    this->links_up_--;
  this->update_coex_preference_();
}

void NimBLETracker::update_coex_preference_() {
#ifdef USE_NIMBLE_SOFTWARE_COEXISTENCE
  // esp32_ble_tracker parity: prefer Bluetooth while connecting and while any
  // link is up (peer GATT responses must get airtime against WiFi traffic),
  // balanced otherwise.
  bool prefer_bt = this->inhibit_count_ > 0 || this->links_up_ > 0;
  if (prefer_bt == this->coex_prefer_bt_)
    return;
  this->coex_prefer_bt_ = prefer_bt;
  ESP_LOGD(TAG, "Setting coexistence preference to %s", prefer_bt ? "Bluetooth" : "balanced");
  esp_coex_preference_set(prefer_bt ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_BALANCE);
#endif
}

// ---- stack lifecycle ----

void NimBLETracker::on_nimble_synced() {
  // loop() starts the scan once the stack reports active.
  this->last_start_failed_ = false;
}

void NimBLETracker::on_nimble_reset(int reason) {
  // The host already failed the discovery; its completion may still be queued.
  if (this->scan_running_) {
    this->scan_running_ = false;
    this->fire_scan_end_();
  }
  this->set_scanner_state_(ScannerState::FAILED);
}

void NimBLETracker::on_nimble_disabling() {
  if (this->scan_running_) {
    ble_gap_disc_cancel();
    this->scan_running_ = false;
    this->fire_scan_end_();
  }
  this->scan_events_.clear();
  this->set_scanner_state_(ScannerState::IDLE);
}

#ifdef USE_OTA_STATE_LISTENER
void NimBLETracker::on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) {
  if (state == ota::OTA_STARTED) {
    ESP_LOGD(TAG, "Stopping scan for OTA");
    this->scan_continuous_before_ota_ = this->scan_continuous_;
    this->stop_scan();
    this->ota_in_progress_ = true;
  } else if (state == ota::OTA_ERROR || state == ota::OTA_ABORT) {
    this->ota_in_progress_ = false;
    // A successful update reboots; only a failed one resumes here.
    if (this->scan_continuous_before_ota_) {
      this->scan_continuous_before_ota_ = false;
      this->scan_continuous_ = true;
    }
  }
}
#endif

void NimBLETracker::log_stats_(uint32_t now) {
  // The old nimble_proxy "Diag counters" line, for INFO-level builds where the
  // DEBUG scan/connection lines are compiled out. Deltas are per interval.
  const uint32_t elapsed_s = std::max<uint32_t>((now - this->stats_last_ms_) / 1000, 1);
  const uint32_t reports = this->reports_received_ - this->stats_last_reports_;
  const uint32_t scan_rsp = this->scan_responses_received_ - this->stats_last_scan_responses_;
  const uint32_t dropped = this->dropped_reports_ - this->stats_last_dropped_;
  this->stats_last_ms_ = now;
  this->stats_last_reports_ = this->reports_received_;
  this->stats_last_scan_responses_ = this->scan_responses_received_;
  this->stats_last_dropped_ = this->dropped_reports_;
  ESP_LOGI(TAG,
           "Stats: %s %s, %" PRIu32 " reports (%" PRIu32 "/s, %" PRIu32 " scan rsp), %" PRIu32
           " dropped; links %u, connects %" PRIu32 " (%" PRIu32 " established), coex %s",
           scanner_state_to_string(this->scanner_state_), this->scan_active_ ? "active" : "passive", reports,
           reports / elapsed_s, scan_rsp, dropped, this->links_up_, this->connect_attempts_,
           this->links_established_, this->coex_prefer_bt_ ? "bt" : "balanced");
}

void NimBLETracker::dump_config() {
  ESP_LOGCONFIG(TAG,
                "NimBLE Tracker:\n"
                "  Scan Duration: %" PRIu32 " s\n"
                "  Scan Interval: %.1f ms\n"
                "  Scan Window: %.1f ms\n"
                "  Connection Scan Window: %.1f ms\n"
                "  Scan Type: %s\n"
                "  Continuous Scanning: %s\n"
                "  Scanner State: %s",
                this->scan_duration_ / 1000, this->scan_interval_ * BLE_SCAN_UNIT_MS,
                this->scan_window_ * BLE_SCAN_UNIT_MS,
                (this->connection_scan_window_ != 0 ? this->connection_scan_window_ : this->scan_window_) *
                    BLE_SCAN_UNIT_MS,
                this->scan_active_ ? "ACTIVE" : "PASSIVE", YESNO(this->scan_continuous_),
                scanner_state_to_string(this->scanner_state_));
  if (this->dropped_reports_ != 0) {
    ESP_LOGCONFIG(TAG, "  Reports dropped (queue full): %" PRIu32, this->dropped_reports_);
  }
  if (this->stats_interval_ms_ != 0) {
    ESP_LOGCONFIG(TAG, "  Stats log interval: %" PRIu32 " s", this->stats_interval_ms_ / 1000);
  }
}

}  // namespace esphome::nimble_tracker
