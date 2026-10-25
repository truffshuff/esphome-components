"""Improv WiFi provisioning over NimBLE - the NimBLE counterpart of esp32_improv.

Same YAML surface and behaviour as esp32_improv (ESPHome 2026.9.0):
authorizer (binary_sensor, optional), status_indicator, identify_duration,
authorized_duration, wifi_timeout, next_url, and the on_start /
on_provisioning / on_provisioned / on_state / on_stop triggers. The service
runs only while WiFi needs provisioning and stops once the device is
provisioned.

Differences from esp32_improv, all deliberate:
- authorizer may be omitted (esp32_improv requires the key, accepting `none`).
- A Device Information Service is exposed alongside the Improv service.
- Credentials the previous nimble_improv stored in its own preference slot are
  moved into the WiFi component once, on first boot of this version.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, improv_base, nimble_base, output
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_START, CONF_ON_STATE, CONF_TRIGGER_ID
from esphome.types import ConfigType

AUTO_LOAD = ["improv_base"]
CODEOWNERS = ["@truffshuff"]
DEPENDENCIES = ["esp32", "wifi", "nimble_base"]

CONF_AUTHORIZED_DURATION = "authorized_duration"
CONF_AUTHORIZER = "authorizer"
CONF_IDENTIFY_DURATION = "identify_duration"
CONF_ON_PROVISIONED = "on_provisioned"
CONF_ON_PROVISIONING = "on_provisioning"
CONF_ON_STOP = "on_stop"
CONF_STATUS_INDICATOR = "status_indicator"
CONF_WIFI_TIMEOUT = "wifi_timeout"

# esp32_improv default, aligned with the WiFi component's ap_timeout.
DEFAULT_WIFI_TIMEOUT = "90s"

improv_ns = cg.esphome_ns.namespace("improv")
Error = improv_ns.enum("Error")
State = improv_ns.enum("State")

nimble_improv_ns = cg.esphome_ns.namespace("nimble_improv")
NimBLEImprov = nimble_improv_ns.class_("NimBLEImprov", cg.Component)
ProvisionedTrigger = nimble_improv_ns.class_(
    "NimBLEImprovProvisionedTrigger", automation.Trigger.template()
)
ProvisioningTrigger = nimble_improv_ns.class_(
    "NimBLEImprovProvisioningTrigger", automation.Trigger.template()
)
StartTrigger = nimble_improv_ns.class_(
    "NimBLEImprovStartTrigger", automation.Trigger.template()
)
StateTrigger = nimble_improv_ns.class_(
    "NimBLEImprovStateTrigger", automation.Trigger.template()
)
StoppedTrigger = nimble_improv_ns.class_(
    "NimBLEImprovStoppedTrigger", automation.Trigger.template()
)

_TRIGGERS = {
    CONF_ON_PROVISIONED: ProvisionedTrigger,
    CONF_ON_PROVISIONING: ProvisioningTrigger,
    CONF_ON_START: StartTrigger,
    CONF_ON_STATE: StateTrigger,
    CONF_ON_STOP: StoppedTrigger,
}


def _validate_authorizer(value):
    # esp32_improv's authorizer is a binary_sensor; the previous nimble_improv
    # took an output, which could never authorize anything.
    return cv.Any(cv.none, cv.use_id(binary_sensor.BinarySensor))(value)


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NimBLEImprov),
            cv.Optional(CONF_AUTHORIZER): _validate_authorizer,
            cv.Optional(CONF_STATUS_INDICATOR): cv.use_id(output.BinaryOutput),
            cv.Optional(
                CONF_IDENTIFY_DURATION, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_AUTHORIZED_DURATION, default="1min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_WIFI_TIMEOUT, default=DEFAULT_WIFI_TIMEOUT
            ): cv.positive_time_period_milliseconds,
            **{
                cv.Optional(key): automation.validate_automation(
                    {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(trigger)}
                )
                for key, trigger in _TRIGGERS.items()
            },
        }
    )
    .extend(improv_base.IMPROV_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

# The peripheral link a provisioning client opens.
CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA, nimble_base.consume_connection_slots(1, "nimble_improv")
)


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Since ESPHome 2026.10.0 (esphome#19264) improv_base emits one
    # USE_IMPROV_NEXT_URL for every transport configured with next_url:, so
    # this no longer borrows esp32_improv's define. Older ESPHome took the
    # component name as a third argument; this call needs 2026.10.0+.
    await improv_base.setup_improv_core(var, config)

    cg.add(var.set_identify_duration(config[CONF_IDENTIFY_DURATION]))
    cg.add(var.set_authorized_duration(config[CONF_AUTHORIZED_DURATION]))
    cg.add(var.set_wifi_timeout(config[CONF_WIFI_TIMEOUT]))

    if config.get(CONF_AUTHORIZER) is not None:
        authorizer = await cg.get_variable(config[CONF_AUTHORIZER])
        cg.add(var.set_authorizer(authorizer))
    if CONF_STATUS_INDICATOR in config:
        status_indicator = await cg.get_variable(config[CONF_STATUS_INDICATOR])
        cg.add(var.set_status_indicator(status_indicator))

    use_state_callback = False
    for key in _TRIGGERS:
        for conf in config.get(key, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
            args = [(State, "state"), (Error, "error")] if key == CONF_ON_STATE else []
            await automation.build_automation(trigger, args, conf)
            use_state_callback = True
    if use_state_callback:
        cg.add_define("USE_NIMBLE_IMPROV_STATE_CALLBACK")
