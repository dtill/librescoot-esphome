"""Librescoot BLE Client — a single external component that owns the BLE client to the
LibreScoot / unu Scooter Pro nRF52 and exposes every scooter characteristic as a
nested entity. The YAML only names the entities it wants; all parsing, the pairing
flow, the extended-command engine and the OTA diagnostics live in C++.

BLE plumbing that must stay in the YAML (shared, stack-level settings):
    esp32_ble:
      io_capability: keyboard_only   # required for the scooter's passkey pairing
    esp32_ble_tracker:
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE
from esphome import automation
from esphome.components import (
    binary_sensor,
    button,
    esp32,
    esp32_ble,
    esp32_ble_client,
    esp32_ble_tracker,
    lock,
    select,
    sensor,
    switch,
    text,
    text_sensor,
    time,
    update,
)
from esphome.components.esp32_ble import BTLoggers
from esphome.const import (
    CONF_ENTITY_CATEGORY,
    CONF_ID,
    CONF_MAC_ADDRESS,
)

CODEOWNERS = ["@dtill"]
DEPENDENCIES = ["esp32_ble_tracker"]
AUTO_LOAD = [
    "esp32_ble_client",
    "sensor",
    "binary_sensor",
    "text_sensor",
    "select",
    "switch",
    "button",
    "text",
    "lock",
    "update",
]

librescoot_ns = cg.esphome_ns.namespace("librescoot_ble_client")
# The hub IS the BLE client (extends BLEClientBase) — it owns the connection, pairing,
# GATT reads/writes/notifications and every entity.
LibrescootBleClient = librescoot_ns.class_(
    "LibrescootBleClient", esp32_ble_client.BLEClientBase
)
LibrescootButton = librescoot_ns.class_("LibrescootButton", button.Button, cg.Parented.template(LibrescootBleClient))
LibrescootSelect = librescoot_ns.class_("LibrescootSelect", select.Select, cg.Parented.template(LibrescootBleClient))
LibrescootSwitch = librescoot_ns.class_("LibrescootSwitch", switch.Switch, cg.Parented.template(LibrescootBleClient))
LibrescootText = librescoot_ns.class_("LibrescootText", text.Text, cg.Parented.template(LibrescootBleClient))
LibrescootLock = librescoot_ns.class_("LibrescootLock", lock.Lock, cg.Parented.template(LibrescootBleClient))
LibrescootUpdate = librescoot_ns.class_("LibrescootUpdate", update.UpdateEntity, cg.Parented.template(LibrescootBleClient))

BtnAction = librescoot_ns.enum("BtnAction", is_class=True)
SelKind = librescoot_ns.enum("SelKind", is_class=True)
SwKind = librescoot_ns.enum("SwKind", is_class=True)
TxtKind = librescoot_ns.enum("TxtKind", is_class=True)

# Passkey-reply action (component-owned; identical pairing UX to the standalone YAML)
PasskeyReplyAction = librescoot_ns.class_("PasskeyReplyAction", automation.Action)
OtaTransferAction = librescoot_ns.class_("OtaTransferAction", automation.Action)
OtaAbortAction = librescoot_ns.class_("OtaAbortAction", automation.Action)

CONF_TIME_ID = "time_id"
CONF_PASSKEY = "passkey"
CONF_OTA_AUTO_RESUME = "ota_auto_resume"
CONF_PRESENCE_TIMEOUT = "presence_timeout"
CONF_GITHUB_REPO = "github_repo"
CONF_UPDATE_INTERVAL = "update_check_interval"
CONF_LINK_INTERVAL = "link_interval"
CONF_LINK_AUTO_HOLD = "link_auto_hold"
# DEFAULT OTA byte source ("github"/"relay"); distinct key from the `ota_source` select entity.
CONF_OTA_SOURCE_DEFAULT = "ota_source_default"
CONF_FIRMWARE_SOURCE = "firmware_source"
CONF_CA_CERTIFICATE = "ca_certificate"
CONF_USE_CERT_BUNDLE = "use_cert_bundle"
CONF_SCOOTER_FILTER = "scooter_filter"

# ---------------------------------------------------------------------------
# Read entity tables:  yaml_key -> (setter, schema)
# entity_category / icon / units are defaulted here so the YAML only carries names.
# ---------------------------------------------------------------------------
SENSORS = {
    "battery_1_soc": ("set_bat1_soc", dict(unit_of_measurement="%", accuracy_decimals=0, device_class="battery", state_class="measurement", icon="mdi:battery")),
    "battery_1_cycles": ("set_bat1_cycles", dict(accuracy_decimals=0, state_class="measurement", icon="mdi:battery-sync", entity_category="diagnostic")),
    "battery_2_soc": ("set_bat2_soc", dict(unit_of_measurement="%", accuracy_decimals=0, device_class="battery", state_class="measurement", icon="mdi:battery")),
    "battery_2_cycles": ("set_bat2_cycles", dict(accuracy_decimals=0, state_class="measurement", icon="mdi:battery-sync", entity_category="diagnostic")),
    "aux_voltage": ("set_aux_voltage", dict(unit_of_measurement="V", accuracy_decimals=2, state_class="measurement", icon="mdi:lightning-bolt", entity_category="diagnostic")),
    # No device_class="battery" on the auxiliary batteries: Home Assistant's device-level
    # battery badge picks the first battery-class %-sensor it finds, so only the traction
    # batteries (battery_1_soc, then battery_2_soc) stay eligible and the badge tracks
    # Battery 1 SoC. These keep their % unit and battery icon; they just don't drive the badge.
    "aux_level": ("set_aux_level", dict(unit_of_measurement="%", accuracy_decimals=0, state_class="measurement", icon="mdi:battery", entity_category="diagnostic")),
    "cbb_level": ("set_cbb_level", dict(unit_of_measurement="%", accuracy_decimals=0, state_class="measurement", icon="mdi:battery-bluetooth", entity_category="diagnostic")),
    "cbb_remaining": ("set_cbb_remaining", dict(unit_of_measurement="Ah", accuracy_decimals=3, state_class="measurement", icon="mdi:battery-70", entity_category="diagnostic")),
    "cbb_full": ("set_cbb_full", dict(unit_of_measurement="Ah", accuracy_decimals=3, state_class="measurement", icon="mdi:battery", entity_category="diagnostic")),
    "cbb_cell": ("set_cbb_cell", dict(unit_of_measurement="V", accuracy_decimals=3, device_class="voltage", state_class="measurement", icon="mdi:sine-wave", entity_category="diagnostic")),
    "odometer": ("set_odometer", dict(unit_of_measurement="km", accuracy_decimals=3, state_class="total_increasing", icon="mdi:counter")),
    "rssi": ("set_rssi", dict(unit_of_measurement="dBm", accuracy_decimals=0, device_class="signal_strength", state_class="measurement", icon="mdi:signal-variant", entity_category="diagnostic")),
    # OTA throughput/byte sensors, measured from the transfer itself (not from the scooter's
    # OTA_STATUS char). The two speeds publish only every 10 s while a transfer is running.
    "ota_download_speed": ("set_ota_dl_speed", dict(unit_of_measurement="kB/s", accuracy_decimals=2, state_class="measurement", icon="mdi:download-network", entity_category="diagnostic")),
    "ota_ble_upload_speed": ("set_ota_ul_speed", dict(unit_of_measurement="kB/s", accuracy_decimals=2, state_class="measurement", icon="mdi:upload-network", entity_category="diagnostic")),
    "ota_ble_bytes_total": ("set_ota_ble_bytes_total", dict(unit_of_measurement="B", accuracy_decimals=0, state_class="total_increasing", icon="mdi:counter", entity_category="diagnostic")),
    "ota_target_transferred": ("set_ota_target_transferred", dict(unit_of_measurement="B", accuracy_decimals=0, state_class="measurement", icon="mdi:progress-upload", entity_category="diagnostic")),
    # Lifetime count of self-heal auto-resumes (a transfer-phase failure recovered without aborting).
    "ota_auto_resume_count": ("set_ota_selfheal_resumes", dict(accuracy_decimals=0, state_class="total_increasing", icon="mdi:backup-restore", entity_category="diagnostic")),
}

BINARY_SENSORS = {
    "battery_1_present": ("set_bat1_present", dict(device_class="plug", icon="mdi:battery-check", entity_category="diagnostic")),
    "battery_2_present": ("set_bat2_present", dict(device_class="plug", icon="mdi:battery-check", entity_category="diagnostic")),
    "navigation_active": ("set_nav_active", dict(icon="mdi:navigation-variant", entity_category="diagnostic")),
    "ums_status": ("set_ums_status", dict(icon="mdi:usb-flash-drive", entity_category="diagnostic")),
    # LTC4020 auxiliary charger, read-only. Its GATT service (9a590120) is absent on current
    # firmware, so the state is read with the "ltc:status" extended command instead.
    "aux_charger": ("set_aux_charger", dict(icon="mdi:car-battery", entity_category="diagnostic")),
    "maps_available": ("set_maps_available", dict(icon="mdi:map-check", entity_category="diagnostic")),
    "navigation_available": ("set_nav_available", dict(icon="mdi:navigation-outline", entity_category="diagnostic")),
    "ble_connection": ("set_ble_connection", dict(device_class="connectivity", icon="mdi:bluetooth", entity_category="diagnostic")),
    # Machinery for the HA integration's pairing Repair dialog. Add `disabled_by_default: true`
    # in the YAML so it doesn't clutter the device page — the integration enables it on demand.
    "passkey_required": ("set_passkey_required", dict(device_class="problem", icon="mdi:key-alert", entity_category="diagnostic")),
    "pairing_required": ("set_pairing_required_sensor", dict(device_class="problem", icon="mdi:bluetooth-off", entity_category="diagnostic")),
    "reboot_required": ("set_reboot_required", dict(device_class="problem", icon="mdi:restart-alert", entity_category="diagnostic")),
    "ha_integration": ("set_ha_integration", dict(device_class="connectivity", icon="mdi:home-assistant", entity_category="diagnostic")),
    "ble_presence": ("set_ble_presence", dict(device_class="presence", icon="mdi:bluetooth-audio", entity_category="diagnostic")),
}

TEXT_SENSORS = {
    "status": ("set_status", dict(icon="mdi:moped")),
    "seatbox": ("set_seatbox", dict(icon="mdi:toolbox")),
    "handlebar_lock": ("set_handlebar", dict(icon="mdi:lock")),
    "power_state": ("set_power_state", dict(icon="mdi:power-settings")),
    "power_mux": ("set_power_mux", dict(icon="mdi:source-branch", entity_category="diagnostic")),
    "battery_1_state": ("set_bat1_state", dict(icon="mdi:battery-heart-variant", entity_category="diagnostic")),
    "battery_2_state": ("set_bat2_state", dict(icon="mdi:battery-heart-variant", entity_category="diagnostic")),
    "sw_mdb": ("set_sw_mdb", dict(icon="mdi:package-variant-closed", entity_category="diagnostic")),
    "sw_nrf": ("set_sw_nrf", dict(icon="mdi:chip", entity_category="diagnostic")),
    "sw_dbc": ("set_sw_dbc", dict(icon="mdi:chip", entity_category="diagnostic")),
    "sw_esp": ("set_sw_esp", dict(icon="mdi:memory", entity_category="diagnostic")),
    "aux_charge_status": ("set_aux_charge", dict(icon="mdi:battery-charging", entity_category="diagnostic")),
    "cbb_charge_status": ("set_cbb_charge", dict(icon="mdi:battery-charging-wireless", entity_category="diagnostic")),
    "keycard_count": ("set_keycard_count", dict(icon="mdi:card-account-details", entity_category="diagnostic")),
    "command_response": ("set_cmd_response", dict(icon="mdi:message-reply-text", entity_category="diagnostic")),
    "command_last_response": ("set_cmd_last_response", dict(icon="mdi:script-text-outline", entity_category="diagnostic")),
    "ota_status": ("set_ota_status", dict(icon="mdi:progress-download", entity_category="diagnostic")),
    "ota_eta": ("set_ota_eta", dict(icon="mdi:timer-sand", entity_category="diagnostic")),
    "scooter_mac": ("set_scooter_mac_sensor", dict(icon="mdi:bluetooth", entity_category="diagnostic")),
}

# Controls: yaml_key -> (setter_or_None, kind_enum, options_or_minmax, entity_category, icon)
SELECTS = {
    "blinker": ("set_blinker", SelKind.BLINKER, ["off", "left", "right", "both"], None, "mdi:car-light-high"),
    "usb_mode": ("set_usb_mode", SelKind.USB_MODE, ["Normal", "Mass Storage"], "diagnostic", "mdi:usb-flash-drive"),
    "ble_link_mode": ("set_link_mode", SelKind.LINK_MODE, ["disconnect", "scan", "auto", "always", "interval"], "diagnostic", "mdi:bluetooth-connect"),
    "ota_channel": ("set_ota_channel", SelKind.OTA_CHANNEL, ["undefined", "stable", "testing", "nightly"], "diagnostic", "mdi:update"),
    "ota_update_method": ("set_ota_method", SelKind.OTA_METHOD, ["delta", "full"], "diagnostic", "mdi:package-variant"),
    # "direct GitHub" is only offered on a board with PSRAM — see _direct_github_ok().
    "ota_source": ("set_ota_source_select", SelKind.OTA_SOURCE, ["HA relay", "direct GitHub"], "config", "mdi:cloud-download"),
}

def _direct_github_ok() -> bool:
    """Can this board fetch the firmware bytes straight from GitHub?

    That means a TLS session to the release CDN alongside BLE and WiFi, which only fits on a board
    with PSRAM; on internal RAM alone the handshake competes with the Bluetooth stack. Boards without
    it use the Home Assistant relay, so the option is not compiled in at all.
    """
    return "psram" in CORE.config


# yaml_key -> (setter, kind_enum, entity_category, icon, restore_mode)
# restore_mode "DISABLED" is mandatory for anything that commands the scooter: ALWAYS_OFF issues a
# write_state(false) during setup, which for these would mean sending a command to the vehicle on
# every boot. DISABLED leaves the state to whatever the scooter reports.
SWITCHES = {
    "alarm_enabled": ("set_alarm_enabled", SwKind.ALARM_ENABLED, None, "mdi:alarm-light", "DISABLED"),
    "pm_scheduled_hibernate_enabled": ("set_pm_sched_hib", SwKind.PM_SCHED_HIB, "config", "mdi:calendar-clock", "DISABLED"),
    # Runtime OTA controls (no scooter write, just local flags), so ALWAYS_OFF is right here: both
    # start OFF every boot and auto-update can never restore ON and install unattended.
    "ota_stage_only": ("set_stage_only_switch", SwKind.STAGE_ONLY, "diagnostic", "mdi:package-variant-closed", "ALWAYS_OFF"),
    "ota_auto_update": ("set_auto_update_switch", SwKind.AUTO_UPDATE, "diagnostic", "mdi:auto-download", "ALWAYS_OFF"),
}

BUTTONS = {
    "seatbox_open": (BtnAction.SEATBOX_OPEN, None, "mdi:package-variant-closed-plus"),
    "hibernate": (BtnAction.HIBERNATE, None, "mdi:power-sleep"),
    "wakeup": (BtnAction.WAKEUP, None, "mdi:power"),
    "reboot_mdb": (BtnAction.REBOOT, "config", "mdi:restart"),
    "reboot_mdb_hard": (BtnAction.REBOOT_HARD, "config", "mdi:restart-alert"),
    "ble_remove_bond": (BtnAction.REMOVE_BOND, "diagnostic", "mdi:bluetooth-off"),
    "pair_scooter": (BtnAction.PAIR, "diagnostic", "mdi:bluetooth-connect"),
    "ota_status_request": (BtnAction.OTA_STATUS_REQ, "diagnostic", "mdi:progress-question"),
    "ota_abort": (BtnAction.OTA_ABORT, "diagnostic", "mdi:cancel"),
    "ota_mdb_update": (BtnAction.OTA_MDB_UPDATE, "diagnostic", "mdi:cloud-download"),
    "ota_dbc_update": (BtnAction.OTA_DBC_UPDATE, "diagnostic", "mdi:cloud-download"),
    "system_time_sync": (BtnAction.SYSTIME_SYNC, "config", "mdi:clock-check"),
    "restart_esp": (BtnAction.RESTART_ESP, "config", "mdi:restart"),
    "alarm_arm": (BtnAction.ALARM_ARM, None, "mdi:shield-lock"),
    "alarm_disarm": (BtnAction.ALARM_DISARM, None, "mdi:shield-off"),
    "alarm_start": (BtnAction.ALARM_START, None, "mdi:alarm-light-outline"),
    "alarm_stop": (BtnAction.ALARM_STOP, None, "mdi:alarm-off"),
    "navigation_clear": (BtnAction.NAV_CLEAR, "diagnostic", "mdi:map-marker-off"),
    "cancel_hibernate": (BtnAction.CANCEL_HIBERNATE, None, "mdi:power-sleep"),
    "refresh": (BtnAction.REFRESH, "diagnostic", "mdi:refresh"),
}

TEXTS = {
    "command": ("set_command_text", TxtKind.COMMAND, 0, 128, "diagnostic", "mdi:console"),
    "navigation_set": (None, TxtKind.NAV_DEST, 0, 128, "config", "mdi:map-marker-plus"),
    "cellular_apn": ("set_apn_text", TxtKind.CELLULAR_APN, 0, 64, "config", "mdi:sim"),
    "pm_scheduled_hibernate_cron": ("set_pm_cron_text", TxtKind.PM_CRON, 0, 64, "config", "mdi:calendar-clock"),
    "pm_scheduled_hibernate_duration": ("set_pm_duration_text", TxtKind.PM_DURATION, 0, 32, "config", "mdi:timer-sand"),
    "system_time_iso": (None, TxtKind.SYSTIME_ISO, 0, 32, "config", "mdi:clock-edit-outline"),
    "ota_source_url": ("set_ota_source_url_text", TxtKind.OTA_SOURCE_URL, 0, 200, "diagnostic", "mdi:link-variant"),
    "ota_version": ("set_ota_version_text", TxtKind.OTA_VERSION, 0, 64, "diagnostic", "mdi:tag-outline"),
}


def _ekw(ec, icon):
    d = {"icon": icon}
    if ec:
        d["entity_category"] = ec
    return d


_ENTITY_SCHEMAS = {}
_ENTITY_SCHEMAS.update(
    {cv.Optional(k): sensor.sensor_schema(**opts) for k, (_s, opts) in SENSORS.items()}
)
_ENTITY_SCHEMAS.update(
    {cv.Optional(k): binary_sensor.binary_sensor_schema(**opts) for k, (_s, opts) in BINARY_SENSORS.items()}
)
_ENTITY_SCHEMAS.update(
    {cv.Optional(k): text_sensor.text_sensor_schema(**opts) for k, (_s, opts) in TEXT_SENSORS.items()}
)
_ENTITY_SCHEMAS.update(
    {cv.Optional(k): select.select_schema(LibrescootSelect, **_ekw(ec, icon)) for k, (_s, _kd, _o, ec, icon) in SELECTS.items()}
)
_ENTITY_SCHEMAS.update(
    {
        cv.Optional(k): switch.switch_schema(LibrescootSwitch, default_restore_mode=rm, **_ekw(ec, icon))
        for k, (_s, _kd, ec, icon, rm) in SWITCHES.items()
    }
)
_ENTITY_SCHEMAS.update(
    {cv.Optional(k): button.button_schema(LibrescootButton, **_ekw(ec, icon)) for k, (_a, ec, icon) in BUTTONS.items()}
)
_ENTITY_SCHEMAS.update(
    {cv.Optional(k): text.text_schema(LibrescootText, mode="TEXT", **_ekw(ec, icon)) for k, (_s, _kd, _mn, _mx, ec, icon) in TEXTS.items()}
)
_ENTITY_SCHEMAS[cv.Optional("scooter_lock")] = lock.lock_schema(LibrescootLock)

# Two update entities — one per scooter board. They must install sequentially (the scooter
# reboots after each component's install), so they share the single OTA engine.
_ENTITY_SCHEMAS[cv.Optional("mdb_update")] = update.update_schema(
    LibrescootUpdate, icon="mdi:chip", entity_category="diagnostic"
)
_ENTITY_SCHEMAS[cv.Optional("dbc_update")] = update.update_schema(
    LibrescootUpdate, icon="mdi:gauge", entity_category="diagnostic"
)


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LibrescootBleClient),
            cv.Required(CONF_MAC_ADDRESS): cv.mac_address,
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # OTA test flag: when true, transfers stop before COMPLETE (nothing is installed).
            cv.Optional(CONF_OTA_AUTO_RESUME, default=True): cv.boolean,
            # GitHub repo (owner/name) the firmware releases come from.
            cv.Optional(CONF_GITHUB_REPO, default="librescoot/librescoot"): cv.string,
            # How often to poll GitHub for a newer release.
            cv.Optional(CONF_UPDATE_INTERVAL, default="6h"): cv.positive_time_period_milliseconds,
            # "BLE Presence" stays Home if an advert was seen within this window (weak signal).
            cv.Optional(CONF_PRESENCE_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            # "interval" BLE Link Mode: how often to connect, refresh every sensor once, then release.
            cv.Optional(CONF_LINK_INTERVAL, default="5min"): cv.positive_time_period_milliseconds,
            # "auto" BLE Link Mode: hold the connection this long, then briefly drop it (~20 s) so a
            # phone or any other central gets a turn on the scooter's single slot. 0 = never yield
            # proactively (pure failover: hold until the link drops on its own).
            cv.Optional(CONF_LINK_AUTO_HOLD, default="3min"): cv.positive_time_period_milliseconds,
            # OTA byte source default. Omitted → chip-based (ESP32-S3 → direct GitHub, else HA relay).
            cv.Optional(CONF_OTA_SOURCE_DEFAULT): cv.one_of("github", "relay", lower=True),
            # Compile-time default byte source for the OTA transfer (e.g. a local mirror). The
            # runtime "OTA Source URL" text entity still overrides it. Default = GitHub.
            cv.Optional(CONF_FIRMWARE_SOURCE): cv.string,
            # TLS trust for the GitHub HTTPS requests, supplied from YAML instead of baked in:
            #   use_cert_bundle → the ESP-IDF Mozilla bundle (auto-enables the sdkconfig option;
            #     needs PSRAM headroom — the S3, not the classic);
            #   ca_certificate  → explicit PEM root/chain; else the built-in GitHub roots are used.
            cv.Optional(CONF_USE_CERT_BUNDLE, default=False): cv.boolean,
            cv.Optional(CONF_CA_CERTIFICATE): cv.string,
            # Case-insensitive substring an advertised BLE name must contain to count as a scooter
            # for the `discovered_scooters` sensor / the HA integration's new-scooter picker.
            cv.Optional(CONF_SCOOTER_FILTER, default="scooter"): cv.string,
        }
    )
    .extend(_ENTITY_SCHEMAS)
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA),
    esp32_ble.consume_connection_slots(1, "librescoot_ble_client"),
)


async def to_code(config):
    esp32_ble.register_bt_logger(BTLoggers.GATT, BTLoggers.SMP)
    cg.add_define("USE_ESP32_BLE_UUID")

    # The hub is the BLE client itself.
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_client(var, config)
    cg.add(var.set_address(config[CONF_MAC_ADDRESS].as_hex))
    cg.add(var.set_auto_connect(True))

    cg.add(var.set_ota_auto_resume(config[CONF_OTA_AUTO_RESUME]))
    cg.add(var.set_github_repo(config[CONF_GITHUB_REPO]))
    # OTA byte source DEFAULT: YAML `ota_source:` wins; else chip-based — ESP32-S3 (PSRAM, can do the
    # GitHub-CDN TLS) defaults to direct GitHub, every other chip defaults to the HA relay.
    if CONF_OTA_SOURCE_DEFAULT in config:
        _ota_src_default = config[CONF_OTA_SOURCE_DEFAULT]
    else:
        try:
            _ota_src_default = "github" if esp32.get_esp32_variant() == "ESP32S3" else "relay"
        except Exception:  # noqa: BLE001 - be safe if the variant can't be determined
            _ota_src_default = "relay"
    # Release notes are passed through to Home Assistant uncapped, so the limit is the board's RAM:
    # the text is held per component plus a copy in each update entity.
    cg.add_define("LSC_CHANGELOG_MAX", 16000 if _direct_github_ok() else 7000)
    if not _direct_github_ok():
        if _ota_src_default == "github":
            raise cv.Invalid(
                "ota_source_default: 'github' needs a board with PSRAM. Add `psram:` (an ESP32-S3 "
                "N16R8 or similar), or use 'relay' and let the Home Assistant integration serve the "
                "firmware bytes."
            )
    else:
        cg.add_define("LSC_DIRECT_GITHUB")
    cg.add(var.set_ota_source_default(_ota_src_default))
    if CONF_FIRMWARE_SOURCE in config:  # after set_github_repo (which sets the default source)
        cg.add(var.set_firmware_source(config[CONF_FIRMWARE_SOURCE]))
    cg.add(var.set_use_cert_bundle(config[CONF_USE_CERT_BUNDLE]))
    if config[CONF_USE_CERT_BUNDLE]:
        # Enabling the bundle option from YAML pulls in the Mozilla roots (no pinned cert needed).
        esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    if CONF_CA_CERTIFICATE in config:
        cg.add(var.set_ca_certificate(config[CONF_CA_CERTIFICATE]))
    cg.add(var.set_scooter_filter(config[CONF_SCOOTER_FILTER]))
    cg.add(var.set_update_check_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_link_interval_ms(config[CONF_LINK_INTERVAL]))
    cg.add(var.set_auto_hold_ms(config[CONF_LINK_AUTO_HOLD]))
    cg.add(var.set_presence_timeout(config[CONF_PRESENCE_TIMEOUT]))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))

    # --- read entities ---
    for key, (setter, _opts) in SENSORS.items():
        if key in config:
            cg.add(getattr(var, setter)(await sensor.new_sensor(config[key])))
    for key, (setter, _opts) in BINARY_SENSORS.items():
        if key in config:
            cg.add(getattr(var, setter)(await binary_sensor.new_binary_sensor(config[key])))
    for key, (setter, _opts) in TEXT_SENSORS.items():
        if key in config:
            cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[key])))

    # --- selects ---
    for key, (setter, kind, options, _ec, _icon) in SELECTS.items():
        if key in config:
            if key == "ota_source" and not _direct_github_ok():
                options = [o for o in options if o != "direct GitHub"]
            sel = await select.new_select(config[key], options=options)
            await cg.register_parented(sel, var)
            cg.add(sel.set_kind(kind))
            cg.add(getattr(var, setter)(sel))

    # --- switches ---
    for key, (setter, kind, _ec, _icon, _rm) in SWITCHES.items():
        if key in config:
            sw = await switch.new_switch(config[key])
            await cg.register_parented(sw, var)
            cg.add(sw.set_kind(kind))
            cg.add(getattr(var, setter)(sw))

    # --- buttons ---
    for key, (action, _ec, _icon) in BUTTONS.items():
        if key in config:
            btn = await button.new_button(config[key])
            await cg.register_parented(btn, var)
            cg.add(btn.set_action(action))

    # --- text inputs ---
    for key, (setter, kind, mn, mx, _ec, _icon) in TEXTS.items():
        if key in config:
            txt = await text.new_text(config[key], min_length=mn, max_length=mx)
            await cg.register_parented(txt, var)
            cg.add(txt.set_kind(kind))
            if setter is not None:
                cg.add(getattr(var, setter)(txt))

    # --- lock ---
    if "scooter_lock" in config:
        lk = await lock.new_lock(config["scooter_lock"])
        await cg.register_parented(lk, var)
        cg.add(var.set_scooter_lock(lk))

    # --- update entities (one per board) ---
    if "mdb_update" in config:
        up = await update.new_update(config["mdb_update"])
        await cg.register_parented(up, var)
        cg.add(var.set_mdb_update(up))
    if "dbc_update" in config:
        up = await update.new_update(config["dbc_update"])
        await cg.register_parented(up, var)
        cg.add(var.set_dbc_update(up))


# ---------------------------------------------------------------------------
# passkey_reply action  (used by the api: actions: block, exactly like the standalone)
# ---------------------------------------------------------------------------
PASSKEY_REPLY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(LibrescootBleClient),
        cv.Required(CONF_PASSKEY): cv.templatable(cv.int_range(min=0, max=999999)),
    }
)


@automation.register_action(
    "librescoot_ble_client.passkey_reply",
    PasskeyReplyAction,
    PASSKEY_REPLY_SCHEMA,
    synchronous=True,
)
async def passkey_reply_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    templ = await cg.templatable(config[CONF_PASSKEY], args, cg.uint32)
    cg.add(var.set_passkey(templ))
    return var


# ---------------------------------------------------------------------------
# ota_transfer action — stream an explicit URL to the scooter over BLE-OTA. This is how the
# transfer engine is tested against a local `python -m http.server` before GitHub/TLS.
# ---------------------------------------------------------------------------
OTA_TRANSFER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(LibrescootBleClient),
        cv.Required("url"): cv.templatable(cv.string),
        cv.Required("size"): cv.templatable(cv.positive_not_null_int),
        cv.Optional("sha256", default=""): cv.templatable(cv.string),
        cv.Required("bundle_id"): cv.templatable(cv.string),
        cv.Optional("component", default=0): cv.templatable(cv.int_range(min=0, max=1)),
    }
)


@automation.register_action(
    "librescoot_ble_client.ota_transfer", OtaTransferAction, OTA_TRANSFER_SCHEMA, synchronous=True
)
async def ota_transfer_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_url(await cg.templatable(config["url"], args, cg.std_string)))
    cg.add(var.set_size(await cg.templatable(config["size"], args, cg.uint32)))
    cg.add(var.set_sha256(await cg.templatable(config["sha256"], args, cg.std_string)))
    cg.add(var.set_bundle_id(await cg.templatable(config["bundle_id"], args, cg.std_string)))
    cg.add(var.set_component(await cg.templatable(config["component"], args, cg.uint8)))
    return var


@automation.register_action(
    "librescoot_ble_client.ota_abort",
    OtaAbortAction,
    cv.Schema({cv.GenerateID(): cv.use_id(LibrescootBleClient)}),
    synchronous=True,
)
async def ota_abort_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
