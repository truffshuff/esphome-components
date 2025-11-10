import esphome.codegen as cg
import esphome.config_validation as cv

# Declare the C++ namespace for header-only helper functions
weather_helpers_ns = cg.global_ns.namespace("weather_helpers")

# Allow the component to be loaded without requiring configuration
MULTI_CONF = True
MULTI_CONF_NO_DEFAULT = True

# Empty config schema - no configuration needed
CONFIG_SCHEMA = cv.Schema({})

async def to_code(config):
    # Component loads automatically, making header available to all lambdas
    pass