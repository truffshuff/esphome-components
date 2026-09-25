"""NimBLE BLE tracker for ESP32 - the BLEHub that stands in for esp32_ble_tracker.

Same YAML surface as esp32_ble_tracker for scanning and automations:
scan_parameters (interval, window, connection_scan_window, duration, active,
continuous), on_ble_advertise / on_ble_service_data_advertise /
on_ble_manufacturer_data_advertise / on_scan_end, and the
nimble_tracker.start_scan / nimble_tracker.stop_scan actions. Consumers bind
through ble_device_base (ble_hub_id), exactly as they bind to
esp32_ble_tracker on a Bluedroid build.

Scan-window defaults follow esp32_ble_tracker on ESPHome 2026.9.0: 320 ms
interval; with WiFi (software coexistence) and ESP-IDF >= 5.5.5 an unset
window is raised to the interval and a 30 ms connection_scan_window is armed,
otherwise the window defaults to 30 ms.
"""

import copy
import logging

from esphome import automation
import esphome.codegen as cg
from esphome.components import ble_device_base, nimble_base, ota
from esphome.components.ble_device_base import automation as ble_automation
from esphome.components.const import CONF_ON_SCAN_END, CONF_SCAN_PARAMETERS, CONF_WINDOW
from esphome.components.esp32 import idf_version
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACTIVE,
    CONF_CONTINUOUS,
    CONF_DURATION,
    CONF_ID,
    CONF_INTERVAL,
    CONF_MANUFACTURER_ID,
    CONF_ON_BLE_ADVERTISE,
    CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE,
    CONF_ON_BLE_SERVICE_DATA_ADVERTISE,
    CONF_SERVICE_UUID,
)
from esphome.core import CORE, ID, TimePeriod
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["esp32", "nimble_base"]
AUTO_LOAD = ["ble_device_base"]
CODEOWNERS = ["@truffshuff"]

ble_device_base.register_hub_provider("nimble_tracker")

CONF_NIMBLE_TRACKER_ID = "nimble_tracker_id"
CONF_NIMBLE_BASE_ID = "nimble_base_id"
CONF_CONNECTION_SCAN_WINDOW = ble_device_base.CONF_CONNECTION_SCAN_WINDOW

nimble_tracker_ns = cg.esphome_ns.namespace("nimble_tracker")
NimBLETracker = nimble_tracker_ns.class_(
    "NimBLETracker", ble_device_base.BLEHub, cg.Component
)
StartScanAction = nimble_tracker_ns.class_("StartScanAction", automation.Action)
StopScanAction = nimble_tracker_ns.class_("StopScanAction", automation.Action)

# esp32_ble_tracker constants (ESPHome 2026.9.0).
IDF_SCAN_WINDOW_FIX_VERSION = cv.Version(5, 5, 5)
MAX_RECOMMENDED_WIFI_SCAN_WINDOW = TimePeriod(milliseconds=600)

_KEY_WINDOW_DEFAULTED = "nimble_tracker_scan_window_defaulted"


def _scan_window_default() -> TimePeriod:
    # Recorded so the raise below can tell a defaulted window from an explicit one.
    CORE.data[_KEY_WINDOW_DEFAULTED] = True
    return cv.positive_time_period(ble_device_base.DEFAULT_SCAN_WINDOW)


def _has_wifi() -> bool:
    return "wifi" in CORE.loaded_integrations


def _raise_defaulted_scan_window(config: ConfigType) -> ConfigType:
    """esp32_ble_tracker parity: with WiFi + the coexistence arbiter on IDF
    >= 5.5.5, a defaulted window becomes the interval and a 30 ms connection
    window is armed; an explicit window is never touched."""
    params = config[CONF_SCAN_PARAMETERS]
    if (
        CORE.data.get(_KEY_WINDOW_DEFAULTED)
        and _has_wifi()
        and idf_version() >= IDF_SCAN_WINDOW_FIX_VERSION
    ):
        params[CONF_WINDOW] = copy.copy(params[CONF_INTERVAL])
        if CONF_CONNECTION_SCAN_WINDOW not in params:
            params[CONF_CONNECTION_SCAN_WINDOW] = cv.positive_time_period(
                ble_device_base.DEFAULT_SCAN_WINDOW
            )
    if (
        connection_window := params.get(CONF_CONNECTION_SCAN_WINDOW)
    ) is not None and connection_window > params[CONF_WINDOW]:
        raise cv.Invalid(
            f"{CONF_CONNECTION_SCAN_WINDOW} ({connection_window}) needs to be "
            f"smaller than the scan window ({params[CONF_WINDOW]})",
            path=[CONF_SCAN_PARAMETERS, CONF_CONNECTION_SCAN_WINDOW],
        )
    return config


def _warn_long_scan_window_with_wifi(config: ConfigType) -> ConfigType:
    window = config[CONF_SCAN_PARAMETERS][CONF_WINDOW]
    if _has_wifi() and window > MAX_RECOMMENDED_WIFI_SCAN_WINDOW:
        _LOGGER.warning(
            "BLE scan window of %s with wifi on the same radio can starve wifi; "
            "keep the window at or below %s",
            window,
            MAX_RECOMMENDED_WIFI_SCAN_WINDOW,
        )
    return config


SCAN_PARAMETERS_SCHEMA = ble_device_base.scan_parameters_schema(
    "320ms", window_default=_scan_window_default, connection_window=True
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NimBLETracker),
            cv.GenerateID(CONF_NIMBLE_BASE_ID): cv.use_id(nimble_base.NimBLEBase),
            cv.Optional(CONF_SCAN_PARAMETERS, default={}): SCAN_PARAMETERS_SCHEMA,
            cv.Optional(CONF_ON_BLE_ADVERTISE): ble_automation.advertise_trigger_schema(
                ble_automation.ESPBTAdvertiseTrigger
            ),
            cv.Optional(
                CONF_ON_BLE_SERVICE_DATA_ADVERTISE
            ): ble_automation.uuid_trigger_schema(
                ble_automation.BLEServiceDataAdvertiseTrigger,
                {cv.Required(CONF_SERVICE_UUID): ble_device_base.bt_uuid},
            ),
            cv.Optional(
                CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE
            ): ble_automation.uuid_trigger_schema(
                ble_automation.BLEManufacturerDataAdvertiseTrigger,
                {cv.Required(CONF_MANUFACTURER_ID): ble_device_base.bt_uuid},
            ),
            cv.Optional(CONF_ON_SCAN_END): ble_automation.scan_end_trigger_schema(
                ble_automation.BLEEndOfScanTrigger
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _raise_defaulted_scan_window,
    _warn_long_scan_window_with_wifi,
)


def request_scanner_state_callback() -> None:
    """Compile the push scanner-state slot the proxy registers with."""
    cg.add_define("USE_BLE_SCANNER_STATE_CALLBACK")


async def to_code(config: ConfigType) -> None:
    # Selects the NimBLE arm of the BLEHub alias (ble_device_base/ble_hub_impl.h).
    cg.add_define("USE_NIMBLE_BLE_TRACKER")
    # NimBLE reports advertisement and scan response separately; merge them
    # the way the IDF Bluedroid stack does for esp32_ble_tracker.
    cg.add_define("USE_BLE_SCAN_RESPONSE_MERGER")
    # The tracker always pushes scanner-state transitions; the BLEHub contract
    # requires the push surface exactly when this is defined.
    request_scanner_state_callback()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_NIMBLE_BASE_ID])

    ota.request_ota_state_listeners()

    scan = config[CONF_SCAN_PARAMETERS]
    cg.add(var.set_scan_interval(ble_device_base.to_ble_units(scan[CONF_INTERVAL])))
    cg.add(var.set_scan_window(ble_device_base.to_ble_units(scan[CONF_WINDOW])))
    if (connection_window := scan.get(CONF_CONNECTION_SCAN_WINDOW)) is not None:
        cg.add(var.set_connection_scan_window(ble_device_base.to_ble_units(connection_window)))
    cg.add(var.set_scan_duration(scan[CONF_DURATION].total_milliseconds))
    cg.add(var.set_scan_active(scan[CONF_ACTIVE]))
    cg.add(var.set_configured_continuous(scan[CONF_CONTINUOUS]))

    for conf in config.get(CONF_ON_BLE_ADVERTISE, []):
        await ble_automation.advertise_trigger_to_code(conf, var)
    for trigger_key, uuid_key, setter_prefix in (
        (CONF_ON_BLE_SERVICE_DATA_ADVERTISE, CONF_SERVICE_UUID, "set_service_uuid"),
        (
            CONF_ON_BLE_MANUFACTURER_DATA_ADVERTISE,
            CONF_MANUFACTURER_ID,
            "set_manufacturer_uuid",
        ),
    ):
        for conf in config.get(trigger_key, []):
            await ble_automation.uuid_trigger_to_code(conf, var, uuid_key, setter_prefix)
    for conf in config.get(CONF_ON_SCAN_END, []):
        await ble_automation.scan_end_trigger_to_code(conf, var)


@automation.register_action(
    "nimble_tracker.start_scan",
    StartScanAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(NimBLETracker),
            cv.Optional(CONF_CONTINUOUS): cv.templatable(cv.boolean),
        }
    ),
    synchronous=True,
)
async def start_scan_action_to_code(
    config: ConfigType, action_id: ID, template_arg: cg.TemplateArguments, args: list
) -> cg.MockObj:
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if (continuous := config.get(CONF_CONTINUOUS)) is not None:
        template_ = await cg.templatable(continuous, args, cg.bool_)
        cg.add(var.set_continuous(template_))
    return var


@automation.register_action(
    "nimble_tracker.stop_scan",
    StopScanAction,
    automation.maybe_simple_id(cv.Schema({cv.GenerateID(): cv.use_id(NimBLETracker)})),
    synchronous=True,
)
async def stop_scan_action_to_code(
    config: ConfigType, action_id: ID, template_arg: cg.TemplateArguments, args: list
) -> cg.MockObj:
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
