"""Retired: nimble_proxy was replaced by bluetooth_proxy + nimble_tracker.

The NimBLE Bluetooth proxy now runs ESPHome 2026.9.0's own bluetooth_proxy
and bluetooth_connection code (vendored in this repository) on a NimBLE
tracker and GATT backend, so Home Assistant sees the same protocol behaviour
as a Bluedroid proxy. This stub only exists to turn an old configuration into
an actionable error instead of "component not found".
"""

import esphome.config_validation as cv

CODEOWNERS = ["@truffshuff"]

MIGRATION = """nimble_proxy has been replaced. Change the configuration to:

  external_components:
    - source: github://truffshuff/esphome-components@<ref>
      components: [nimble_base, nimble_tracker, ble_device_base,
                   bluetooth_connection, bluetooth_proxy]

  nimble_base:
  nimble_tracker:
    scan_parameters:          # was scan_interval / scan_window / scan_active
      interval: 320ms
      window: 60ms
      active: true
  bluetooth_proxy:
    active: true
    connection_slots: 1

Removed options: scan_duplicate_filter and scan_duplicate_cache_size (Home
Assistant needs every advertisement, as with Bluedroid), advertisement_send_interval_ms
and advertisement_batch_size (upstream batching), advertising_name (Improv
advertising belongs to nimble_improv), max_connections (use connection_slots).
The scanner switch (switch: platform: nimble_proxy) becomes a template switch
calling nimble_tracker.start_scan / nimble_tracker.stop_scan. See the
nimble_tracker README."""


def _retired(config):
    raise cv.Invalid(MIGRATION)


CONFIG_SCHEMA = _retired
