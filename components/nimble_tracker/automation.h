// Scan-control actions for nimble_tracker (same semantics as
// esp32_ble_tracker.start_scan / stop_scan and rp2_ble_tracker's copies). The
// advertisement triggers are the neutral ble_device_base classes.

#pragma once

#include "nimble_tracker.h"

#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"

namespace esphome::nimble_tracker {

template<typename... Ts> class StartScanAction final : public Action<Ts...>, public Parented<NimBLETracker> {
 public:
  TEMPLATABLE_VALUE(bool, continuous)
  void play(const Ts &...x) override {
    // Without continuous: the configured value is used - stop_scan() clears the
    // runtime flag, so a bare stop/start pair would otherwise never resume
    // continuous scanning.
    const bool want =
        this->continuous_.has_value() ? this->continuous_.value(x...) : this->parent_->configured_continuous();
    if (this->parent_->scan_running()) {
      // Same mode on a running scan is a no-op (esp32 parity); a real mode
      // switch re-anchors so a switch to one-shot runs a full duration.
      if (want != this->parent_->scan_continuous()) {
        this->parent_->set_scan_continuous(want);
        this->parent_->restart_scan_duration();
      }
      return;
    }
    this->parent_->set_scan_continuous(want);
    this->parent_->start_scan();
  }
};

template<typename... Ts> class StopScanAction final : public Action<Ts...>, public Parented<NimBLETracker> {
 public:
  void play(const Ts &...x) override { this->parent_->stop_scan(); }
};

}  // namespace esphome::nimble_tracker
