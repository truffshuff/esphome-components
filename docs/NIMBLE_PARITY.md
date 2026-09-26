# NimBLE vs ESPHome 2026.9.0 Bluedroid: parity status

This covers the NimBLE stack in this repository (`nimble_base`,
`nimble_tracker`, the vendored `bluetooth_proxy` / `bluetooth_connection` /
`ble_device_base`, `nimble_improv`), compared with ESPHome 2026.9.0's built-in
Bluedroid stack (`esp32_ble`, `esp32_ble_tracker`, `bluetooth_proxy`,
`esp32_improv`).

**Status: source-proven, partly hardware-verified.** Every row marked parity
was checked against the ESPHome 2026.9.0 and NimBLE (ESP-IDF 5.5.5) sources.
Three builds compile cleanly: the proxy, the proxy plus on-device sensors, and
the full halo device. The last section lists the hardware tests that turn
"source-proven" into "verified".

**Hardware results so far** (halo-v1-79e384, 1 slot, 2026-09-25/26):

- Scanning overnight (7 h, ~30 reports/s) with no crash. With the scan ring at
  256 slots, 0 reports are dropped through 2.5 s main-loop stalls.
- HA connects while scanning. The scan stops for the connect and resumes
  after it with `connection_scan_window`. The slot is freed and the scan window
  and coexistence are restored on every disconnect, including connects that HA
  cancels after 20 s.
- Uncached connect: MTU 247, discovery done in ~3.4 s. Reconnect with HA's
  cache (`V3_WITH_CACHE`): connected in ~0.3 s with no discovery.
- A HomeKit accessory paired over BLE through the proxy (pair-setup, local
  disconnect, cached reconnect for pair-verify).
- Min free internal heap ~63-67 KB, the same as the previous fork. An open
  link did not change free internal heap.

Not yet run: the remaining rows of the test plan, notably notify/indicate
rates, long writes, pair/unpair from HA, host reset, OTA with a link open, and
the Bluedroid side of each comparison.

## How parity is achieved

The proxy runs ESPHome's own `bluetooth_proxy` and `bluetooth_connection`
code. The files that implement the Home Assistant protocol are byte-identical
to upstream (see `components/VENDORED.md`). Only two pieces are NimBLE-specific:

- `nimble_tracker` implements upstream's `BLEHub` contract (scanning).
- `NimBLEGattClient` implements upstream's `BLEGattConnection` contract (GATT).

Reply types, the connection lifecycle, service streaming, owed-reply retries,
slot accounting, feature flags and scanner-state reporting are therefore
upstream code.

## Parity matrix

| Area | Status | Notes |
|---|---|---|
| Stack init, failure marking | Parity | `nimble_base` marks itself and its dependents failed, like `esp32_ble`. The host task starts directly; the old 15 s fallback is gone. |
| `ble.enable` / `ble.disable` | Parity (HW) | `nimble_base.enable` / `.disable` / `.is_enabled`. Listeners settle links before the stack stops, like `ble_before_disabled`. |
| Host reset recovery | Parity (HW) | Scan restarts after re-sync; links report disconnected. |
| OTA | Parity | The tracker pauses scanning and every backend disconnects, like `esp32_ble_tracker`. |
| Continuous scanning, scan periods, `on_scan_end` | Parity | Duration-based periods, restarted from `loop()`. A stuck scan is restarted rather than rebooting the device (deliberate, see below). |
| Active / passive, runtime mode switch | Parity | Upstream proxy path + `request_scan_mode()`. The scanner switch no longer shares this entry point. |
| Scan interval / window / `connection_scan_window` | Parity | Same schema and defaults, including the 2026.9.0 WiFi window raise. |
| Duplicate filtering | Parity | None, like Bluedroid. |
| Advertisement + scan response | Parity | Merged into one ≤62-byte frame by upstream's `ScanResponseMerger`. |
| Raw advertisement batching | Parity | Upstream's 16-frame batches and ~200 ms flush. |
| Scanner state reporting | Parity | IDLE / STARTING / RUNNING / STOPPING / STOPPED / FAILED, real and configured mode, retry on refusal. |
| On-device consumers | Parity for neutral consumers | Anything binding through `ble_device_base` works: `ble_presence`, `ble_rssi`, `ble_scanner`, `xiaomi_*`, `atc_mithermometer`, `pvvx_mithermometer`, `ruuvi*`, `bthome_mithermometer`, `pinky_winky`, the `on_ble_*` triggers, and others. |
| `ble_client`, `esp32_ble_server`, `esp32_ble_beacon` | **Not available** | Tied to Bluedroid (`esp32_ble_tracker` / `esp32_ble`) upstream. See "Remaining differences". |
| Connect | Parity (HW) | Scan stopped first, one stack-wide connect at a time (queued), FAST/MEDIUM parameters, configurable `connection_timeout` (20 s). |
| Connected reply | Parity | Sent after MTU exchange (and after discovery for uncached connects). Cached connects skip discovery. |
| MTU | Parity | Exchanged on every connect; 517 offered. |
| Service / characteristic / descriptor discovery | Parity | Full discovery into a transient table streamed by upstream's streamer (≤1360-byte batches). Oversize databases fail cleanly instead of streaming truncated. |
| 128-bit UUIDs | Parity | Upstream packing. |
| Read (incl. long values) | Parity | Read-long reassembly, up to 4 reads in flight. |
| Write with / without response, long writes | Parity | `BluetoothGATTWriteResponse`; prepared writes above MTU−3; oversize write-without-response is refused instead of truncated. |
| Descriptor read / write | Parity | |
| Notify / indicate | Parity | Local registration + `BluetoothGATTNotifyResponse`; the client writes the CCCD. NimBLE confirms indications. |
| Pair / unpair / clear cache | Parity | Upstream reply types. Unpair removes NimBLE bonds for every address type. Clear cache succeeds (NimBLE keeps no stack-side GATT cache). |
| Bond store | Parity | Oldest bond recycled when full; re-pairing after the peer lost its bond. |
| Connection params | Parity | |
| Error codes | Parity | NimBLE codes mapped into Bluedroid's space: ATT codes, HCI reasons, `-1` not connected, 0x84/0x85/0x87 stack codes. |
| Disconnect, cancel-while-connecting, unknown address, no free slot | Parity | Upstream wrapper plus a backend that cancels in-flight connects. |
| API subscriber loss | Parity | Upstream proxy tears the connections down. |
| Owed replies under TCP backpressure | Parity | Upstream latches. |
| Coexistence | Parity | Software coexistence requested; Bluetooth preferred while connecting and while links are up. |
| Security config | Partial | `io_capability`, `auth_req_mode`. `max_key_size` / `min_key_size` have no runtime NimBLE equivalent. |
| Adapter MAC | Parity+ | Valid before the host syncs (`esp_read_mac`). |
| Improv | Parity | `esp32_improv`'s lifecycle, authorizer, identify, status indicator, advertising and triggers. |
| `cache_services` | Accepted, no effect | Bluedroid's stack-side NVS cache; NimBLE has none. |

## Remaining differences

**Stack or ESPHome architecture.** These can't be closed from an external
component without re-implementing upstream components:

- `ble_client`, `esp32_ble_server`, `esp32_ble_beacon` and `esp32_improv` are
  written against Bluedroid APIs upstream. `nimble_improv` replaces
  `esp32_improv`. The others have no NimBLE counterpart here.
- `max_key_size` / `min_key_size` are compile-time in NimBLE.
- Improv start/stop: the WiFi component calls `esp32_improv` directly, so
  `nimble_improv` watches WiFi state itself. The triggers are the same.
- A `wifi:` block with no network is only accepted by ESPHome's schema when
  `esp32_improv` or `improv_serial` is configured. With `nimble_improv` alone,
  add a fallback `ap:` (Improv still starts, since no network is stored).

**Deliberate:**

- A scan that never completes is restarted. `esp32_ble_tracker` reboots
  instead, but halo runs with reboot timeouts disabled to keep in-RAM history.
- `nimble_improv` exposes a Device Information Service and keeps `authorizer`
  optional.
- `cache_services` is accepted and has no effect.

**Unverified (hardware):** every row marked (HW) above, plus the tests below.

## Hardware test plan

Run each test once on NimBLE and once on Bluedroid (halo `ble_esphome.yaml`)
and compare.

| Test | Expected (both stacks) |
|---|---|
| HA connects to a peripheral while scanning | Connects; scan resumes after |
| GetServices on a device with 128-bit services | UUIDs match nRF Connect |
| `start_notify` on notify-only and indicate-only characteristics | Succeeds; data flows |
| Write with response: ≤20, 21–512 bytes | Acked; full payload on the peer |
| Reconnect with HA's cache (`V3_WITH_CACHE`) | No discovery; operations work immediately |
| Pair / unpair / clear cache from HA | Correct response types, no "connection dropped" |
| HA scanner mode passive → active | Mode switches; state reported |
| Static iBeacon for 10 min | RSSI updated on every advertisement |
| BTHome / Xiaomi sensor value change | Every change seen |
| Device name only in its scan response | One merged frame |
| Host reset (`ble_hs_sched_reset`) | Scan resumes; links reported down |
| API disconnect with a link open | Link torn down |
| OTA with a link open | Link closed, scan paused |
| HA disconnect while connecting | Slot freed cleanly |
| Two simultaneous HA connects (2 slots) | Serialized; both connect |
| `nimble_base.disable` then `.enable` | Links reported down; scanning resumes |
| 8 h scan with WiFi load | No growth in dropped reports; WiFi throughput normal |
| ≥50 Hz notifications for 10 min | No drops (`dump_config` counter) |
| Improv with WiFi connected | Not advertising |
| Improv with no stored network | Advertises; provisions; stops 10 s after |
| Min free internal heap, idle and 1 link | Record against docs/MEMORY.md in halo |
