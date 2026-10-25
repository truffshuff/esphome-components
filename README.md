# `ESPHome` components

ESPHome external components, forked from
[buglloc/esphome-components](https://github.com/buglloc/esphome-components) (via
[yashmulgaonkar/esphome-components](https://github.com/yashmulgaonkar/esphome-components)).
They are developed alongside the [Halo](https://github.com/truffshuff/halo) air-quality
display (LilyGo T-Display S3 Long) and target **ESPHome 2026.10.0 or newer** on ESP-IDF.

Use it from your YAML. Pin a commit SHA so a push here cannot change your build:
```yaml
external_components:
  - source: github://truffshuff/esphome-components@<commit-sha>
    components: [ axs15231, weather_helpers ]
```

Active development is on the `nimble-bluedroid-parity` branch. You can take a look at samples
of usage of those components in [examples](examples) folder.

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

## [weather_helpers](components/weather_helpers)

Header-only helpers for weather pages built from Home Assistant's `weather.get_forecasts`
data: an MDI icon glyph and colour per condition, ISO-8601 timestamp parsing into the
device's local date and hour, and formatting for temperatures, percentages and
precipitation. There is no C++ object; listing the component copies the header into the
build and lambdas call `weather_helpers::...`:

```yaml
external_components:
  - source: github://truffshuff/esphome-components@<commit-sha>
    components: [ weather_helpers ]

weather_helpers:

time:
  - platform: sntp
    timezone: America/New_York   # local dates/hours come from localtime_r()

# in a lambda:
#   return weather_helpers::get_weather_icon(id(condition));
#   return weather_helpers::format_hour(weather_helpers::convert_to_local_hour(id(datetime)));
```

The icon glyphs must be included in your MDI icon font's `glyphs:` list; the header's
"Weather Icon Mapping" section lists them.

## [PinkyWinky](components/pinky_winky) integration

[PinkyWinky](https://github.com/buglloc/pinky-winky/) (BLE beacon) integration.

Requirements:
  - ESP-IDF and a BLE tracker: `esp32_ble_tracker` (Bluedroid) or `nimble_tracker` (NimBLE)
  - This fork's copy: it binds through `ble_device_base`, which is what lets it run on
    `nimble_tracker`. The original buglloc component supports `esp32_ble_tracker` only.

Minimal example:
```yaml
external_components:
  - source: github://truffshuff/esphome-components@<commit-sha>
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
| [nimble_improv](components/nimble_improv) | `improv_ble` (`esp32_improv` before 2026.10.0) | Improv WiFi provisioning |

Home Assistant sees the same proxy behaviour as from a Bluedroid proxy, with a
smaller flash and internal-RAM footprint. Parity status and the hardware test
plan are in [docs/NIMBLE_PARITY.md](docs/NIMBLE_PARITY.md). How the vendored
components are maintained is in [components/VENDORED.md](components/VENDORED.md).
Examples: [examples/nimble](examples/nimble).

`nimble_proxy` is retired; its README lists the replacement config.
