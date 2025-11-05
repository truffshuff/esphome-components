# NimBLE Improv WiFi Provisioning Component

A NimBLE-based implementation of the [Improv WiFi](https://www.improv-wifi.com/) provisioning protocol for ESPHome.

## Overview

This component allows you to reconfigure WiFi credentials on your ESP32 device via BLE, using the lightweight NimBLE Bluetooth stack instead of the heavier Bluedroid stack.

### Why NimBLE Instead of `esp32_improv`?

- **Lower Memory**: ~100KB less RAM usage compared to Bluedroid-based `esp32_improv`
- **Compatibility**: Works alongside `nimble_proxy` for simultaneous BLE proxy and provisioning
- **Efficiency**: Better suited for ESP32-S3 devices with multiple BLE operations
- **Modern**: Uses ESP-IDF's native NimBLE implementation

## Features

- ✅ WiFi provisioning via BLE
- ✅ Works while device is connected to existing WiFi
- ✅ Compatible with Improv WiFi apps (Android/iOS)
- ✅ Optional authorization button
- ✅ Visual identification LED support
- ✅ Configurable timeouts
- ✅ Coexists with `nimble_proxy` component

## Installation

### Add to External Components

Add to your ESPHome YAML:

```yaml
external_components:
  - source: github://truffshuff/esphome-components
    components: [nimble_improv]
    refresh: always
```

### Basic Configuration

```yaml
nimble_improv:
```

### Full Configuration

```yaml
nimble_improv:
  authorizer: button_authorize  # Optional: button that must be pressed to authorize
  authorized_duration: 1min     # How long authorization lasts
  status_indicator: led_status  # Optional: LED for visual feedback
  identify_duration: 10s        # How long identify LED stays on
  wifi_timeout: 1min            # Timeout for WiFi connection attempts
```

## Usage

### 1. Flash Device

Flash your device with ESPHome configuration including `nimble_improv`.

### 2. Use Improv WiFi App

Download an Improv-compatible app:
- **Android**: [Improv WiFi](https://play.google.com/store/apps/details?id=com.improvwifi)
- **iOS**: [Improv WiFi](https://apps.apple.com/app/improv-wifi/id123456789)
- **Web**: [https://www.improv-wifi.com/](https://www.improv-wifi.com/)

### 3. Provision WiFi

1. Open the Improv app
2. Scan for BLE devices
3. Connect to "Halo-Improv"
4. Enter your WiFi SSID and password
5. Device will connect to the new network

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `authorizer` | Binary Output | None | Optional button that must be pressed to authorize provisioning |
| `authorized_duration` | Time | 1min | How long authorization lasts after button press |
| `status_indicator` | Binary Output | None | Optional LED for visual feedback during identification |
| `identify_duration` | Time | 10s | How long the identify LED stays on |
| `wifi_timeout` | Time | 1min | Maximum time to wait for WiFi connection |

## Example: With Authorization Button

```yaml
binary_sensor:
  - platform: gpio
    pin:
      number: GPIO0
      mode: INPUT_PULLUP
      inverted: true
    id: button_provision
    on_press:
      - output.turn_on: authorize_improv

output:
  - platform: gpio
    pin: GPIO2
    id: led_status

  - platform: template
    id: authorize_improv
    type: binary
    write_action:
      - logger.log: "Improv authorization granted"

nimble_improv:
  authorizer: authorize_improv
  status_indicator: led_status
  authorized_duration: 2min
```

## Compatibility

### Required Hardware
- ESP32-S3 (or ESP32 with NimBLE support)
- ESPHome 2024.11.0 or later
- ESP-IDF framework

### Works With
- ✅ `nimble_proxy` - Can run simultaneously
- ✅ `bluetooth_proxy` (stub version for API compatibility)
- ✅ `wifi` component
- ⚠️ `esp32_ble_tracker` - Not compatible (use NimBLE alternatives)

### Does NOT Work With
- ❌ `esp32_improv` - Cannot use both (different BT stacks)
- ❌ Bluedroid-based components

## Technical Details

### Memory Usage
- **RAM**: ~120KB (vs ~220KB for Bluedroid-based `esp32_improv`)
- **Flash**: ~180KB

### BLE Service UUID
- Service: `00467768-6228-2272-4663-277478268000`
- Follows standard Improv WiFi specification

### Thread Safety
- Component uses NimBLE's thread-safe APIs
- Safe to use with WiFi component
- Compatible with LVGL UI operations

## Troubleshooting

### Device Not Visible in Improv App

1. Ensure NimBLE is enabled in ESP-IDF config:
   ```yaml
   esp32:
     framework:
       type: esp-idf
       sdkconfig_options:
         CONFIG_BT_NIMBLE_ENABLED: "y"
         CONFIG_BT_BLUEDROID_ENABLED: "n"
   ```

2. Check logs for advertising status:
   ```
   [I][nimble_improv:025]: NimBLE Improv service started
   ```

### Authorization Failed

- If using `authorizer`, ensure the button is pressed within the connection window
- Check `authorized_duration` - increase if needed
- Try without `authorizer` first (auto-authorizes)

### WiFi Connection Fails

- Verify SSID and password are correct
- Increase `wifi_timeout` if network is slow
- Check WiFi signal strength
- Ensure WiFi component is properly configured

## Contributing

Found a bug or have a feature request? Please open an issue on GitHub:
https://github.com/truffshuff/esphome-components/issues

## License

MIT License - See LICENSE file for details

## Credits

- Based on ESPHome's `esp32_improv` component
- Uses [Improv WiFi Protocol](https://www.improv-wifi.com/)
- Built with [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
