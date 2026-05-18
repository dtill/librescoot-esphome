"""LSC (Librescoot) Battery NFC interface component for ESPHome.

Reads the unu Scooter Pro main battery via a PN532 NFC reader, mirrors the
battery-service Go FSM (heartbeat cycle = CMD_SEATBOX_CLOSED + CMD_ON/OFF
every 10 s, seatbox-state-aware command flow, just-inserted wake-up, BMS
state-mismatch recovery), and exposes the result as standard HA entities.

Phase 2 of the project; Phase 1 (lambda + includes header) stays in
esphome_configs/librescoot-batt-nfc-interface-cffbde.yaml as a reference.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    button,
    pn532,
    sensor,
    switch,
    text_sensor,
)
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)

CODEOWNERS = ["@dtill"]
DEPENDENCIES = ["pn532"]
AUTO_LOAD = ["sensor", "text_sensor", "binary_sensor", "switch", "button"]
MULTI_CONF = False

lsc_battery_nfc_ns = cg.esphome_ns.namespace("lsc_battery_nfc")
LSCBatteryNFC = lsc_battery_nfc_ns.class_("LSCBatteryNFC", cg.PollingComponent)
LSCHighCurrentSwitch = lsc_battery_nfc_ns.class_(
    "LSCHighCurrentSwitch", switch.Switch, cg.Parented.template(LSCBatteryNFC)
)
LSCSeatboxSwitch = lsc_battery_nfc_ns.class_(
    "LSCSeatboxSwitch", switch.Switch, cg.Parented.template(LSCBatteryNFC)
)
LSCFullDumpButton = lsc_battery_nfc_ns.class_(
    "LSCFullDumpButton", button.Button, cg.Parented.template(LSCBatteryNFC)
)
LSCManualRefreshButton = lsc_battery_nfc_ns.class_(
    "LSCManualRefreshButton", button.Button, cg.Parented.template(LSCBatteryNFC)
)

# ---------------------------------------------------------------------------
# Config keys
# ---------------------------------------------------------------------------

CONF_PN532_ID = "pn532_id"
CONF_HEARTBEAT_INTERVAL = "heartbeat_interval"
CONF_KEEP_ACTIVE_ON_SEATBOX_OPEN = "keep_active_on_seatbox_open"

# Sensors
CONF_VOLTAGE = "voltage"
CONF_CURRENT = "current"
CONF_LEVEL = "level"
CONF_HEALTH = "health"
CONF_CYCLE_COUNT = "cycle_count"
CONF_REMAINING_CAPACITY = "remaining_capacity"
CONF_FULL_CAPACITY = "full_capacity"
CONF_FAULT_CODE = "fault_code"
CONF_TEMP_0 = "temp_0"
CONF_TEMP_1 = "temp_1"
CONF_TEMP_2 = "temp_2"
CONF_TEMP_3 = "temp_3"

# Text sensors
CONF_STATE = "state"
CONF_FAULT_DESCRIPTION = "fault_description"
CONF_SERIAL = "serial"
CONF_FIRMWARE = "firmware"
CONF_MANUFACTURING_DATE = "manufacturing_date"
CONF_MANUFACTURER = "manufacturer"

# Binary sensors
CONF_TAG_PRESENT = "tag_present"
CONF_BATTERY_DETECTED = "battery_detected"
CONF_LOW_SOC = "low_soc"

# Controls
CONF_HIGH_CURRENT_PATH = "high_current_path"
CONF_SEATBOX_CLOSED = "seatbox_closed"
CONF_FULL_DUMP = "full_dump"
CONF_MANUAL_REFRESH = "manual_refresh"


# ---------------------------------------------------------------------------
# Config schema
# ---------------------------------------------------------------------------

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LSCBatteryNFC),
        cv.Required(CONF_PN532_ID): cv.use_id(pn532.PN532),
        cv.Optional(
            CONF_HEARTBEAT_INTERVAL, default="10s"
        ): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_KEEP_ACTIVE_ON_SEATBOX_OPEN, default=False): cv.boolean,
        # ----- Primary sensors -----------------------------------------------
        cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement="mV",
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CURRENT): sensor.sensor_schema(
            unit_of_measurement="mA",
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HEALTH): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CYCLE_COUNT): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
        ),
        # ----- Diagnostic sensors --------------------------------------------
        cv.Optional(CONF_REMAINING_CAPACITY): sensor.sensor_schema(
            unit_of_measurement="mAh",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FULL_CAPACITY): sensor.sensor_schema(
            unit_of_measurement="mAh",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FAULT_CODE): sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_TEMP_0): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_TEMP_1): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_TEMP_2): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_TEMP_3): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # ----- Text sensors --------------------------------------------------
        cv.Optional(CONF_STATE): text_sensor.text_sensor_schema(),
        cv.Optional(CONF_FAULT_DESCRIPTION): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_SERIAL): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_FIRMWARE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_MANUFACTURING_DATE): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_MANUFACTURER): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # ----- Binary sensors ------------------------------------------------
        cv.Optional(CONF_TAG_PRESENT): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_BATTERY_DETECTED): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_LOW_SOC): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_BATTERY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # ----- Switches ------------------------------------------------------
        # High-current path: always-off on boot (safety — don't come back from
        # a reboot with the battery's discharge path enabled).
        cv.Optional(CONF_HIGH_CURRENT_PATH): switch.switch_schema(
            LSCHighCurrentSwitch,
            default_restore_mode="ALWAYS_OFF",
        ),
        # Seatbox: persist across reboots, default "closed" (battery is in the
        # scooter with the seat shut) on fresh flash. Matches the internal
        # default seatbox_closed_ = true so UI and FSM stay in sync.
        cv.Optional(CONF_SEATBOX_CLOSED): switch.switch_schema(
            LSCSeatboxSwitch,
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="RESTORE_DEFAULT_ON",
        ),
        # ----- Buttons -------------------------------------------------------
        cv.Optional(CONF_FULL_DUMP): button.button_schema(
            LSCFullDumpButton,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_MANUAL_REFRESH): button.button_schema(
            LSCManualRefreshButton,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.polling_component_schema("2s"))


# ---------------------------------------------------------------------------
# Codegen
# ---------------------------------------------------------------------------


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    pn = await cg.get_variable(config[CONF_PN532_ID])
    cg.add(var.set_pn532(pn))
    cg.add(var.set_heartbeat_interval(config[CONF_HEARTBEAT_INTERVAL]))
    cg.add(var.set_keep_active_on_seatbox_open(config[CONF_KEEP_ACTIVE_ON_SEATBOX_OPEN]))

    # ---- Sensors --------------------------------------------------------
    for key, setter in (
        (CONF_VOLTAGE, "set_voltage_sensor"),
        (CONF_CURRENT, "set_current_sensor"),
        (CONF_LEVEL, "set_level_sensor"),
        (CONF_HEALTH, "set_health_sensor"),
        (CONF_CYCLE_COUNT, "set_cycle_count_sensor"),
        (CONF_REMAINING_CAPACITY, "set_remaining_capacity_sensor"),
        (CONF_FULL_CAPACITY, "set_full_capacity_sensor"),
        (CONF_FAULT_CODE, "set_fault_code_sensor"),
        (CONF_TEMP_0, "set_temp_0_sensor"),
        (CONF_TEMP_1, "set_temp_1_sensor"),
        (CONF_TEMP_2, "set_temp_2_sensor"),
        (CONF_TEMP_3, "set_temp_3_sensor"),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    # ---- Text sensors ---------------------------------------------------
    for key, setter in (
        (CONF_STATE, "set_state_text_sensor"),
        (CONF_FAULT_DESCRIPTION, "set_fault_description_text_sensor"),
        (CONF_SERIAL, "set_serial_text_sensor"),
        (CONF_FIRMWARE, "set_firmware_text_sensor"),
        (CONF_MANUFACTURING_DATE, "set_manufacturing_date_text_sensor"),
        (CONF_MANUFACTURER, "set_manufacturer_text_sensor"),
    ):
        if key in config:
            ts = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(ts))

    # ---- Binary sensors -------------------------------------------------
    for key, setter in (
        (CONF_TAG_PRESENT, "set_tag_present_binary_sensor"),
        (CONF_BATTERY_DETECTED, "set_battery_detected_binary_sensor"),
        (CONF_LOW_SOC, "set_low_soc_binary_sensor"),
    ):
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, setter)(bs))

    # ---- Switches -------------------------------------------------------
    if CONF_HIGH_CURRENT_PATH in config:
        sw = await switch.new_switch(config[CONF_HIGH_CURRENT_PATH])
        await cg.register_parented(sw, var)
        cg.add(var.set_high_current_switch(sw))

    if CONF_SEATBOX_CLOSED in config:
        sw = await switch.new_switch(config[CONF_SEATBOX_CLOSED])
        await cg.register_parented(sw, var)
        cg.add(var.set_seatbox_switch(sw))

    # ---- Buttons --------------------------------------------------------
    if CONF_FULL_DUMP in config:
        btn = await button.new_button(config[CONF_FULL_DUMP])
        await cg.register_parented(btn, var)
        cg.add(var.set_full_dump_button(btn))

    if CONF_MANUAL_REFRESH in config:
        btn = await button.new_button(config[CONF_MANUAL_REFRESH])
        await cg.register_parented(btn, var)
        cg.add(var.set_manual_refresh_button(btn))
