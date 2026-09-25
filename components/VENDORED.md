# Vendored ESPHome components

Three components in this directory are copies of ESPHome's own, carried here so
a NimBLE build can run ESPHome's Bluetooth proxy unchanged:

| Component | Upstream | Why it is vendored |
|---|---|---|
| `ble_device_base` | ESPHome 2026.9.0 | `ble_hub_impl.h` binds the `BLEHub` alias with a closed `#if` ladder, and `ble_device.h` includes Bluedroid headers under `USE_ESP32`. |
| `bluetooth_connection` | ESPHome 2026.9.0 | The GATT-backend ladder picks Bluedroid on `USE_ESP32_BLE`, and its codegen registry only knows Bluedroid and rp2. |
| `bluetooth_proxy` | ESPHome 2026.9.0 | The esp32 arm of the codegen hard-wires `esp32_ble_tracker`, and three `USE_ESP32` guards call tracker-only methods. |

ESPHome has no extension point for a second ESP32 BLE stack, so an external
component with the same name overrides the built-in one. Because of that,
these copies must only be loaded by NimBLE builds. A Bluedroid build
(halo's `ble_esphome.yaml`) never lists them in `external_components`.

## What is and is not changed

The files that implement the Home Assistant protocol are **byte-identical to
upstream**, so the proxy's behaviour stays upstream's:

- `bluetooth_connection/bluetooth_connection_hub.{h,cpp}`: per-slot connection
  wrapper, owed-reply retries, service streaming.
- `bluetooth_connection/bluetooth_connection.h`: error domain, capability
  flags, UUID packing.
- `ble_device_base/ble_gatt_client.h`, `ble_hub.h`, `scan_response_merger.*`,
  `automation.*`, `ble_client_state.*`, `ble_aes_ccm.*`.

Every C++ change is a guard, marked `[nimble]`:

| File | Change |
|---|---|
| `ble_device_base/ble_device.h` (4 places), `ble_device.cpp` (1) | `#ifdef USE_ESP32` → `#if defined(USE_ESP32) && !defined(USE_NIMBLE_BLE)` around the Bluedroid-typed adapters (`esp_bt_uuid_t`, `BLEScanResult`). |
| `ble_device_base/ble_hub_impl.h` | New first arm: `USE_NIMBLE_BLE_TRACKER` → `nimble_tracker::NimBLETracker`. |
| `bluetooth_connection/bluetooth_connection.cpp` | Bluedroid includes and the Bluedroid `unpair_device` / `clear_gatt_cache` excluded under `USE_NIMBLE_BLE`. The NimBLE versions live in `bluetooth_connection_nimble.cpp`. |
| `bluetooth_connection/bluetooth_connection_gatt_backend.h` | New first arm: `USE_NIMBLE_BLE` → `NimBLEGattClient`. |
| `bluetooth_proxy/bluetooth_proxy.h` | Setup priority and feature flags take the neutral-hub path under `USE_NIMBLE_BLE`. |
| `bluetooth_proxy/bluetooth_proxy.cpp` | `bluetooth_scanner_set_mode()` takes the neutral `request_scan_mode()` path under `USE_NIMBLE_BLE`. |

The neutral-hub path the proxy switches to is upstream's own. It is the code
rp2, bk72xx and ln882x proxies run.

The Python modules are different:

- `ble_device_base/__init__.py`: upstream, plus a header note and `nimble_tracker`
  as the ESP32 hub name in the "no tracker configured" error.
- `bluetooth_connection/__init__.py`: rewritten for the single NimBLE backend.
  The helper names and signatures the proxy calls are unchanged.
- `bluetooth_proxy/__init__.py`: rewritten for ESP32 + NimBLE. The YAML
  surface is upstream's esp32 surface (`active`, `connection_slots`,
  `cache_services`, `connections`), plus `ble_hub_id`.

Not carried: `bluetooth_connection_bluedroid.*` and `bluetooth_connection_rp2.*`.

## Added (not upstream)

- `bluetooth_connection/bluetooth_connection_nimble.{h,cpp}`: the NimBLE
  implementation of `BLEGattConnectionContract`.
- `nimble_base`, `nimble_tracker`, `nimble_improv`.

## Re-syncing with a new ESPHome release

1. Copy the three upstream directories over these (keep
   `bluetooth_connection_nimble.*`; drop the Bluedroid and rp2 backend files).
2. Re-apply the guard changes in the table above. `grep -rn "\[nimble\]"` on
   the previous version lists every one.
3. Diff the upstream Python modules against the previous release and carry any
   new keys, defines or helper signatures into the rewritten `__init__.py`
   files.
4. Check the contracts for drift: `BLEHubContract` (`ble_hub.h`) and
   `BLEGattConnectionContract` (`ble_gatt_client.h`). The code fails to compile
   when a method is missing, so a clean compile of
   `examples/nimble/nimble_both.yaml` confirms it.
5. Bump the version in this file and in the three `__init__.py` headers.
