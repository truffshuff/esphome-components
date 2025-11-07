# NimBLE Proxy - Bluetooth LE Proxy for Home Assistant

A complete Bluetooth Low Energy proxy implementation using ESP-IDF's native NimBLE stack, providing seamless BLE device integration with Home Assistant.

## Overview

This component allows ESP32 devices to act as BLE proxies for Home Assistant, extending the range and reliability of BLE device connectivity. It scans for BLE advertisements and forwards them to Home Assistant via the ESPHome API.

## Features

### ✅ BLE Scanning
- Continuous BLE advertisement scanning
- Configurable scan parameters (320ms interval, 30ms window)
- Active scanning for scan response data
- RSSI tracking for all discovered devices
- No duplicate filtering (reports all advertisements)

### ✅ Advertisement Forwarding
- Batches advertisements for efficient transmission (5 per batch)
- 100ms timeout for partial batches
- Thread-safe API connection management
- Automatic buffering when Home Assistant disconnected
- Supports up to 62 bytes of advertisement data per device

### ✅ Home Assistant Integration
- Compatible with ESPHome API component
- Reports scanner state and mode to Home Assistant
- Feature flags: passive scan, raw advertisements, state/mode reporting
- Single API connection support (like native bluetooth_proxy)
- Dynamic scanner control (enable/disable from Home Assistant)

### ✅ GATT Service Support
- Service registration mechanism for other components
- Advertising UUID registration for custom services
- Works with `nimble_improv` for WiFi provisioning
- Concurrent scanning and advertising

### ✅ Resource Efficient
- Uses ESP-IDF native NimBLE (not NimBLE-Arduino or Bluedroid)
- Shares BLE stack with other NimBLE components
- Lower memory footprint than Bluedroid-based solutions
- Single BLE host task for all operations

## Configuration

### Basic Configuration

```yaml
esphome:
  name: ble-proxy

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# Required: NimBLE base component
nimble_base:

# WiFi connection
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

### With Improv WiFi Provisioning

```yaml
esphome:
  name: ble-proxy-improv

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# Required: NimBLE base component
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

# WiFi provisioning via BLE
nimble_improv:
  authorizer: none
```

### Configuration Options

```yaml
nimble_proxy:
  # Enable/disable BLE proxy (default: true)
  active: true

  # Maximum BLE connections to support (default: 3)
  # Note: Active connections not yet implemented, this reserves resources
  max_connections: 3
```

## How It Works

### Initialization
1. Registers sync callback with `nimble_base` during construction
2. Waits for NimBLE stack initialization by `nimble_base`
3. Once synced, starts BLE scanning and/or advertising
4. Connects to Home Assistant API when available

### Scanning Flow
1. **Start Scan**: Initiated automatically after NimBLE sync
2. **Advertisement Received**: Callback processes each advertisement
3. **Buffer**: Adds advertisement to batch (max 5 advertisements)
4. **Send**: Transmits batch when full or after 100ms timeout
5. **Home Assistant**: Processes advertisements for BLE device tracking

### Advertising (with Improv)
1. `nimble_improv` registers its service UUID during construction
2. `nimble_proxy` detects registered UUIDs after sync
3. Starts advertising with device name + MAC and Improv UUID
4. Handles GAP events (connect/disconnect)
5. Automatically resumes advertising after disconnection

### API Integration
1. Home Assistant connects via ESPHome API
2. Calls `subscribe_api_connection()` to register
3. Proxy sends scanner state and begins forwarding advertisements
4. Home Assistant can control scanner state (start/stop)
5. Connection unsubscribes when Home Assistant disconnects

## Home Assistant Integration

### Feature Flags

The component reports the following capabilities to Home Assistant:

- **PASSIVE_SCAN**: Supports passive BLE scanning
- **RAW_ADVERTISEMENTS**: Forwards raw advertisement data
- **STATE_AND_MODE**: Reports scanner state and mode

**Not yet implemented:**
- ACTIVE_CONNECTIONS: Active GATT connections
- REMOTE_CACHING: GATT service caching
- PAIRING: BLE pairing/bonding
- CACHE_CLEARING: Clear cached services

### Scanner States

- **STARTING**: Initializing BLE stack
- **RUNNING**: Actively scanning for devices
- **STOPPED**: Scan explicitly stopped
- **IDLE**: Not yet started (pre-sync)

### Scanner Modes

- **ACTIVE**: Active scanning (requests scan response)
- **PASSIVE**: Passive scanning only (not implemented)

## Performance Characteristics

### Scan Parameters
- **Interval**: 512 units (320ms)
- **Window**: 48 units (30ms)
- **Duty Cycle**: ~9.4% (30ms scan every 320ms)
- **Discovery**: General discovery (not limited)

### Advertisement Batching
- **Batch Size**: 5 advertisements
- **Timeout**: 100ms for partial batches
- **Max Data**: 62 bytes per advertisement
- **Overhead**: Minimal (batching reduces API calls)

### Resource Usage
- **Memory**: ~40KB for NimBLE stack (shared with other components)
- **CPU**: Minimal (event-driven callbacks)
- **Network**: Depends on BLE device density in area

## Compatibility

### ESPHome Components
- **bluetooth_proxy**: Acts as drop-in replacement (namespace aliasing)
- **api**: Required for Home Assistant communication
- **wifi**: Required for network connectivity
- **nimble_base**: Required for NimBLE initialization
- **nimble_improv**: Optional, for WiFi provisioning

### Home Assistant
- Compatible with Home Assistant's native BLE integration
- Appears as "ESPHome Bluetooth Proxy" in integrations
- Supports passive tracking of BLE devices
- Future: Active connections for control (not yet implemented)

### ESP32 Variants
- **ESP32**: Fully supported
- **ESP32-S3**: Fully supported
- **ESP32-C3**: Fully supported
- **ESP32-C6**: Should work (untested)
- **ESP32-H2**: Not supported (Bluetooth 5.0 only, no classic BT)

## Troubleshooting

### No devices appearing in Home Assistant

**Check:**
- ESPHome API is connected to Home Assistant
- `nimble_base` component is configured
- BLE scanning has started (check logs for "BLE scan started")
- Other BLE devices are in range

**Enable debug logging:**
```yaml
logger:
  level: DEBUG
  logs:
    nimble_proxy: VERBOSE
    nimble_base: DEBUG
```

### Advertisements not forwarding

**Symptoms:** Scan shows devices but Home Assistant doesn't see them

**Check:**
- API connection is subscribed (look for "API connection subscribed")
- No errors in advertisement buffering/sending
- Home Assistant BLE integration is enabled

### High memory usage

**Causes:**
- Too many devices in range causing buffer overflow
- Memory leak (check for continuously increasing usage)

**Solutions:**
- Reduce scan window/interval (requires code modification)
- Decrease `BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE`
- Filter by RSSI (not yet implemented)

### Connection conflicts

**Symptom:** `nimble_improv` or other BLE features not working

**Check:**
- Both components are configured correctly
- `nimble_base` is initialized before others
- No conflicts with other BLE libraries (e.g., NimBLE-Arduino)

## Technical Details

### Setup Priority

Runs at `setup_priority::AFTER_BLUETOOTH + 1.0f` (501.0), ensuring it initializes after `nimble_base` (500.0).

### Thread Safety

- Advertisement buffer access is mutex-protected
- API connection tracking uses mutex
- Safe concurrent access from NimBLE host task and ESPHome main thread

### Advertisement Data Format

Each advertisement contains:
- **Address**: 48-bit MAC address (6 bytes)
- **Address Type**: Public/random (1 byte)
- **RSSI**: Signal strength in dBm
- **Data**: Advertisement payload (up to 62 bytes)
- **Length**: Advertisement data length

### Namespace Aliasing

Creates `bluetooth_proxy::global_bluetooth_proxy` alias to `nimble_proxy::NimBLEProxy`, allowing compatibility with ESPHome API code that expects the native bluetooth_proxy component.

## Future Enhancements

- [ ] Active GATT connections (connect, read, write, subscribe)
- [ ] GATT service caching
- [ ] BLE pairing/bonding support
- [ ] RSSI filtering
- [ ] Whitelist/blacklist filtering
- [ ] Connection limit enforcement
- [ ] Passive scan mode
- [ ] Extended advertising support (BLE 5.0+)

## See Also

- [nimble_base](../nimble_base/README.md) - Required NimBLE stack initialization
- [nimble_improv](../nimble_improv/README.md) - Optional WiFi provisioning via BLE
- [ESPHome bluetooth_proxy](https://esphome.io/components/bluetooth_proxy.html) - Native Bluedroid-based implementation
- [Home Assistant BLE Integration](https://www.home-assistant.io/integrations/bluetooth/) - How Home Assistant uses BLE proxies
