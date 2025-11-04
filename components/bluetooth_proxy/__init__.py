# Stub component that only provides the bluetooth_proxy.h header
# This is needed for ESPHome API when using NimBLE instead of Bluedroid
import esphome.codegen as cg
import esphome.config_validation as cv

# Empty component - just provides header files
CODEOWNERS = ["@truffshuff"]
AUTO_LOAD = []

CONFIG_SCHEMA = cv.Schema({})

async def to_code(config):
    # Define USE_BLUETOOTH_PROXY for the API component
    cg.add_define("USE_BLUETOOTH_PROXY")
    pass
