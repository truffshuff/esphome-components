import esphome.config_validation as cv

# Header-only component: listing `weather_helpers:` in the config is enough for
# ESPHome to copy weather_helpers.h into the build, where every lambda can call
# weather_helpers::... No C++ object is created.
MULTI_CONF = True
MULTI_CONF_NO_DEFAULT = True

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
