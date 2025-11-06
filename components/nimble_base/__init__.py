"""
NimBLE Base Component
Provides core NimBLE stack initialization and service registration for ESP32
Used by nimble_proxy and nimble_improv components
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@truffshuff"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = []

nimble_base_ns = cg.esphome_ns.namespace("nimble_base")
NimBLEBase = nimble_base_ns.class_("NimBLEBase", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NimBLEBase),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # No library dependency needed - using ESP-IDF native NimBLE
