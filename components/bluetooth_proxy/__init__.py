"""Home Assistant Bluetooth proxy over the NimBLE stack.

[nimble] VENDORED from ESPHome 2026.9.0 (esphome/components/bluetooth_proxy)
by truffshuff/esphome-components. The C++ (bluetooth_proxy.{h,cpp}) is
upstream's with three "[nimble]"-marked guard changes that route an ESP32
NimBLE build down upstream's own neutral-hub path (the one rp2/bk72xx/ln882x
use) instead of the esp32_ble_tracker-native one. This module is rewritten:
upstream's esp32 arm hard-wires esp32_ble_tracker + Bluedroid, which a NimBLE
build cannot load. See components/VENDORED.md.

YAML surface is upstream's esp32 surface: active, connection_slots,
cache_services and the internal connections list, plus ble_hub_id (defaults to
the single nimble_tracker).
"""

import esphome.codegen as cg
from esphome.components import (
    ble_device_base,
    bluetooth_connection,
    nimble_base,
    nimble_tracker,
)
import esphome.config_validation as cv
from esphome.const import CONF_ACTIVE, CONF_ID
from esphome.types import ConfigType

AUTO_LOAD = ["ble_device_base", "bluetooth_connection", "nimble_tracker"]
DEPENDENCIES = ["api", "esp32", "nimble_base"]
CODEOWNERS = ["@truffshuff"]

CONF_CONNECTION_SLOTS = "connection_slots"
CONF_CACHE_SERVICES = "cache_services"
CONF_CONNECTIONS = "connections"
DEFAULT_CONNECTION_SLOTS = 3

bluetooth_proxy_ns = cg.esphome_ns.namespace("bluetooth_proxy")
BluetoothProxy = bluetooth_proxy_ns.class_("BluetoothProxy", cg.Component)

_MAX_CONNECTIONS = bluetooth_connection.NIMBLE_MAX_CONNECTIONS
CONNECTION_SCHEMA = bluetooth_connection.hub_connection_schema()


def _populate_connections(config: ConfigType) -> ConfigType:
    """One wrapper + backend pair per slot (upstream's esp32 pattern)."""
    if CONF_CONNECTIONS in config:
        if not config[CONF_ACTIVE]:
            raise cv.Invalid("Connections can only be used if the proxy is set to active")
        nimble_base.consume_connection_slots(len(config[CONF_CONNECTIONS]), "bluetooth_proxy")(
            config
        )
        return config
    if not config[CONF_ACTIVE]:
        return config
    slots: int = config[CONF_CONNECTION_SLOTS]
    nimble_base.consume_connection_slots(slots, "bluetooth_proxy")(config)
    return {**config, CONF_CONNECTIONS: [CONNECTION_SCHEMA({}) for _ in range(slots)]}


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BluetoothProxy),
            cv.Optional(CONF_ACTIVE, default=True): cv.boolean,
            # Upstream maps this to Bluedroid's CONFIG_BT_GATTC_CACHE_NVS_FLASH, a
            # stack-side database cache. Home Assistant caches the database
            # itself (V3_WITH_CACHE connects skip discovery entirely) and NimBLE
            # keeps no stack-side cache, so the key is accepted for config
            # compatibility and has no effect.
            cv.Optional(CONF_CACHE_SERVICES, default=True): cv.boolean,
            cv.Optional(CONF_CONNECTION_SLOTS, default=DEFAULT_CONNECTION_SLOTS): cv.All(
                cv.positive_int, cv.Range(min=1, max=_MAX_CONNECTIONS)
            ),
            cv.Optional(CONF_CONNECTIONS): cv.All(
                cv.ensure_list(CONNECTION_SCHEMA),
                cv.Length(min=1, max=_MAX_CONNECTIONS),
            ),
        }
    )
    .extend(ble_device_base.BLE_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    _populate_connections,
)


async def _connections_to_code(var: cg.MockObj, config: ConfigType) -> None:
    """Identical to upstream: one wrapper + backend pair per slot."""
    connections = config.get(CONF_CONNECTIONS, [])
    cg.add_define("BLUETOOTH_PROXY_MAX_CONNECTIONS", len(connections))
    if connections:
        cg.add_define("USE_BLUETOOTH_PROXY_CONNECTIONS")
    for connection_conf in connections:
        backend = await bluetooth_connection.new_gatt_backend(connection_conf)
        connection = cg.new_Pvariable(connection_conf[CONF_ID])
        cg.add(connection.set_backend(backend))
        cg.add(var.register_connection(connection))


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_active(config[CONF_ACTIVE]))
    hub = await cg.get_variable(config[ble_device_base.CONF_BLE_HUB_ID])
    cg.add(var.set_ble_hub(hub))

    # nimble_tracker pushes scanner-state transitions (esp32_ble_tracker
    # parity): the proxy retries a refused state frame instead of polling.
    nimble_tracker.request_scanner_state_callback()

    await _connections_to_code(var, config)

    # Upstream value: 16 x ~80 B fills ~97% of a WiFi MTU per API frame.
    cg.add_define("BLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE", 16)
    cg.add_define("USE_BLUETOOTH_PROXY")
