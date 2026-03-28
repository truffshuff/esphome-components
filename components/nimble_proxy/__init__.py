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
CONF_SCAN_ACTIVE = "scan_active"
CONF_SCAN_INTERVAL = "scan_interval"
CONF_SCAN_WINDOW = "scan_window"
CONF_SCAN_DUPLICATE_FILTER = "scan_duplicate_filter"
CONF_ADVERTISING_NAME = "advertising_name"

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

        # Scanner tuning for noisy RF environments.
        # Values are in BLE units of 0.625ms (same as NimBLE/IDF APIs).
        cv.Optional(CONF_SCAN_ACTIVE, default=False): cv.boolean,
        cv.Optional(CONF_SCAN_INTERVAL, default=2048): cv.All(
            cv.positive_int,
            cv.Range(min=16, max=65535)
        ),
        cv.Optional(CONF_SCAN_WINDOW, default=256): cv.All(
            cv.positive_int,
            cv.Range(min=16, max=65535)
        ),
        cv.Optional(CONF_SCAN_DUPLICATE_FILTER, default=True): cv.boolean,
        cv.Optional(CONF_ADVERTISING_NAME, default=""): cv.string_strict,
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

    if config[CONF_SCAN_WINDOW] > config[CONF_SCAN_INTERVAL]:
        raise cv.Invalid(
            f"'{CONF_SCAN_WINDOW}' must be <= '{CONF_SCAN_INTERVAL}'"
        )

    return config


async def to_code(config):
    # Validate and migrate configuration
    config = _validate_config(config)

    # Provide sane defaults for API compile-time constants if not set
    # Reduced from 5 to 2 to prevent protocol buffer encoding assertion failures
    # Large batches can exceed TCP send buffer capacity causing proto.h:820 crashes
    cg.add_build_flag("-DBLUETOOTH_PROXY_ADVERTISEMENT_BATCH_SIZE=2")
    cg.add_build_flag("-DBLUETOOTH_PROXY_MAX_CONNECTIONS=%d" % config[CONF_CONNECTION_SLOTS])

    var = cg.new_Pvariable(config[CONF_ID])
    # Register as runtime Component so setup()/loop() run for advertisement batching,
    # diagnostics counters, and scanner state updates.
    await cg.register_component(var, config)

    cg.add(var.set_active(config[CONF_ACTIVE]))
    cg.add(var.set_connection_slots(config[CONF_CONNECTION_SLOTS]))
    cg.add(var.set_scan_active(config[CONF_SCAN_ACTIVE]))
    cg.add(var.set_scan_interval(config[CONF_SCAN_INTERVAL]))
    cg.add(var.set_scan_window(config[CONF_SCAN_WINDOW]))
    cg.add(var.set_scan_duplicate_filter(config[CONF_SCAN_DUPLICATE_FILTER]))
    cg.add(var.set_advertising_name(config[CONF_ADVERTISING_NAME]))

    # Note: cache_services parameter is accepted for API compatibility with Bluedroid bluetooth_proxy
    # but is not yet functional. Active GATT connections required before service caching can be implemented.

    # No library dependency needed - using ESP-IDF native NimBLE
