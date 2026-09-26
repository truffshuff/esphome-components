# `ESPHome` components

A collection of my ESPHome components. Tested on ESP Home v2024.12.2 and may be broken on older ones :)

To use this repository you should confugure it inside your yaml-configuration:
```yaml
external_components:
  - source: github://buglloc/esphome-components
    refresh: 10min
```

You can take a look at samples of usage of those components in [examples](examples) folder.

## [axs15231](components/axs15231) touchscreen

Touch driver for the [AXS15231](docs/datasheet/AXS15231_Datasheet_V0.4_20221108.pdf) on the [T-Display S3 Long](https://www.lilygo.cc/products/t-display-s3-long). Use it with ESPHome's own `mipi_spi` display (`model: AXS15231`).

ESPHome ships an `axs15231` touchscreen too. This one differs in two ways:
  - It maps the panel's X/Y the way the T-Display S3 Long reports them.
  - It reports single-finger touch events only, which filters the phantom touches the panel produces when the backlight turns off ([LilyGo issue #30](https://github.com/Xinyuan-LilyGO/T-Display-S3-Long/issues/30)).

Minimal example:
```yaml
external_components:
  - source: github://truffshuff/esphome-components
    components: [ axs15231 ]

i2c:
  - id: touch_i2c
    sda: 15
    scl: 10
    frequency: 400kHz

touchscreen:
  - platform: axs15231
    display: lily_display
    i2c_id: touch_i2c
    interrupt_pin: GPIO11
```

See the [full example](examples/axs15231/t-display-s3-long-touch.yaml) in the [examples](examples) folder.

The earlier `axs15231` display platform and `sy6970` PMU component were removed: ESPHome's `mipi_spi` supports this display, and ESPHome now has its own `sy6970` component.

## [PinkyWinky](components/pinky_winky) integration

[PinkyWinky](https://github.com/buglloc/pinky-winky/) (BLE beacon) integration.

Requirements:
  - ESP-IDF and a BLE tracker: `esp32_ble_tracker` (Bluedroid) or `nimble_tracker` (NimBLE)

Minimal example:
```yaml
external_components:
  - source: github://buglloc/esphome-components
    components: [ pinky_winky ]

time:
  - platform: homeassistant

esp32_ble_tracker:

pinky_winky:
  - id: my_pinky
    mac_address: "11:22:33:44:55:66"
    secret: "so-secret-much-strong"

binary_sensor:
  - platform: pinky_winky
    id: my_pinky_button
    input: my_pinky
    name: "Pinky button"
```

See the [full example](examples/pinky_winky/bike-xiao-esps3.yaml) for usage with  [Seeed Studio XIAO ESP32S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) in the [examples](examples) folder.

Post about: [Open Sesame: PinkyWinky](https://ut.buglloc.com/2024/06/open-sesame/)

## NimBLE Bluetooth stack

An alternative to ESPHome's Bluedroid-based BLE components, for ESPHome
2026.9.0 and later:

| Component | Replaces | |
|---|---|---|
| [nimble_base](components/nimble_base) | `esp32_ble` | Stack bring-up, security, enable/disable |
| [nimble_tracker](components/nimble_tracker) | `esp32_ble_tracker` | Scanning, `on_ble_*` triggers, BLE sensors |
| [bluetooth_proxy](components/bluetooth_proxy) + [bluetooth_connection](components/bluetooth_connection) + [ble_device_base](components/ble_device_base) | the built-in ones | ESPHome's own proxy code, vendored with a NimBLE backend |
| [nimble_improv](components/nimble_improv) | `esp32_improv` | Improv WiFi provisioning |

Home Assistant sees the same proxy behaviour as from a Bluedroid proxy, with a
smaller flash and internal-RAM footprint. Parity status and the hardware test
plan are in [docs/NIMBLE_PARITY.md](docs/NIMBLE_PARITY.md). How the vendored
components are maintained is in [components/VENDORED.md](components/VENDORED.md).
Examples: [examples/nimble](examples/nimble).

`nimble_proxy` is retired; its README lists the replacement config.
