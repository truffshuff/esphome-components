import esphome.codegen as cg
import esphome.config_validation as cv

DEPENDENCIES = []

# Register the namespace for your helper functions
weather_helpers_ns = cg.esphome_ns.namespace("weather_helpers")

# No configuration — this is purely a code helper
CONFIG_SCHEMA = cv.Schema({})

async def to_code(config):
    # Nothing to generate, just ensure the component is included
    pass