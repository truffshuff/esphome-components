# NimBLE Base Component

The `nimble_base` component provides core NimBLE Bluetooth Low Energy (BLE) stack initialization for ESP32 devices. This is a foundational component used by other NimBLE-based components like `nimble_proxy` and `nimble_improv`.

## Purpose

This component was created to separate the core NimBLE initialization from specific BLE functionality, allowing users to:
- Use `nimble_improv` without requiring `nimble_proxy`
- Use `nimble_proxy` without improv functionality
- Use both components together
- Add custom BLE services more easily

## Features

- NimBLE stack initialization (NVS, Bluetooth controller, host task)
- GATT service registration mechanism for other components
- BLE advertising UUID registration
- Device name and MAC address utilities
- Shared across all NimBLE-based components

## Configuration

Add this to your ESPHome YAML configuration:

```yaml
nimble_base:
```

That's it! The component has no configuration options - it simply initializes the NimBLE stack.

## Usage Examples

### Improv WiFi Provisioning Only

```yaml
# Basic configuration
esphome:
  name: my-device

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# NimBLE base (required)
nimble_base:

# WiFi credentials (can be changed via BLE)
wifi:
  ssid: "MyNetwork"
  password: "MyPassword"

# Improv provisioning
nimble_improv:
  authorizer: none
  status_indicator: led
```

### BLE Proxy Only (Home Assistant)

```yaml
# Basic configuration
esphome:
  name: ble-proxy

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# NimBLE base (required)
nimble_base:

# WiFi
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

# API for Home Assistant
api:
  encryption:
    key: !secret api_key

# BLE Proxy
nimble_proxy:
  active: true
  max_connections: 3
```

### Both Improv and Proxy

```yaml
# Basic configuration
esphome:
  name: full-device

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# NimBLE base (required)
nimble_base:

# WiFi
wifi:
  ssid: "MyNetwork"
  password: "MyPassword"

# API for Home Assistant
api:
  encryption:
    key: !secret api_key

# Both components
nimble_proxy:
  active: true
  max_connections: 3

nimble_improv:
  authorizer: none
```

## Technical Details

### Setup Priority

The component runs at `setup_priority::AFTER_BLUETOOTH` (500.0), ensuring it initializes before other BLE components.

### Component Dependencies

- **Required by**: `nimble_proxy`, `nimble_improv`
- **Depends on**: `esp32`

### Service Registration

Other components can register GATT services during their construction:

```cpp
// In your component constructor
nimble_base::NimBLEBase::register_gatt_services(my_gatt_services);
nimble_base::NimBLEBase::register_advertising_service_uuid(&MY_SERVICE_UUID);
```

## Migration from Previous Versions

If you were using `nimble_improv` before, you now need to explicitly add `nimble_base`:

**Old configuration:**
```yaml
nimble_improv:
  # config
```

**New configuration:**
```yaml
nimble_base:

nimble_improv:
  # config
```

The `nimble_proxy` component also now requires `nimble_base` to be declared.

## See Also

- [nimble_proxy](../nimble_proxy/README.md) - BLE proxy for Home Assistant
- [nimble_improv](../nimble_improv/README.md) - WiFi provisioning via BLE
