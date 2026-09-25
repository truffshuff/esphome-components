"""Per-slot GATT connection wrapper plus the NimBLE GATT client backend.

[nimble] VENDORED from ESPHome 2026.9.0 (esphome/components/bluetooth_connection)
by truffshuff/esphome-components. The C++ hub wrapper
(bluetooth_connection_hub.{h,cpp}) and the shared helpers
(bluetooth_connection.{h,cpp}) are upstream's with the "[nimble]"-marked guards
only; the Bluedroid and rp2 backends are not carried. This module is rewritten:
upstream's is a per-platform backend registry (Bluedroid on esp32, BTstack on
rp2), and this copy has exactly one backend - NimBLEGattClient - on ESP32.

The public helpers keep upstream's names and signatures (gatt_client_schema,
hub_connection_schema, new_gatt_backend, HUB_MAX_CONNECTIONS) so the vendored
bluetooth_proxy codegen reads the same as upstream's. See
components/VENDORED.md for the patch list and the re-sync procedure.
"""

import esphome.codegen as cg
from esphome.components import nimble_tracker, ota
import esphome.config_validation as cv
from esphome.const import PLATFORM_ESP32
from esphome.core import CORE
from esphome.types import ConfigType

AUTO_LOAD = ["ble_device_base", "nimble_tracker"]
DEPENDENCIES = ["esp32"]
CODEOWNERS = ["@truffshuff"]

bluetooth_connection_ns = cg.esphome_ns.namespace("bluetooth_connection")

# NimBLE on ESP32 accepts up to 9 concurrent links (CONFIG_BT_NIMBLE_MAX_CONNECTIONS
# range 1..9 on non-C2/non-native-controller chips), the same cap as Bluedroid's
# CONFIG_BT_ACL_CONNECTIONS. nimble_base raises the Kconfig value to cover every
# slot consumer; this is the ceiling the proxy validates against.
NIMBLE_MAX_CONNECTIONS = 9

# Kept under upstream's name: the vendored bluetooth_proxy reads the esp32 cap here.
HUB_MAX_CONNECTIONS: dict[str, int] = {PLATFORM_ESP32: NIMBLE_MAX_CONNECTIONS}

HubBluetoothConnection = bluetooth_connection_ns.class_("BluetoothConnection")
NimBLEGattClient = bluetooth_connection_ns.class_(
    "NimBLEGattClient", cg.Component, cg.Parented.template(nimble_tracker.NimBLETracker)
)

CONF_BACKEND_ID = "backend_id"


def gatt_client_schema(platform: str | None = None) -> cv.Schema:
    """Schema fragment for one GATT backend instance: its generated id plus the
    tracker it coordinates the radio with (scan inhibit while connecting).

    `platform` is accepted for upstream signature compatibility; only ESP32 exists here.
    """
    return cv.Schema(
        {
            cv.GenerateID(nimble_tracker.CONF_NIMBLE_TRACKER_ID): cv.use_id(
                nimble_tracker.NimBLETracker
            ),
            cv.GenerateID(CONF_BACKEND_ID): cv.declare_id(NimBLEGattClient),
        }
    )


def hub_connection_schema(platform: str | None = None) -> cv.Schema:
    """Per-slot schema for the proxy's connection wrappers: the wrapper id on top
    of the backend fragment, plus the component keys (they apply to the backend,
    the slot's real Component) - same shape as upstream."""
    return (
        gatt_client_schema(platform)
        .extend({cv.GenerateID(): cv.declare_id(HubBluetoothConnection)})
        .extend(cv.COMPONENT_SCHEMA)
    )


async def new_gatt_backend(config: ConfigType) -> cg.MockObj:
    """Instantiate one NimBLE backend and bind it to the tracker."""
    from esphome.components import ble_device_base

    if not CORE.is_esp32:
        raise cv.Invalid("The NimBLE GATT backend only exists on ESP32")
    ble_device_base.request_gatt_client()
    backend = cg.new_Pvariable(config[CONF_BACKEND_ID])
    await cg.register_component(backend, config)
    await cg.register_parented(backend, config[nimble_tracker.CONF_NIMBLE_TRACKER_ID])
    # The backend drops its link when an OTA starts (esp32_ble_tracker parity:
    # the tracker disconnects every client on OTA_STARTED).
    ota.request_ota_state_listeners()
    return backend
