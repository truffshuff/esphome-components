# NimBLE Improv - Complete Implementation

This is a **fully functional** implementation of the Improv WiFi provisioning protocol using ESP-IDF's native NimBLE stack for ESPHome.

## What's Implemented

This implementation includes **all the missing pieces** from the upstream stub:

### ✅ GATT Service Registration
- Complete `ble_gatt_svc_def` structure with all 5 Improv characteristics
- Service UUID: `00467768-6228-2272-4663-277478268000`
- Characteristics:
  - **Status** (read + notify): Current provisioning state
  - **Error** (read): Last error code
  - **RPC Command** (write): Receives commands from client
  - **RPC Result** (read + notify): Sends responses to client
  - **Capabilities** (read): Reports device capabilities

### ✅ Characteristic Access Callbacks
- Full read/write handlers for all characteristics
- Proper UUID matching and operation handling
- Command parsing for WIFI_SETTINGS, IDENTIFY, GET_DEVICE_INFO
- Response formatting and transmission

### ✅ GAP Event Handling
- Connection/disconnection events
- Auto-authorization (if no authorizer configured)
- Automatic advertising restart after disconnect
- Connection handle management

### ✅ BLE Advertising
- Advertises Improv service UUID in scan response
- Configurable device name ("Halo Improv")
- General discoverable mode
- Automatic restart on advertising complete

### ✅ WiFi Provisioning Flow
1. Client connects via BLE
2. Device auto-authorizes (or waits for authorizer trigger)
3. Client sends WiFi credentials via RPC Command
4. Device attempts WiFi connection with timeout
5. On success: Sends IP address redirect URL via RPC Result
6. On failure: Sets error code and returns to authorized state

### ✅ Protocol Commands Implemented
- **WIFI_SETTINGS (0x01)**: Parse SSID/password, attempt connection
- **IDENTIFY (0x02)**: Trigger status indicator (if configured)
- **GET_DEVICE_INFO (0x03)**: Return firmware, version, hardware, device name
- **GET_WIFI_NETWORKS (0x04)**: Stub for future WiFi scanning

## Key Features

### Memory Efficient
- Uses ESP-IDF native NimBLE (not NimBLE-Arduino)
- Shares BLE stack with nimble_proxy component
- No Bluedroid overhead

### ESPHome Integration
- Uses ESPHome WiFi component for connection
- Integrates with optional binary output for status indicator
- Authorization timeout management
- Configurable WiFi connection timeout

### Improv Protocol Compliant
- Follows https://www.improv-wifi.com/ specification
- Compatible with standard Improv clients:
  - Improv WiFi web app
  - Mobile apps supporting Improv
  - Chrome browser with Web Bluetooth API

## Configuration

```yaml
# Ensure nimble_proxy is loaded first to initialize NimBLE stack
nimble_proxy:
  active: true
  max_connections: 3

# Full featured nimble_improv with local implementation
nimble_improv:
  authorized_duration: 2min  # How long authorization lasts
  wifi_timeout: 1min         # Timeout for WiFi connection attempts
  # Optional: status_indicator for IDENTIFY command
  # Optional: authorizer for manual authorization control
```

## Dependencies

**Required:**
- `nimble_proxy` component (must be loaded first)
- ESP32 with ESP-IDF framework
- NimBLE enabled in sdkconfig

**Compatible With:**
- ESPHome WiFi component
- bluetooth_proxy stub (for API message types)

## Implementation Details

### Service Registration Flow
1. Check if NimBLE host is initialized (via `ble_hs_is_enabled()`)
2. Call `ble_gatts_count_cfg()` to count service definitions
3. Call `ble_gatts_add_svcs()` to register services
4. Find characteristic handles via `ble_gatts_find_chr()`
5. Start BLE advertising with Improv UUID

### State Machine
- **STOPPED**: Service not running
- **AWAITING_AUTHORIZATION**: Connected, waiting for authorization
- **AUTHORIZED**: Ready to receive WiFi credentials
- **PROVISIONING**: Attempting WiFi connection
- **PROVISIONED**: WiFi connected successfully

### Error Handling
- **ERROR_NONE**: No error
- **ERROR_INVALID_RPC**: Malformed command data
- **ERROR_UNKNOWN_RPC**: Unrecognized command
- **ERROR_UNABLE_TO_CONNECT**: WiFi connection failed/timeout
- **ERROR_NOT_AUTHORIZED**: Command received before authorization
- **ERROR_UNKNOWN**: Unexpected error

## Testing

Use any Improv-compatible client:

1. **Web Browser** (Chrome/Edge):
   - Visit https://www.improv-wifi.com/
   - Click "Connect device via Improv"
   - Select "Halo Improv" from BLE device list
   - Enter WiFi credentials

2. **nRF Connect** (for debugging):
   - Scan for "Halo Improv"
   - Connect and explore GATT services
   - Find service UUID ending in `...268000`
   - Write to RPC Command characteristic

3. **ESPHome Logs**:
   - Watch for "NimBLE Improv" messages
   - Connection events logged with handle IDs
   - State transitions and command processing visible

## Differences from Upstream Stub

| Feature | Upstream | This Implementation |
|---------|----------|---------------------|
| GATT Service | TODO comment | ✅ Fully registered |
| Characteristics | Not registered | ✅ All 5 registered |
| Access Callbacks | Stub return 0 | ✅ Full read/write handling |
| GAP Events | Stub, no logic | ✅ Connection management |
| Advertising | Not started | ✅ Full advertising with UUID |
| Command Processing | Partially implemented | ✅ 3 commands working |
| Response Sending | TODO comment | ✅ Notifications sent |
| Device Info | TODO | ✅ Returns firmware/version/hardware |

## Future Enhancements

- [ ] WiFi network scanning (GET_WIFI_NETWORKS command)
- [ ] Save credentials to flash memory
- [ ] Auto-disable after successful provisioning (optional)
- [ ] Identify timer (turn off status after identify_duration)
- [ ] Support for authorization via binary sensor trigger

## Credits

Based on the stub implementation from https://github.com/truffshuff/esphome-components

Completed implementation following the pattern of nimble_proxy component.

Improv WiFi protocol: https://www.improv-wifi.com/

## License

Same as ESPHome (MIT License)
