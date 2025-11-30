import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@truffshuff"]
DEPENDENCIES = ["api", "nimble_base"]
AUTO_LOAD = []

nimble_proxy_ns = cg.esphome_ns.namespace("nimble_proxy")
NimBLEProxy = nimble_proxy_ns.class_("NimBLEProxy", cg.Component)

# Configuration parameter names - matching Bluedroid bluetooth_proxy for API compatibility
CONF_ACTIVE = "active"
CONF_CONNECTION_SLOTS = "connection_slots"  # Renamed from max_connections to match Bluedroid
CONF_CACHE_SERVICES = "cache_services"      # Placeholder for future GATT caching support

# Legacy parameter support for backward compatibility
CONF_MAX_CONNECTIONS = "max_connections"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NimBLEProxy),
        cv.Optional(CONF_ACTIVE, default=True): cv.boolean,

        # Primary parameter (matches Bluedroid bluetooth_proxy)
        cv.Optional(CONF_CONNECTION_SLOTS): cv.All(
            cv.positive_int,
            cv.Range(min=1, max=9)  # Match Bluedroid's max of 9
        ),

        # Legacy parameter (deprecated, for backward compatibility)
        cv.Optional(CONF_MAX_CONNECTIONS): cv.All(
            cv.positive_int,
            cv.Range(min=1, max=9)
        ),

        # Placeholder for future GATT service caching (accepted but not yet implemented)
        cv.Optional(CONF_CACHE_SERVICES, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


def _validate_config(config):
    """Validate configuration and handle legacy parameter migration."""
    # Handle connection_slots vs max_connections
    if CONF_CONNECTION_SLOTS in config and CONF_MAX_CONNECTIONS in config:
        raise cv.Invalid(
            f"Cannot specify both '{CONF_CONNECTION_SLOTS}' and '{CONF_MAX_CONNECTIONS}'. "
            f"Use '{CONF_CONNECTION_SLOTS}' ('{CONF_MAX_CONNECTIONS}' is deprecated)."
        )

    # Migrate legacy max_connections to connection_slots
    if CONF_MAX_CONNECTIONS in config:
        CORE.data.setdefault("nimble_proxy", {})
        CORE.data["nimble_proxy"]["used_legacy_param"] = True
        config[CONF_CONNECTION_SLOTS] = config.pop(CONF_MAX_CONNECTIONS)

    # Set default if neither specified
    if CONF_CONNECTION_SLOTS not in config:
        config[CONF_CONNECTION_SLOTS] = 3

    return config


async def to_code(config):
    # Validate and migrate configuration
    config = _validate_config(config)

    # Provide sane defaults for API compile-time constants if not set
    cg.add_build_flag("-DBLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE=5")
    cg.add_build_flag("-DBLUETOOTH_PROXY_MAX_CONNECTIONS=%d" % config[CONF_CONNECTION_SLOTS])

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_active(config[CONF_ACTIVE]))
    cg.add(var.set_connection_slots(config[CONF_CONNECTION_SLOTS]))

    # Log warning if cache_services is enabled (not yet implemented)
    if config[CONF_CACHE_SERVICES]:
        cg.add_library(None, None)  # Dummy to allow logging
        # Note: We accept the parameter for API compatibility but don't use it yet
        # Active GATT connections required before service caching can be implemented

    # Log deprecation warning if legacy parameter was used
    if CORE.data.get("nimble_proxy", {}).get("used_legacy_param", False):
        cg.add_library(None, None)  # Dummy to allow logging
        # Warning will be shown during compilation

    # No library dependency needed - using ESP-IDF native NimBLE
