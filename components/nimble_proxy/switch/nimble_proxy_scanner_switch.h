#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "../nimble_proxy.h"

namespace esphome {
namespace nimble_proxy {

class NimBLEProxyScannerSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_parent(NimBLEProxy *parent) { this->parent_ = parent; }

 protected:
  void write_state(bool state) override;

  NimBLEProxy *parent_{nullptr};
};

}  // namespace nimble_proxy
}  // namespace esphome
