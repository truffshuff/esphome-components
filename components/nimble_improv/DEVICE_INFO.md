# Device Information Service

The `nimble_improv` component automatically exposes a comprehensive BLE Device Information Service with details about your device.

## Characteristics Exposed

### 1. Manufacturer Name (0x2A29)
**Populated from your YAML config:**
```yaml
esphome:
  name: my-device
  project:
    name: "yourcompany.product-name"  # ← Used here
    version: "1.0.0"
```
- **Shows**: `"yourcompany.product-name"` if project name is defined
- **Fallback**: `"ESPHome"` if no project name

### 2. Model Number (0x2A24)
**Populated from friendly_name or board:**
```yaml
esphome:
  name: my-device
  friendly_name: "Living Room Sensor"  # ← Used here

esp32:
  board: esp32dev  # ← Fallback if no friendly_name
```
- **Shows**: `"Living Room Sensor"` (friendly_name preferred)
- **Fallback**: `"esp32dev"` (board type)
- **Last resort**: `"ESP32"`

### 3. Serial Number (0x2A25)
**Populated from device name:**
```yaml
esphome:
  name: living-room-sensor  # ← Used here (hostname)
```
- **Shows**: `"living-room-sensor"` (unique device identifier)

### 4. Hardware Revision (0x2A27)
**Automatically detected from ESP32 chip:**
- **Shows**: `"ESP32 Rev 3 (2 cores)"` (chip model, silicon revision, core count)
- Dynamically reads chip information at runtime

### 5. Firmware Revision (0x2A26)
**Automatically populated:**
- **Shows**: `"Jan 15 2025, 14:30:00"` (compilation timestamp)
- Useful for tracking when firmware was built

### 6. Software Revision (0x2A28)
**Populated from project version and ESPHome version:**
```yaml
esphome:
  name: my-device
  project:
    name: "yourcompany.product-name"
    version: "1.0.0"  # ← Used here
```
- **Shows**: `"1.0.0 (ESPHome 2024.12.0)"` if project version defined
- **Fallback**: `"ESPHome 2024.12.0"` if no project version

## Complete Example Configuration

```yaml
esphome:
  name: living-room-sensor
  friendly_name: "Living Room Sensor"
  project:
    name: "mycompany.smart-sensor"
    version: "2.1.0"

esp32:
  board: esp32dev
  framework:
    type: esp-idf

nimble_base:

nimble_improv:
  authorizer: none
```

### What BLE clients will see:

| Characteristic | Value |
|----------------|-------|
| Manufacturer | `"mycompany.smart-sensor"` |
| Model Number | `"Living Room Sensor"` |
| Serial Number | `"living-room-sensor"` |
| Hardware Revision | `"ESP32 Rev 3 (2 cores)"` |
| Firmware Revision | `"Jan 15 2025, 14:30:00"` |
| Software Revision | `"2.1.0 (ESPHome 2024.12.0)"` |

## Minimal Configuration

If you don't define project information, sensible defaults are used:

```yaml
esphome:
  name: my-device

esp32:
  board: esp32dev
  framework:
    type: esp-idf

nimble_base:

nimble_improv:
  authorizer: none
```

### What BLE clients will see:

| Characteristic | Value |
|----------------|-------|
| Manufacturer | `"ESPHome"` |
| Model Number | `"esp32dev"` |
| Serial Number | `"my-device"` |
| Hardware Revision | `"ESP32 Rev 3 (2 cores)"` |
| Firmware Revision | `"Jan 15 2025, 14:30:00"` |
| Software Revision | `"ESPHome 2024.12.0"` |

## Use Cases

### 1. Device Management
When scanning BLE devices, you can identify:
- Which product line it belongs to (Manufacturer)
- Human-readable name (Model Number)
- Unique device ID (Serial Number)

### 2. Support & Debugging
Support staff can quickly see:
- Hardware version (chip model and revision)
- When firmware was compiled
- Software version (your app version + ESPHome version)

### 3. Improv WiFi Provisioning
Improv apps can display device information during WiFi setup, helping users:
- Identify the correct device to provision
- Verify they're configuring the right hardware
- Check firmware/software versions

### 4. Inventory Management
For commercial products:
- Track product variants (Model Number)
- Maintain device registry (Serial Number)
- Verify hardware revisions in the field

## Viewing Device Information

You can view this information using:
- **nRF Connect** (iOS/Android) - Browse GATT services
- **LightBlue** (iOS/macOS) - Device Information Service tab
- **Improv WiFi apps** - Many display device info during provisioning
- **Home Assistant** - Shown in device details after BLE discovery

## Technical Details

All characteristics are:
- **Read-only** (cannot be written by clients)
- **Standard BLE UUIDs** (part of official Bluetooth spec)
- **Automatically populated** (no code changes needed)
- **Static values** (read once at connection time)

The Device Information Service UUID is `0x180A` (standard BLE service).
