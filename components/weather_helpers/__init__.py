import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import core

# Create a C++ namespace (not under esphome::)
weather_helpers_ns = cg.global_ns.namespace("weather_helpers")

# Define a tiny dummy component to make ESPHome include the .cpp file
WeatherHelpersComponent = cg.Component.new("WeatherHelpersComponent")

CONFIG_SCHEMA = cv.Schema({}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable("weather_helpers_component", WeatherHelpersComponent)
    await cg.register_component(var, config)