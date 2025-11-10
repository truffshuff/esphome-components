import esphome.codegen as cg

# Tell ESPHome there is a global C++ namespace
weather_helpers_ns = cg.global_ns.namespace("weather_helpers")

# Force inclusion of weather_helpers.cpp even though no platform is registered
cg.add_library("weather_helpers", None)