# librescoot-esphome
ESPHome Components and configurations for "UNU Scooter Pro" with opensource [Librescoot Firmware](https://github.com/librescoot)


- [MDB nRF-BLE-Client](#mdb-nrf-ble-client)
    - [Pairing ESP32 with Scooter Pro nRF via ESPHome BLE](#pairing-esp32-with-scooter-pro-nrf-via-esphome-ble)
    - [Using the Extended Command](#using-the-extended-command)
    - [Exposed Entities](#exposed-entities)
- [CBB monitoring via I²C addr 0x36 and 0x0B](#cbb-monitoring-via-ic-addr-0x36-and-0x0b)
    - [Wiring](#wiring)
    - [I2C Addressing](#i2c-addressing)
    - [Sensors](#sensors)
    - [Framework](#framework)
    - [Console Output](#console-output)
- [DBC Status LED driver via I²C addr 0x30](#dbc-status-led-driver-via-ic-addr-0x30)
    - [Wiring](#wiring-1)
    - [I²C Addressing](#ic-addressing)
    - [Channels](#channels)
    - [ESPHome integration](#esphome-integration)
    - [Manual controls](#manual-controls)
    - [Framework](#framework-1)
    - [Log output](#log-output)
- [Battery NFC interface via PN532](#battery-NFC-interface-via-PN532)

---

## Example .yaml files:

### [MDB nRF-BLE-Client](librescoot-nrf-ble-client-example.yaml)

**Component edition.** BLE interface for the Unu-Scooter Pro (Librescoot FW, nRF >v2.0.0-ls),
built as the [`librescoot_ble_client`](components/librescoot_ble_client/) external component.
Where the [standalone edition](librescoot-ble-client-minimal-example.yaml) wires ~25 `ble_client` characteristic
sensors and template entities together in YAML, this edition puts all of that — the BLE
client, the passkey pairing, every characteristic parser, the extended-command engine, the
BLE link manager and the OTA-status diagnostics — into one C++ component. **The YAML only
names the entities it wants.** Same functionality, a fraction of the YAML.
More info about the characteristics at [LibreScoot Tech Reference - Bluetooth Interface Documentation](https://reference.librescoot.org/dev/bluetooth/)

```yaml
external_components:
  - source:
      type: local
      path: my_components
    components: [librescoot_ble_client]

# BLE stack plumbing (shared, stack-level; stays in the YAML)
esp32_ble:
  io_capability: keyboard_only   # required for the scooter's passkey pairing
esp32_ble_tracker:

librescoot_ble_client:
  id: librescoot_ble_client_hub
  mac_address: !secret librescoot_nrf_ble_mac_addr
  time_id: sntp_time             # optional, enables the clock-set entities
  # --- optional OTA settings (defaults shown) ---
  github_repo: "librescoot/librescoot"   # repo the firmware releases come from
  update_check_interval: 6h              # how often to poll GitHub for a newer release
  stage_only: false                      # true = transfers stop before COMPLETE (no install)

  status:      {name: "Status"}
  odometer:    {name: "Odometer"}
  scooter_lock: {name: "Scooter Lock"}
  # ... only the entities you name are created (full list in the component README) ...
```

The complete option and entity-key reference lives in the
[component README](components/librescoot_ble_client/README.md).

#### Pairing ESP32 with Scooter Pro nRF via ESPHome BLE
> [!WARNING]
> ### A paired ESP is a key to your scooter — read this first
>
> Pairing the ESP with the scooter creates a **bond** that is stored in the **ESP32's flash
> (NVS)**. It **survives reboots — and survives re-flashing the firmware.** A normal
> `esphome run` / OTA update does **not** erase it.
>
> - **Anyone holding a paired ESP can connect to and control your scooter** — even after
>   flashing completely different firmware onto that ESP. Treat a paired ESP like a physical
>   key: don't give it away or throw it out while it's still bonded.
> - **The scooter can only forget _all_ bonds at once**, never a single device. If you clear
>   the ESP's bond from the scooter side, you also **lose your phone pairing** and have to
>   re-pair your phone.
> - **To remove the bond from the ESP:** press **"BLE Remove Bond"** in Home Assistant, or
>   erase the ESP's `nvs` flash region with `esptool` (a plain re-flash won't touch it).
> 

An ESP32 board running ESPHome connects to the Scooter Pro's nRF BLE chip over Bluetooth. The connection is secured with a one-time 6-digit passkey that gets generated during pairing. The component performs the pairing internally; the passkey is entered from Home Assistant.

Setup:

1. Flash the ESPHome [nRF-BLE-Client](librescoot-nrf-ble-client-example.yaml) onto your ESP32. Set all secrets in your ESPHome-Device-Builder's `secrets.yaml`, especially your scooter's BLE MAC address.
2. Add the ESP32 device to Home Assistant via the ESPHome integration.

Pairing:

1. Turn on the scooter into parked mode. When the ESP32 first connects to the scooter, it will ask for a passkey. To enter it:
2. Open Home Assistant and go to **Settings → Developer Tools → Actions** (or go directly to `/config/developer-tools/action` in your browser).
3. In the action search field, type **"passkey"** — you'll see the `passkey_reply` action appear.
4. Select it. A field for a 6-digit code will show up.
5. Check the scooter's dashboard (DBC) for the passkey the scooter is expecting, enter it in HA, and hit **Perform Action**.
6. Once accepted, the connection is established. The bond is saved — future reconnects happen automatically.

Wire the action to the component:

```yaml
api:
  actions:
    - action: passkey_reply
      variables:
        pin: int
      then:
        - librescoot_ble_client.passkey_reply:
            id: librescoot_ble_client_hub
            passkey: !lambda "return pin;"
```

> When switching to a new ESP chip, first press **BLE Remove Bond** (disabled by default — enable it in HA first) *and* remove the bond on the scooter side, otherwise the new chip cannot re-pair against the stale bond.

#### Using the Extended Command

The nRF exposes a text command channel (write on `9a590401`, responses notify on `9a590402`). The **Command** text input sends an arbitrary command string; the component collects the reply with a terminator-aware engine (it stops on `:ok`, `:error:`, a standalone line, or a `…:count:<n>` header followed by *n* lines) with a 20 s timeout fallback. Two sensors surface the result:

- **Command last response** — the cleaned, correlated reply (truncated to 255 characters, the Home Assistant text-state limit; the full text is visible in the ESPHome web server / log).
- **Command response** — the raw notification feed as it arrives.

Many commands are already wrapped as first-class entities (the alarm switches, USB Mode, the navigation and power-management controls, the config text inputs, etc.), so the freeform channel is mainly for ad-hoc queries such as `cap:list` (the default prefill) or `cap:<category>`.

Configuration keys use a `get:`/`set:` convention. Read-only queries the component runs on connect (they do **not** actuate the scooter): `status:version:dbc`, `status:maps-available`, `status:navigation-available`, `keycard:count`, `get:cellular.apn`, `get:pm.scheduled-hibernate-*`.

#### Exposed Entities

Entity keys are opt-in: only keys you declare with a `name` are instantiated. All read
entities become *unknown* while the BLE link is down and repopulate on reconnect. "Update"
is the poll interval; `on connect` values are queried once per connection, `on notify` are
pushed by the scooter, and `—` marks action/control entities with no periodic state.

**Vehicle & battery telemetry (read)**

| Key | Entity | Type | Update | Description |
| --- | --- | --- | --- | --- |
| `status` | Status | Text Sensor | 5 s | Operating state (`ready-to-drive`, `parked`, `stand-by`). |
| `seatbox` | Seatbox | Text Sensor | 5 s | Seatbox open/closed. |
| `handlebar_lock` | Handlebar Lock | Text Sensor | 5 s | Handlebar lock state. |
| `power_state` | Power State | Text Sensor | 5 s | Power-management state. |
| `power_mux` | MDB Power Mux Selected Input | Text Sensor | 600 s | Selected power input. |
| `odometer` | Odometer | Sensor (km) | 120 s | Total distance travelled. |
| `battery_1_soc` / `battery_2_soc` | Battery n SoC | Sensor (%) | 60 s | Main battery charge (unknown when unplugged). |
| `battery_1_cycles` / `battery_2_cycles` | Battery n Cycles | Sensor | 600 s | Charge cycles (unknown when unplugged). |
| `battery_1_state` / `battery_2_state` | Battery n State | Text Sensor | 120 s | Battery state (unknown when unplugged). |
| `battery_1_present` / `battery_2_present` | Battery n Present | Binary Sensor | 10 s | Slot occupied. |
| `aux_voltage` | Aux Battery Voltage | Sensor (V) | 60 s | 12 V aux battery voltage. |
| `aux_level` | Aux Battery Level | Sensor (%) | 60 s | Aux battery charge. |
| `aux_charge_status` | Aux Charge Status | Text Sensor | 120 s | Aux charge state. |
| `cbb_level` | CBB Battery Level | Sensor (%) | 60 s | Connectivity battery charge. |
| `cbb_charge_status` | CBB Charge Status | Text Sensor | 120 s | CBB charge state. |
| `cbb_remaining` / `cbb_full` | CBB Remaining / Full Capacity | Sensor (Ah) | 600 s | µAh → Ah. |
| `cbb_cell` | CBB Cell Voltage | Sensor (V) | 120 s | µV → V. |
| `navigation_active` | Navigation Active | Binary Sensor | 60 s | Navigation running. |
| `ums_status` | UMS Status | Binary Sensor | 60 s | USB mass-storage active. |
| `maps_available` | Maps Available | Binary Sensor | on connect | Offline maps present. |
| `navigation_available` | Navigation Available | Binary Sensor | on connect | Navigation service available. |
| `keycard_count` | Keycard Count | Text Sensor | on connect | Registered keycards. |

**Versions**

| Key | Entity | Type | Update | Description |
| --- | --- | --- | --- | --- |
| `sw_mdb` | SW MDB | Text Sensor | 1800 s + on connect | i.MX (MDB) version. |
| `sw_nrf` | SW nRF | Text Sensor | 1800 s + on connect | nRF52 firmware version. |
| `sw_dbc` | SW DBC | Text Sensor | on connect | Dashboard version. |
| `sw_esp` | SW ESP | Text Sensor | static | ESPHome build of this bridge. |

**BLE link**

| Key | Entity | Type | Update | Description |
| --- | --- | --- | --- | --- |
| `ble_connection` | BLE Connection | Binary Sensor | 1 s | Connected to the scooter. |
| `ble_presence` | BLE Presence | Binary Sensor | 1 s | Connected, or advertisement seen while scanning. |
| `rssi` | BLE RSSI | Sensor (dBm) | 60 s | Link signal strength. |
| `ble_link_mode` | BLE Link Mode | Select | — | `disconnect` / `scan` / `auto` / `always`; persisted. `auto` releases the link for ~20 s on each disconnect so a phone can take over; any OTA pins the link up. |

> **Connect-on-demand:** in `scan`/`disconnect` the link is down, but triggering any control
> still works — the component brings the link up, replays the action, waits for the reply,
> then releases the link again.

**Lock & controls**

| Key | Entity | Type | Description |
| --- | --- | --- | --- |
| `scooter_lock` | Scooter Lock | Lock | Lock = `stand-by`, unlock = `ready-to-drive`; reflected, defaults LOCKED. |
| `blinker` | Blinker | Select | `off` / `left` / `right` / `both`. |
| `usb_mode` | USB Mode | Select | `Normal` / `Mass Storage`, reflected from UMS. |
| `seatbox_open` | Seatbox Open | Button | Open the seatbox. |
| `hibernate` / `wakeup` | Hibernate / Wakeup | Button | Power management. |
| `reboot_mdb` / `reboot_mdb_hard` | Reboot MDB / (hard) | Button | Reboot the MDB. |

**Alarm & navigation**

| Key | Entity | Type |
| --- | --- | --- |
| `alarm_enabled` / `alarm_armed` | Alarm Enabled / Armed | Switch (optimistic) |
| `alarm_start` / `alarm_stop` | Alarm Start / Stop | Button |
| `navigation_set` | Navigation Set to | Text (`lat,lon[,name]`) |
| `navigation_clear` | Navigation Clear | Button |
| `cancel_hibernate` | Cancel Hibernate | Button |

**Configuration**

| Key | Entity | Type | Description |
| --- | --- | --- | --- |
| `cellular_apn` | Cellular APN | Text | Read back on connect; edit sends `set:cellular.apn`. |
| `pm_scheduled_hibernate_enabled` | PM Scheduled Hibernation Enabled | Switch | Read back on connect. |
| `pm_scheduled_hibernate_cron` | PM Scheduled Hibernation Cron | Text | Cron expression. |
| `pm_scheduled_hibernate_duration` | PM Scheduled Hibernation Duration | Text | e.g. `5h30m`. |
| `ota_channel` | OTA channel | Select | `undefined` / `stable` / `testing` / `nightly`. Reflects the running channel. |
| `ota_update_method` | OTA Update Method | Select | `delta` (default, small patch) / `full` (complete `.mender` image). |

**System, time & diagnostics**

| Key | Entity | Type | Description |
| --- | --- | --- | --- |
| `system_time_sync` | System Time sync with ESP | Button | Set the clock to the ESP's SNTP time. |
| `system_time_iso` | System Time Set UTC ISO-8601 | Text | Set the clock from `2026-07-26T18:45:30Z`. |
| `refresh` | A-Refresh Sensor States | Button | Re-poll everything + re-run on-connect queries. |
| `ble_remove_bond` | BLE Remove Bond | Button | Remove the bond (disabled by default). |
| `restart_esp` | Restart ESPHome Device | Button | Reboot the ESP bridge. |

**Extended command**

| Key | Entity | Type | Update | Description |
| --- | --- | --- | --- | --- |
| `command` | Command | Text | — | Arbitrary command (prefilled `cap:list`). |
| `command_last_response` | Command last response | Text Sensor | on command | Correlated reply (255-char limit). |
| `command_response` | Command response | Text Sensor | on notify | Raw notification feed. |

**OTA — firmware update (service `9a590500`)**

| Key | Entity | Type | Description |
| --- | --- | --- | --- |
| `update` | OTA Librescoot Update | Update | One entity for both firmware parts. GitHub release check vs the running versions (MDB `9a59a041`, DBC `status:version:dbc`); shows release notes. |
| `ota_update` | OTA Update | Button | Transfer + install the target release (MDB then DBC), honouring the method select. |
| `ota_version` | OTA Version | Text | Optional specific release tag; empty = latest. Verified against GitHub before any bytes move. |
| `ota_source_url` | OTA Source URL | Text | Where the firmware bytes come from. Default = GitHub; the HA integration points this at its local plain-HTTP relay automatically. |
| `ota_status` | OTA Status | Text Sensor | Decoded OTA status (`Transferring X%`, milestones, errors). |
| `ota_eta` | OTA Upload ETA | Text Sensor | Estimated time remaining, `HH:MM:SS`. |
| `ota_status_request` | OTA Status Request | Button | `STATUS_REQ` (diagnostics). |
| `ota_abort` | OTA Abort | Button | Abort an OTA session (user cancel). |

**How it works.** The component queries the GitHub releases API itself over TLS (only the two
GitHub root CAs are pinned in `github_ca.h`, so the Mozilla bundle is off:
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE: n`) and compares the newest release for the channel shown
by `OTA channel`. Pressing **OTA Update** resolves the release's assets (delta or full per
`OTA Update Method`), then streams them over BLE-OTA to the scooter — **MDB first, then DBC**,
each a separate session distinguished by a component byte in the START message. When all bytes
are acknowledged it sends COMPLETE; the scooter verifies the SHA-256 and installs. Installing
is always allowed — the scooter's firmware decides when to apply it and reboots to finish.

The status/log cadence scales with file size (≈5 s for deltas, 60 s up to 30 MB, 5 min beyond)
so a big transfer doesn't flood Home Assistant.

*Build-time test flag:* set `stage_only: true` in the component config to run the whole
transfer but stop **before** COMPLETE (nothing is installed) — useful for exercising the path.

**Pairing (for the HA integration)**

| Key | Entity | Type | Description |
| --- | --- | --- | --- |
| `passkey_required` | Passkey Required | Binary Sensor | On while the scooter is asking for the pairing passkey. **Add `disabled_by_default: true`** — the Home Assistant integration enables it on demand and turns it into a Repairs pop-up; it isn't meant to be a user-facing entity. |

### [CBB monitoring via I²C addr 0x36 and 0x0B](librescoot-cbb-example.yaml)

The UNU-CBB battery board uses a **MAX17305** fuel gauge. A custom C++ header [librescoot-cbb-max17301.h](librescoot-cbb-max17301.h) handles the chip-specific 16-bit register map and dual-address scheme.

#### Wiring

I2C is wired to the CBB Module Connector (#63):

| ESP Pin | Signal | CBB Connector |
| :--- | :--- | :--- |
| `D2` | SDA | Pin 5 (wire 131) |
| `D1` | SCL | Pin 2 (wire 132) |

#### I2C Addressing

The MAX17305 maps its registers across two 7-bit I2C addresses. The header switches between them automatically based on the register being accessed:

| Address | Used for |
| :--- | :--- |
| `0x36` | Real-time data (registers `0x00`–`0xFF`) |
| `0x0B` | Configuration & NVRAM (registers `0x100`+) |

Values are read as 16-bit little-endian: `(buffer[1] << 8) | buffer[0]`.

#### Sensors

The class polls the fuel gauge every 10 s and exposes the following sensors. Sensors marked ☆ are disabled by default in HA and can be enabled on demand.

| Sensor | Unit | Notes |
| :--- | :--- | :--- |
| Battery Age | % | `FullCapNom / DesignCap` — capacity vs. design spec |
| Battery Cycles | cycles | Quarter-cycle resolution (LSb = 25%) |
| Battery Temperature | °C | 1/256 °C/LSb |
| Battery Remaining Capacity | mAh | RepCap register, scaled via NRSense |
| Battery Full Capacity | mAh | FullCapRep register, tracks aging |
| Battery Serial Number | — | ASCII from NV registers `0xE8–0xEF` |
| Battery Unique ID | — | 64-bit chip UID from registers `0xBC–0xBF` |
| Battery Chip Part | — | Decoded from DevName register `0x21` |

**Disabled by default**

| Sensor | Unit | Notes |
| :--- | :--- | :--- |
| Battery SOC | % | ModelGauge m5 state of charge, aging-compensated |
| Battery VFSOC | % | Voltage-based SOC, diagnostic only |
| Battery Voltage | V | Pack voltage, 78.125 µV/LSb |
| Battery Current | mA | `Raw × 1.5625 µV / NRSense`; negative = discharge |
| Battery Charging | — | True when current > 5 mA |
| Battery Temperature Min | °C | Extreme since last NV save (not lifetime) |
| Battery Temperature Max | °C | Extreme since last NV save (not lifetime) |
| Battery Time to Empty | min | 0 when chip reports 0xFFFF (not computed) |
| Battery Time to Full | min | 0 when chip reports 0xFFFF (not computed) |
| Battery Charge FET Disabled | — | CommStat bit 8 |
| Battery Discharge FET Disabled | — | CommStat bit 9 |
| Battery NV Error | — | CommStat bit 2 |

#### Framework

Built for the **ESP-IDF** framework on ESP32, also compatible with ESP8266 (D1 Mini). Uses direct `I2CBus` transactions rather than the Arduino `Wire` abstraction for stability and performance.

#### Console Output

Matches the [UNU-CBB-Battery](https://github.com/Julinho666/UNU-CBB-Battery) Arduino sketch format for debugging:

`SOC: 99% VFSOC: 51%, Current: -1.50mA, Charging: No, Voltage: 3.72V`

### [DBC Status LED driver via I²C addr 0x30](librescoot-dbc-led-example.yaml)

The UNU Dashboard Connector (DBC) uses an **LP5562** 4-channel programmable LED driver to control the tri-color keycard status indicator. ESPHome's stock component set doesn't include the LP5562, so this example pulls it in from [ssieb's community fork](https://github.com/ssieb/esphome/tree/lp5562).
#### Wiring

The LP5562 lives on the DBC's I²C3 bus, exposed on the Dashboard Connector and routed through the MDB:

| ESP pin | DBC pin | Signal | Route |
| :--- | :--- | :--- | :--- |
| `D2` | 8 | I²C3 SDA | wire 82 → wire 141 → MDB CM1 Pin 5 |
| `D1` | 16 | I²C3 SCL | wire 83 → wire 149 → MDB CM1 Pin 2 |

#### I²C Addressing

The LP5562 sits on the DBC's I²C3 bus at a single 7-bit address:

| Address | Used for |
| :--- | :--- |
| `0x30` | All register access (enable, PWM, current, config) |

Register writes are straightforward 8-bit values — no dual-address tricks or 16-bit endian concerns.

#### Channels

The LP5562 exposes four PWM channels. On the DBC only three are populated with LED dies (the fourth, intended for a white backlight channel, is not wired up):

| Channel | Register | LED color |
| :--- | :--- | :--- |
| 0 | `0x04` | Red |
| 1 | `0x03` | Green |
| 2 | `0x02` | Amber |
| 3 | `0x0E` | *not populated* |

Mixing red + green produces a yellow tone suitable for the "authenticating" state described in the Librescoot keycard protocol. The amber die is left as a distinct third color rather than mixed with the others.

#### ESPHome integration

Each channel is wrapped as a `monochromatic` light with a `pulse` effect named `breathe`, giving a smooth fade-in / fade-out pattern used as the idle indicator. On boot, the green channel starts breathing by default once the chip is confirmed present on the bus.

**Online detection**

A `binary_sensor` of platform `template` named `LED Chip Online` is updated every 2 seconds by a lambda that performs a 1-byte I²C read against `0x30`. When the chip ACKs, the sensor transitions to `true` and the default green breathing effect starts via its `on_press` trigger. When the chip stops responding (cable unplugged, power loss), the sensor transitions to `false` and all lights are turned off in `on_release`, which stops the pulse effect and prevents the LP5562 output component from spamming write errors.

This makes the bus hot-pluggable: the device can boot without the chip connected, and will pick it up the moment it appears.

#### Manual controls

Four template `button` entities are exposed for manual color switching:

*   **LED Red / Green / Yellow** — each one turns off the other two lights and starts the `breathe` effect on the selected channel. All three are guarded by a condition checking `led_chip_online`, so button presses while the chip is unreachable are no-ops instead of generating I²C errors.
*   **LED Off** — unconditionally turns off all three lights.

#### Framework

Built for the **Arduino** framework on ESP8266 (NodeMCU v3, ESP-12E). Uses the stock ESPHome I²C implementation. No custom C++ header needed — the ssieb `lp5562` component handles register-level access internally.

#### Log output

On first detection of the chip and in steady state:

```
[I][binary_sensor:...]: 'LED Chip Online': Sending state ON
[I][main:...]: LED chip (0x30) detected on I2C bus - starting default green breathing
```

If the chip is pulled off the bus at runtime:

```
[W][main:...]: LED chip (0x30) not responding on I2C bus
[I][binary_sensor:...]: 'LED Chip Online': Sending state OFF
```

### [Battery NFC interface via PN532](librescoot-battery-nfc-example.yaml)


Uses [`lsc_battery_nfc`](components/lsc_battery_nfc/)-component

Talks to the scooter's main battery over an NFC reader, reads BMS telemetry, and exposes a high-current-path switch plus a seatbox-state switch to Home Assistant. Mirrors the [librescoot battery-service](https://github.com/librescoot/battery-service) Go FSM at a useful fidelity (heartbeat + state-mismatch recovery + seatbox-open maintenance loop).

* **Telemetry**: voltage, current, level %, state-of-health, BMS state (`asleep`/`idle`/`active`), cycle count, four cell-pack temperatures, capacity, fault code + human-readable description, serial, firmware, manufacturing date, manufacturer.
* **Controls**: High-Current Path switch (drives wake sequence + active-keep heartbeat), Seatbox Closed switch (persistent across reboots, drives the safety override and the OPENED/INSERTED maintenance loop).
* **Diagnostics**: Full NFC Dump, Force Status Refresh.
* **Safety**: foreign tags are detected but writes are inhibited; opening the seatbox forces the high-current path off (unless `keep_active_on_seatbox_open: true`); the high-current switch never restores ON across reboots.

Full details, options, FSM walk-through, and NFC protocol info: **[component README](components/lsc_battery_nfc/README.md)**.

#### Quick start

Pull the component straight from this repo:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/dtill/librescoot-esphome
      ref: main
    components: [lsc_battery_nfc]

i2c:
  sda: D2
  scl: D1
  scan: true
  id: lsc_i2c_bus_1

pn532_i2c:
  id: pn532_rdr
  i2c_id: lsc_i2c_bus_1
  address: 0x24
  update_interval: 60min   # suppress upstream polling; the component does its own

lsc_battery_nfc:
  id: lsc_battery
  pn532_id: pn532_rdr
  update_interval: 2s
  heartbeat_interval: 10s
  keep_active_on_seatbox_open: false

  # Sensors / switches / buttons are all opt-in; declare with a name to expose.
  state:
    name: "Battery State"
  voltage:
    name: "Battery Voltage"
  level:
    name: "Battery Level"
  high_current_path:
    name: "Battery High-Current Path"
  seatbox_closed:
    name: "Seatbox Closed"
```

A complete reference YAML (every sub-entity) is at [`librescoot-battery-nfc-example.yaml`](librescoot-battery-nfc-example.yaml).

#### Hardware

* ESP8266 (NodeMCU) or ESP32 with I²C.
* **HW-147 PN-NFC v3** PN532 board (red, large square antenna) on `0x24`. Smaller round-antenna PN532 breakouts have not been reliable through the battery housing.

| ESP Pin | Signal | PN532 (HW-147) |
| :--- | :--- | :--- |
| `D2` | SDA | SDA |
| `D1` | SCL | SCL |
| `3V3` | VCC | VCC (3.3 V — do not feed 5 V) |
| `GND` | GND | GND |

---

## License

See individual files for license headers; the librescoot project conventions apply.

