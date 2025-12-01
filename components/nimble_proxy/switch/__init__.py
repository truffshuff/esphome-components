import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
    ICON_BLUETOOTH,
)
from .. import nimble_proxy_ns, NimBLEProxy

DEPENDENCIES = ["nimble_proxy"]
CONF_NIMBLE_PROXY_ID = "nimble_proxy_id"

NimBLEProxyScannerSwitch = nimble_proxy_ns.class_(
    "NimBLEProxyScannerSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = switch.switch_schema(
    NimBLEProxyScannerSwitch,
    entity_category=ENTITY_CATEGORY_CONFIG,
    icon=ICON_BLUETOOTH,
).extend(
    {
        cv.GenerateID(CONF_NIMBLE_PROXY_ID): cv.use_id(NimBLEProxy),
    }
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_NIMBLE_PROXY_ID])
    cg.add(var.set_parent(parent))
