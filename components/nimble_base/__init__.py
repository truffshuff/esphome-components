"""NimBLE stack owner for ESP32 - the NimBLE counterpart of esp32_ble.

Brings the controller and NimBLE host up, applies the security-manager
configuration, owns GATT-server service registration and delivers the host's
sync/reset events on the main loop. nimble_tracker, the vendored
bluetooth_proxy / bluetooth_connection and nimble_improv all build on it.

Options mirror esp32_ble where NimBLE has an equivalent: name, io_capability,
auth_req_mode, enable_on_boot, connection_timeout, use_psram, and the
nimble_base.enable / nimble_base.disable actions (ble.enable / ble.disable).
max_key_size / min_key_size have no runtime NimBLE equivalent and are not
offered.
"""

from esphome import automation
from esphome.automation import maybe_simple_id
import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_ENABLE_ON_BOOT, CONF_ID, CONF_NAME
from esphome.core import CORE, CoroPriority, TimePeriod, coroutine_with_priority
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = ["@truffshuff"]
DEPENDENCIES = ["esp32"]

CONF_IO_CAPABILITY = "io_capability"
CONF_AUTH_REQ_MODE = "auth_req_mode"
CONF_CONNECTION_TIMEOUT = "connection_timeout"
CONF_USE_PSRAM = "use_psram"

KEY_NIMBLE_BASE = "nimble_base"
KEY_USED_CONNECTION_SLOTS = "used_connection_slots"

# NimBLE (like Bluedroid's CONFIG_BT_ACL_CONNECTIONS) supports 1..9 links on
# chips with the ESP32/S3/C3-family controller.
MAX_CONNECTIONS = 9
MAX_CONNECTIONS_KCONFIG = "CONFIG_BT_NIMBLE_MAX_CONNECTIONS"

nimble_base_ns = cg.esphome_ns.namespace("nimble_base")
NimBLEBase = nimble_base_ns.class_("NimBLEBase", cg.Component)
EnableAction = nimble_base_ns.class_("EnableAction", automation.Action)
DisableAction = nimble_base_ns.class_("DisableAction", automation.Action)
IsEnabledCondition = nimble_base_ns.class_("IsEnabledCondition", automation.Condition)

# Bluetooth-spec IO capability values (host/ble_sm.h BLE_SM_IO_CAP_*), keyed by
# the esp32_ble option names.
IO_CAPABILITY = {
    "display_only": 0,
    "display_yes_no": 1,
    "keyboard_only": 2,
    "none": 3,
    "keyboard_display": 4,
}

# esp32_ble auth_req_mode names -> (bonding, mitm, secure_connections).
AUTH_REQ_MODE = {
    "no_bond": (False, False, False),
    "bond": (True, False, False),
    "mitm": (False, True, False),
    "bond_mitm": (True, True, False),
    "sc_only": (False, False, True),
    "sc_bond": (True, False, True),
    "sc_mitm": (False, True, True),
    "sc_mitm_bond": (True, True, True),
}
# Unset: bond with LE Secure Connections, no MITM - there is no trusted
# confirmation channel on a headless proxy, so it must not claim MITM.
DEFAULT_AUTH_REQ = (True, False, True)


def consume_connection_slots(value: int, consumer: str):
    """Reserve NimBLE connection slots for a component (esp32_ble API parity).

    The total sizes CONFIG_BT_NIMBLE_MAX_CONNECTIONS at build time and is checked
    against any value the user set explicitly.
    """

    def _consume(config: ConfigType) -> ConfigType:
        data = CORE.data.setdefault(KEY_NIMBLE_BASE, {})
        slots: list[str] = data.setdefault(KEY_USED_CONNECTION_SLOTS, [])
        slots.extend([consumer] * value)
        return config

    return _consume


def _used_slots() -> list[str]:
    return CORE.data.get(KEY_NIMBLE_BASE, {}).get(KEY_USED_CONNECTION_SLOTS, [])


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NimBLEBase),
        cv.Optional(CONF_NAME): cv.All(cv.string, cv.Length(max=29)),
        cv.Optional(CONF_IO_CAPABILITY, default="none"): cv.enum(IO_CAPABILITY, lower=True),
        cv.Optional(CONF_AUTH_REQ_MODE): cv.one_of(*AUTH_REQ_MODE, lower=True),
        cv.Optional(CONF_ENABLE_ON_BOOT, default=True): cv.boolean,
        cv.Optional(CONF_CONNECTION_TIMEOUT, default="20s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=TimePeriod(seconds=10), max=TimePeriod(seconds=180)),
        ),
        # No default (esp32_ble parity): requires_component would otherwise
        # reject every config without psram:, even with the option unset.
        cv.Optional(CONF_USE_PSRAM): cv.All(cv.requires_component("psram"), cv.boolean),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config: ConfigType) -> ConfigType:
    used = _used_slots()
    if len(used) > MAX_CONNECTIONS:
        raise cv.Invalid(
            f"NimBLE supports at most {MAX_CONNECTIONS} connections, but "
            f"{len(used)} are reserved ({', '.join(sorted(set(used)))})"
        )
    esp32_conf = fv.full_config.get().get("esp32", {})
    options = esp32_conf.get("framework", {}).get("sdkconfig_options", {})
    if MAX_CONNECTIONS_KCONFIG in options:
        try:
            explicit = int(str(options[MAX_CONNECTIONS_KCONFIG]).strip('"'))
        except ValueError:
            explicit = None
        if explicit is not None and explicit < len(used):
            raise cv.Invalid(
                f"sdkconfig_options sets {MAX_CONNECTIONS_KCONFIG}={explicit}, but "
                f"{len(used)} connection slots are reserved "
                f"({', '.join(used)}). Remove the option (nimble_base sizes it) "
                "or raise it."
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


@coroutine_with_priority(CoroPriority.FINAL)
async def _reconcile_nimble_sdkconfig(use_psram: bool) -> None:
    """Defaults only: an option the user set in sdkconfig_options always wins."""
    default = esp32.set_idf_sdkconfig_default
    default("CONFIG_BT_ENABLED", True)
    default("CONFIG_BT_NIMBLE_ENABLED", True)
    default("CONFIG_BT_BLUEDROID_ENABLED", False)
    default(MAX_CONNECTIONS_KCONFIG, max(1, len(_used_slots())))
    # Bluedroid offers its stack default (GATT_MAX_MTU_SIZE, 517) in the MTU
    # exchange; NimBLE's Kconfig default is 256.
    default("CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU", 517)
    if use_psram:
        default("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL", True)


async def to_code(config: ConfigType) -> None:
    # ESPHome 2026.9.0 (esphome#18599) excludes the IDF `bt` component unless
    # asked for; every NimBLE header lives there.
    esp32.request_bluetooth()
    # One 2.4 GHz radio: without software coexistence WiFi reception collapses
    # under BLE scanning. ESPHome 2026.7.0 (esphome#17008) only enables it when
    # a component asks (esp32_ble_tracker does; this is the NimBLE equivalent).
    if "wifi" in CORE.loaded_integrations:
        esp32.request_software_coexistence()
        cg.add_define("USE_NIMBLE_SOFTWARE_COEXISTENCE")

    # Selects the NimBLE arms in the vendored ble_device_base /
    # bluetooth_connection / bluetooth_proxy.
    cg.add_define("USE_NIMBLE_BLE")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_NAME in config:
        cg.add(var.set_device_name(config[CONF_NAME]))
    cg.add(var.set_io_capability(config[CONF_IO_CAPABILITY]))
    bonding, mitm, sc = (
        AUTH_REQ_MODE[config[CONF_AUTH_REQ_MODE]]
        if CONF_AUTH_REQ_MODE in config
        else DEFAULT_AUTH_REQ
    )
    cg.add(var.set_bonding(bonding))
    cg.add(var.set_mitm(mitm))
    cg.add(var.set_secure_connections(sc))
    cg.add(var.set_enable_on_boot(config[CONF_ENABLE_ON_BOOT]))
    cg.add(var.set_connection_timeout(config[CONF_CONNECTION_TIMEOUT].total_milliseconds))

    CORE.add_job(_reconcile_nimble_sdkconfig, config.get(CONF_USE_PSRAM, False))


NIMBLE_BASE_ACTION_SCHEMA = maybe_simple_id({cv.GenerateID(): cv.use_id(NimBLEBase)})


@automation.register_action(
    "nimble_base.enable", EnableAction, NIMBLE_BASE_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "nimble_base.disable", DisableAction, NIMBLE_BASE_ACTION_SCHEMA, synchronous=True
)
async def nimble_base_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition(
    "nimble_base.is_enabled", IsEnabledCondition, NIMBLE_BASE_ACTION_SCHEMA
)
async def nimble_base_is_enabled_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
