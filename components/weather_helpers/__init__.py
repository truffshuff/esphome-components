import esphome.codegen as cg
import esphome.config_validation as cv

DEPENDENCIES = []

weather_helpers_ns = cg.esphome_ns.namespace("weather_helpers")

# Register a dummy component (optional if you want it to appear in logs)
CONFIG_SCHEMA = cv.Schema({})

async def to_code(config):
    pass