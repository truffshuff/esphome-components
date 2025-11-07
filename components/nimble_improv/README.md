# NimBLE Improv - WiFi Provisioning via BLE

A complete implementation of the [Improv WiFi](https://www.improv-wifi.com/) provisioning protocol using ESP-IDF's native NimBLE stack for ESPHome.

## Overview

This component allows users to provision WiFi credentials to ESP32 devices via Bluetooth Low Energy, making initial setup easier without needing to hardcode credentials or use a captive portal.

## Features

### ✅ Complete Improv Protocol Implementation
- All 5 Improv GATT characteristics implemented
- Service UUID: `00467768-6228-2272-4663-277478268000`
- Characteristics:
  - **Status** (0x...01, read + notify): Current provisioning state
  - **Error** (0x...02, read + notify): Last error code
  - **RPC Command** (0x...03, write): Receives commands from client
  - **RPC Result** (0x...04, read + notify): Sends responses to client
  - **Capabilities** (0x...05, read): Reports device capabilities (Identify support)

### ✅ Device Information Service
- Standard BLE Device Information Service (0x180A)
- Provides manufacturer, model, serial number, firmware version, hardware revision

### ✅ Command Support
- **WIFI_SETTINGS (0x01)**: Accept and save WiFi credentials, attempt connection
- **IDENTIFY (0x02)**: Trigger status indicator (if configured)
- **GET_DEVICE_INFO (0x03)**: Return device name, firmware version, hardware info
- **GET_WIFI_NETWORKS (0x04)**: Returns empty list (allows manual entry)

### ✅ WiFi Provisioning Flow
1. Client connects via BLE
2. Device auto-authorizes (or waits for authorizer trigger)
3. Client sends WiFi credentials via RPC Command
4. Device attempts WiFi connection with timeout
5. On success: Saves credentials to NVS and sends IP address redirect URL
6. On failure: Sets error code and returns to authorized state

### ✅ Credential Persistence
- Saves WiFi credentials to NVS (non-volatile storage)
- Credentials survive reboots
- Loaded automatically before WiFi component initializes
- Overrides YAML-configured credentials

### ✅ BLE Integration
- Uses `nimble_base` for NimBLE stack initialization
- Works with `nimble_proxy` for Home Assistant BLE proxy functionality
- Shares BLE stack with other NimBLE components
- Advertising handled by `nimble_proxy` when both components are used

### ✅ ESPHome Integration
- Uses ESPHome WiFi component for connection management
- Optional binary output for status indicator
- Configurable authorization timeout
- Configurable WiFi connection timeout
- Compatible with ESPHome's native WiFi features

## Configuration

### Minimal Configuration (Improv Only)

```yaml
esphome:
  name: my-device

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# Required: NimBLE base component
nimble_base:

# WiFi (initial credentials, can be changed via Improv)
wifi:
  ssid: "InitialNetwork"
  password: "InitialPassword"

# Improv WiFi provisioning
nimble_improv:
  authorizer: none  # Auto-authorize connections
```

### With BLE Proxy (Improv + Home Assistant)

```yaml
esphome:
  name: my-device

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

# BLE Proxy for Home Assistant
nimble_proxy:
  active: true
  max_connections: 3

# Improv WiFi provisioning
nimble_improv:
  authorized_duration: 2min
  wifi_timeout: 1min
  # Optional: status_indicator for visual feedback
```

### Full Configuration Options

```yaml
nimble_improv:
  # How long authorization lasts (default: 60s)
  authorized_duration: 2min

  # Timeout for WiFi connection attempts (default: 60s)
  wifi_timeout: 1min

  # Optional: LED or output to indicate status
  status_indicator: status_led

  # Optional: Button or sensor to trigger authorization
  # authorizer: provision_button
```

## Dependencies

**Required:**
- `nimble_base` component (provides NimBLE stack initialization)
- ESP32 with ESP-IDF framework
- ESPHome `wifi` component

**Optional:**
- `nimble_proxy` - For Home Assistant BLE proxy functionality
- `output` - For status indicator LED
- `binary_sensor` - For authorization button/trigger

## How It Works

### Service Registration
1. Constructor registers GATT services with `nimble_base` during component creation
2. `nimble_base` initializes NimBLE stack and registers all services during its setup
3. `nimble_improv` retrieves characteristic handles once NimBLE host syncs
4. Improv service UUID is registered for advertising (handled by `nimble_base` or `nimble_proxy`)

### Connection Flow
1. **BLE Advertisement**: Device advertises with Improv service UUID
2. **Client Connection**: Client app (e.g., Improv web app) connects via BLE
3. **Authorization**: Auto-authorized if no authorizer configured, otherwise waits for trigger
4. **Credential Transfer**: Client writes SSID/password to RPC Command characteristic
5. **WiFi Connection**: Device attempts to connect with provided credentials
6. **Result**: Success sends redirect URL via notification; failure sets error state
7. **Persistence**: Successful credentials saved to NVS for future boots

### State Machine
- **STOPPED**: Service not running (pre-initialization)
- **AWAITING_AUTHORIZATION**: BLE connected, waiting for authorization
- **AUTHORIZED**: Authorized and ready to receive WiFi credentials
- **PROVISIONING**: Attempting WiFi connection with provided credentials
- **PROVISIONED**: WiFi connected successfully, credentials saved

### Error Codes
- **ERROR_NONE** (0x00): No error
- **ERROR_INVALID_RPC** (0x01): Malformed command data
- **ERROR_UNKNOWN_RPC** (0x02): Unrecognized command
- **ERROR_UNABLE_TO_CONNECT** (0x03): WiFi connection failed or timeout
- **ERROR_NOT_AUTHORIZED** (0x04): Command received before authorization
- **ERROR_UNKNOWN** (0xFF): Unexpected error

## Usage / Testing

### Using the Improv Web App

1. **Open the Improv WiFi web app**:
   - Visit https://www.improv-wifi.com/ in Chrome/Edge browser
   - Click "Connect device via Improv"

2. **Connect to your device**:
   - Your device will appear with its configured name + MAC address
   - Example: `my-device-ABC123`
   - Click to connect

3. **Provision WiFi**:
   - Enter your WiFi network SSID and password
   - Click "Provision"
   - Wait for connection confirmation
   - Device will provide redirect URL to its web interface

### Using nRF Connect (Debugging)

1. **Scan for device**:
   - Open nRF Connect mobile app
   - Scan for devices advertising Improv service
   - Connect to your device

2. **Explore services**:
   - Find Improv service (UUID: `00467768-...`)
   - Enable notifications on Status, Error, and RPC Result
   - View characteristic values

3. **Send commands** (advanced):
   - Write to RPC Command characteristic
   - Format: `[command][length][data...]`
   - Monitor notifications for responses

### Monitoring Logs

Enable verbose logging to see detailed Improv activity:

```yaml
logger:
  level: DEBUG
  logs:
    nimble_improv: VERBOSE
    nimble_base: DEBUG
```

Watch for:
- `"BLE connection detected"` - Client connected
- `"Auto-authorized"` - Authorization granted
- `"Processing RPC command"` - Command received
- `"WiFi connected successfully"` - Provisioning complete
- `"Saved WiFi credentials to NVS"` - Credentials persisted

## Troubleshooting

### Device not appearing in scan
- Ensure `nimble_base` is configured
- Check that BLE advertising is working (use nRF Connect to scan)
- Verify ESP-IDF framework is being used (not Arduino)

### Connection fails immediately
- Check authorization settings
- Review logs for error messages
- Ensure device isn't already connected to another client

### WiFi provisioning fails
- Verify SSID and password are correct
- Check `wifi_timeout` setting (may need to increase)
- Monitor logs for `ERROR_UNABLE_TO_CONNECT`

### Credentials not persisting
- Ensure NVS is initialized (nimble_base handles this)
- Check for NVS flash errors in logs
- Verify sufficient flash space

## See Also

- [nimble_base](../nimble_base/README.md) - Required NimBLE stack initialization
- [nimble_proxy](../nimble_proxy/README.md) - Optional BLE proxy for Home Assistant
- [Improv WiFi Protocol](https://www.improv-wifi.com/) - Official specification
- [Device Information Service](DEVICE_INFO.md) - BLE DIS implementation details
