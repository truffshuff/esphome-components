"""
NimBLE Base Component
Provides core NimBLE stack initialization and service registration for ESP32
Used by nimble_proxy and nimble_improv components
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
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
    # ESPHome 2026.9.0 (esphome#18599) excludes the ESP-IDF `bt` component from
    # the build by default and re-includes it only when a component calls
    # esp32.request_bluetooth(). Without this, every header we pull from the
    # `bt` component disappears from the include path and the build fails with:
    #
    #   nimble_proxy/nimble_proxy.h:12:10: fatal error: esp_bt.h:
    #   No such file or directory
    #
    # nimble_base is the dependency root for nimble_proxy and nimble_improv
    # (both list it in DEPENDENCIES), so this one call covers all three.
    # `bt` pulls esp_coex and esp_wifi back via its own REQUIRES.
    #
    # hasattr guard keeps this component building on ESPHome < 2026.9.0, where
    # request_bluetooth() does not exist and `bt` was never excluded.
    if hasattr(esp32, "request_bluetooth"):
        esp32.request_bluetooth()

    # WiFi/BT software coexistence. The ESP32-S3 has ONE 2.4 GHz radio shared
    # between WiFi and BLE; without arbitration they collide and WiFi
    # *reception* collapses while transmission still looks fine.
    #
    # ESP-IDF's ESP_COEX_SW_COEXIST_ENABLE is "default y" whenever WiFi and BT
    # are both enabled, so this used to come for free. ESPHome 2026.7.0
    # (esphome#17008) added a reconciler that decides it explicitly:
    #
    #     if net.software_coexistence:
    #         set_idf_sdkconfig_default("CONFIG_SW_COEXIST_ENABLE", True)
    #     else:
    #         set_idf_sdkconfig_default("CONFIG_SW_COEXIST_ENABLE", False)
    #
    # Only esp32_ble_tracker calls request_software_coexistence(), so a NimBLE
    # build silently landed on False from 2026.7.0 onward.
    #
    # Observed on an ESP32-S3 at -48 dBm with noise -96 and AP-reported
    # satisfaction 100: 26,195 packets transmitted against 330 received,
    # rx_rate oscillating between the 6 Mbps basic-rate floor and 39 Mbps,
    # OTA downloads starving to ~0.6 kB/s or timing out in the handshake, and
    # repeated getaddrinfo EAI_FAIL on inbound DNS responses.
    if hasattr(esp32, "request_software_coexistence"):
        esp32.request_software_coexistence()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # No library dependency needed - using ESP-IDF native NimBLE
