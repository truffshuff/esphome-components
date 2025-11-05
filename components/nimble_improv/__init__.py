"""
NimBLE-based Improv WiFi Provisioning Component
Allows WiFi reconfiguration via BLE using the Improv protocol
Uses ESP-IDF native NimBLE stack (not NimBLE-Arduino) for memory efficiency
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, output
from esphome.const import (
    CONF_ID,
)
from esphome.core import CORE, coroutine_with_priority

DEPENDENCIES = ["esp32", "wifi"]
AUTO_LOAD = ["output"]
CODEOWNERS = ["@truffshuff"]

# Define our own constants since they don't exist in esphome.const
CONF_AUTHORIZER = "authorizer"
CONF_AUTHORIZED_DURATION = "authorized_duration"
CONF_IDENTIFY_DURATION = "identify_duration"
CONF_STATUS_INDICATOR = "status_indicator"
CONF_WIFI_TIMEOUT = "wifi_timeout"

nimble_improv_ns = cg.esphome_ns.namespace("nimble_improv")
NimBLEImprov = nimble_improv_ns.class_("NimBLEImprov", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NimBLEImprov),
        cv.Optional(CONF_AUTHORIZER): cv.use_id(output.BinaryOutput),
        cv.Optional(
            CONF_AUTHORIZED_DURATION, default="1min"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_STATUS_INDICATOR): cv.use_id(output.BinaryOutput),
        cv.Optional(
            CONF_IDENTIFY_DURATION, default="10s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_WIFI_TIMEOUT, default="1min"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


@coroutine_with_priority(52.0)
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_define("USE_NIMBLE_IMPROV")

    if CONF_AUTHORIZER in config:
        activator = await cg.get_variable(config[CONF_AUTHORIZER])
        cg.add(var.set_authorizer(activator))
        cg.add(
            var.set_authorized_duration(config[CONF_AUTHORIZED_DURATION])
        )

    if CONF_STATUS_INDICATOR in config:
        status_indicator = await cg.get_variable(config[CONF_STATUS_INDICATOR])
        cg.add(var.set_status_indicator(status_indicator))

    cg.add(var.set_identify_duration(config[CONF_IDENTIFY_DURATION]))
    cg.add(var.set_wifi_timeout(config[CONF_WIFI_TIMEOUT]))

    # No library dependency needed - using ESP-IDF native NimBLE
    # Ensure NimBLE is enabled and Bluedroid is explicitly disabled
    if CORE.using_esp_idf:
        cg.add_build_flag("-DCONFIG_BT_NIMBLE_ENABLED=1")
        cg.add_build_flag("-DCONFIG_BT_BLUEDROID_ENABLED=0")
        cg.add_build_flag("-DCONFIG_BLUEDROID_ENABLED=0")
        cg.add_build_flag("-DCONFIG_BT_CLASSIC_ENABLED=0")
        cg.add_build_flag("-DCONFIG_BT_BLE_ENABLED=0")
