import esphome.codegen as cg

# Declare the C++ namespace for header-only helper functions
# No component registration needed - functions are inline in weather_helpers.h
weather_helpers_ns = cg.global_ns.namespace("weather_helpers")