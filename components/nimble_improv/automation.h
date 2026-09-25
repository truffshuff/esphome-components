// Improv state triggers for nimble_improv - esp32_improv's five, same semantics.

#pragma once

#ifdef USE_NIMBLE_IMPROV_STATE_CALLBACK
#include "nimble_improv.h"

#include "esphome/core/automation.h"

#include <improv.h>

namespace esphome::nimble_improv {

class NimBLEImprovProvisionedTrigger final : public Trigger<> {
 public:
  explicit NimBLEImprovProvisionedTrigger(NimBLEImprov *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_PROVISIONED && !parent->is_failed())
        this->trigger();
    });
  }
};

class NimBLEImprovProvisioningTrigger final : public Trigger<> {
 public:
  explicit NimBLEImprovProvisioningTrigger(NimBLEImprov *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_PROVISIONING && !parent->is_failed())
        this->trigger();
    });
  }
};

class NimBLEImprovStartTrigger final : public Trigger<> {
 public:
  explicit NimBLEImprovStartTrigger(NimBLEImprov *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if ((state == improv::STATE_AUTHORIZED || state == improv::STATE_AWAITING_AUTHORIZATION) && !parent->is_failed())
        this->trigger();
    });
  }
};

class NimBLEImprovStateTrigger final : public Trigger<improv::State, improv::Error> {
 public:
  explicit NimBLEImprovStateTrigger(NimBLEImprov *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (!parent->is_failed())
        this->trigger(state, error);
    });
  }
};

class NimBLEImprovStoppedTrigger final : public Trigger<> {
 public:
  explicit NimBLEImprovStoppedTrigger(NimBLEImprov *parent) {
    parent->add_on_state_callback([this, parent](improv::State state, improv::Error error) {
      if (state == improv::STATE_STOPPED && !parent->is_failed())
        this->trigger();
    });
  }
};

}  // namespace esphome::nimble_improv

#endif  // USE_NIMBLE_IMPROV_STATE_CALLBACK
