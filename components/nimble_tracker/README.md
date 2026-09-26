# nimble_tracker

The NimBLE BLE scanner: it stands in for `esp32_ble_tracker` on a NimBLE build.
It implements ESPHome 2026.9.0's neutral `BLEHub` contract, so anything that
binds through `ble_device_base` works against it unchanged. That includes
`bluetooth_proxy`, the `on_ble_*` triggers, and on-device sensors such as
`ble_presence`, `ble_rssi`, `ble_scanner`, `xiaomi_*`, `atc_mithermometer`,
`pvvx_mithermometer`, `ruuvitag`, `bthome_mithermometer` and `pinky_winky`.

## Configuration

Same surface as `esp32_ble_tracker`:

```yaml
nimble_base:

nimble_tracker:
  id: ble_tracker
  scan_parameters:
    interval: 320ms
    window: 30ms              # see "Scan window default" below
    connection_scan_window: 30ms
    duration: 5min            # one scan period; on_scan_end fires at each
    active: true
    continuous: true
  on_ble_advertise:
    - then:
        - lambda: 'ESP_LOGD("ble", "%s", x.address_str_to(buf));'
  on_ble_service_data_advertise: ...
  on_ble_manufacturer_data_advertise: ...
  on_scan_end: ...
```

### Actions

```yaml
- nimble_tracker.start_scan:
    id: ble_tracker
    continuous: true
- nimble_tracker.stop_scan:
    id: ble_tracker
```

A pause/resume switch, as halo uses around OTA and HTTP fetches:

```yaml
switch:
  - platform: template
    name: "BLE Scanner"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_ON
    on_turn_on:
      - nimble_tracker.start_scan: { id: ble_tracker, continuous: true }
    on_turn_off:
      - nimble_tracker.stop_scan: ble_tracker
```

## Behaviour (matches esp32_ble_tracker)

- **No duplicate filtering.** Every advertisement is reported. Home Assistant
  needs repeats for RSSI and for payloads that change under one address.
- **Advertisement and scan response merged.** NimBLE reports them separately;
  they are merged into one frame (up to 62 bytes) by ESPHome's own
  `ScanResponseMerger`, which is what the IDF Bluedroid stack delivers.
- **Scan periods.** A continuous scan runs in `duration` periods and restarts;
  `on_scan_end` fires at the end of each.
- **Connections.** The scan stops for every outgoing connect (NimBLE cannot
  connect while scanning) and resumes afterwards. While a link is up, the
  window drops to `connection_scan_window` and coexistence prefers Bluetooth.
- **OTA.** Scanning pauses for the update.
- **Scanner state.** Every transition is pushed to the proxy, along with the
  real and configured scan modes.

### Scan window default

This follows ESPHome 2026.9.0. With WiFi configured on ESP-IDF 5.5.5 or later,
an unset `window` is raised to `interval` (the coexistence arbiter shares the
radio), and `connection_scan_window` defaults to 30 ms. Without WiFi, the window
defaults to 30 ms. An explicit `window` is never changed, and a window above
600 ms with WiFi logs a warning.

### Differences

- A scan period that never completes is restarted. `esp32_ble_tracker` reboots
  the device in that case.
- `log_stats_interval` (default `0s`, off; minimum `10s`) is an addition. It
  prints one INFO line per interval with the scanner state and mode, reports
  and scan responses received, dropped reports, open links, connect attempts,
  established links and the coexistence preference:

  ```
  [I][nimble_tracker]: Stats: RUNNING active, 5412 reports (90/s, 1873 scan rsp), 0 dropped; links 0, connects 0 (0 established), coex balanced
  ```

  It is meant for builds whose logger runs at INFO, where the tracker's DEBUG
  lines are compiled out. The same counters are available to template sensors
  through `get_reports_received()`, `get_dropped_reports()`,
  `get_active_links()`, `get_connect_attempts()` and
  `get_links_established()`.

## Migrating from `nimble_proxy`

`nimble_proxy` is retired: configs that still use it fail with instructions.
Its scan options map as follows:

| `nimble_proxy` | now |
|---|---|
| `scan_interval: 512` (units of 0.625 ms) | `nimble_tracker: scan_parameters: interval: 320ms` |
| `scan_window: 96` | `window: 60ms` |
| `scan_active: true` | `active: true` |
| `scan_duplicate_filter`, `scan_duplicate_cache_size` | removed (Bluedroid does not filter) |
| `advertisement_send_interval_ms`, `advertisement_batch_size` | removed (upstream batching) |
| `connection_slots` / `max_connections` | `bluetooth_proxy: connection_slots:` |
| `switch: platform: nimble_proxy` | template switch above |
