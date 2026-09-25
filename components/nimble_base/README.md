# nimble_base

The NimBLE counterpart of ESPHome's `esp32_ble`. It brings up the ESP32 BLE
controller and the NimBLE host, applies the security-manager configuration,
owns GATT-server service registration, and delivers the host's sync and reset
events on the ESPHome main loop.

Every other NimBLE component here builds on it: `nimble_tracker`, the vendored
`bluetooth_proxy` / `bluetooth_connection`, and `nimble_improv`.

## Configuration

```yaml
nimble_base:
  # All optional; defaults shown.
  name: <App name>          # GAP device name, max 29 chars
  io_capability: none       # display_only | display_yes_no | keyboard_only | keyboard_display | none
  auth_req_mode: sc_bond    # no_bond | bond | mitm | bond_mitm | sc_only | sc_bond | sc_mitm | sc_mitm_bond
  enable_on_boot: true
  connection_timeout: 20s   # outgoing connects (10s - 180s)
  use_psram: false          # host allocations in PSRAM (needs psram:)
```

The options follow `esp32_ble`'s names. `max_key_size` / `min_key_size` are not
offered because NimBLE fixes them at build time.

### Actions and conditions

```yaml
- nimble_base.disable:     # ble.disable equivalent
- nimble_base.enable:      # ble.enable equivalent
- if:
    condition: nimble_base.is_enabled
```

Disabling first tells every component to settle (links are reported
disconnected, scanning and advertising stop), then stops and de-initialises the
stack. Enabling starts it again from scratch.

## What it sets up for you

- It requests the IDF `bt` component (ESPHome 2026.9.0 excludes it by default)
  and, when WiFi is configured, WiFi/BT software coexistence (2026.7.0 only
  enables it on request).
- It supplies sdkconfig defaults. A value you set in `sdkconfig_options` always
  wins:
  - `CONFIG_BT_NIMBLE_ENABLED` on, `CONFIG_BT_BLUEDROID_ENABLED` off.
  - `CONFIG_BT_NIMBLE_MAX_CONNECTIONS`: the sum of the slots components reserve
    (`bluetooth_proxy` connection slots, plus one for `nimble_improv`).
    Setting it lower yourself fails validation.
  - `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`: 517, the MTU Bluedroid offers.
  - `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` with `use_psram: true`.
- A full bond store evicts its oldest bond (`ble_store_util_status_rr`) instead
  of failing new pairings.

## For component authors

```cpp
#include "esphome/components/nimble_base/nimble_base.h"

class MyThing : public Component, public nimble_base::NimBLEStackListener {
  void setup() override { nimble_base::global_nimble_base->add_listener(this); }
  void on_nimble_synced() override { /* GAP/GATT calls are valid now */ }
  void on_nimble_reset(int reason) override { /* links and procedures are gone */ }
  void on_nimble_disabling() override { /* stop radio use now */ }
};
```

All three callbacks run on the main loop. NimBLE's own callbacks run on its
host task: copy what you need there (see `nimble_event_ring.h`) and act on it
in `loop()`.

GATT services must be registered before `setup()`, typically from your
constructor:

```cpp
nimble_base::NimBLEBase::register_gatt_services(my_service_table);
```

Python components reserve connection slots the way they do with `esp32_ble`:

```python
from esphome.components import nimble_base
CONFIG_SCHEMA = cv.All(MY_SCHEMA, nimble_base.consume_connection_slots(1, "my_component"))
```
