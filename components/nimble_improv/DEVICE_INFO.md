# Device Information Service

`nimble_improv` registers a standard BLE Device Information Service (UUID `0x180A`)
next to the Improv service. It is part of the same GATT server, so it is only
reachable while Improv is advertising — that is, while the device needs WiFi
provisioning (see the README's "When it runs").

All characteristics are read-only. Each value is produced when a client reads it
(`nimble_improv.cpp`, the `ATTR_DIS_*` cases), from compile-time configuration
and the chip itself.

## Characteristics

| Characteristic | UUID | Value | Fallback |
|---|---|---|---|
| Manufacturer Name | `0x2A29` | `esphome: project: name` | `ESPHome` |
| Model Number | `0x2A24` | `esphome: friendly_name` | `esphome: name` |
| Serial Number | `0x2A25` | `esphome: name` | — |
| Hardware Revision | `0x2A27` | IDF target, silicon revision and core count, e.g. `esp32s3 rev 0.2 (2 cores)` | — |
| Firmware Revision | `0x2A26` | Firmware build time, e.g. `2026-09-26 15:32:10 -0400` | — |
| Software Revision | `0x2A28` | `esphome: project: version` | `ESPHome <version>`, e.g. `ESPHome 2026.9.0` |

The hardware revision's minor number is the chip's `revision % 100`: an ESP32-S3
v0.2 reads `rev 0.2`.

## Example

```yaml
esphome:
  name: living-room-sensor
  friendly_name: "Living Room Sensor"
  project:
    name: "mycompany.smart-sensor"
    version: "2.1.0"

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

nimble_base:

nimble_improv:
```

A client reading the service sees:

| Characteristic | Value |
|---|---|
| Manufacturer | `mycompany.smart-sensor` |
| Model Number | `Living Room Sensor` |
| Serial Number | `living-room-sensor` |
| Hardware Revision | `esp32s3 rev 0.2 (2 cores)` |
| Firmware Revision | `2026-09-26 15:32:10 -0400` |
| Software Revision | `2.1.0` |

Without `project:` and `friendly_name:` the same device reports `ESPHome`,
`living-room-sensor`, `living-room-sensor`, and `ESPHome 2026.9.0`.

## Viewing it

Any BLE GATT browser shows the service while the device is provisioning, for
example nRF Connect (iOS/Android) or LightBlue (iOS/macOS). Improv clients may
display it during setup.
