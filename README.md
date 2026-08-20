# librescoot-esphome
ESPHome Components and configurations for "UNU Scooter Pro" with opensource [Librescoot Firmware](https://github.com/librescoot)


- [MDB nRF-BLE-Client](#mdb-nrf-ble-client)
    - [Quick start](#quick-start)
    - [What you get](#what-you-get)
    - [Which ESP32 — and do I need the Home Assistant integration?](#which-esp32--and-do-i-need-the-home-assistant-integration)
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

### [MDB nRF-BLE-Client](librescoot-ble-client-example.yaml)

**Component edition.** Bluetooth interface for the unu Scooter Pro (Librescoot FW, nRF >v2.0.0-ls),
built as the [`librescoot_ble_client`](components/librescoot_ble_client/) external component: the
BLE client, passkey pairing, every characteristic parser, the extended-command engine, the link
manager and the **BLE-OTA firmware transfer** all live in C++. **The YAML only names the entities it
wants.** Characteristic reference:
[LibreScoot Tech Reference — Bluetooth Interface](https://reference.librescoot.org/latest/bluetooth/)

```yaml
external_components:
  - source: github://dtill/librescoot-esphome
    components: [librescoot_ble_client]

# BLE stack plumbing (shared, stack-level; stays in the YAML)
esp32_ble:
  io_capability: keyboard_only   # required for the scooter's passkey pairing
esp32_ble_tracker:

librescoot_ble_client:
  id: librescoot_ble_client_hub
  mac_address: !secret librescoot_nrf_ble_mac_addr

  status:       {name: "Status"}
  odometer:     {name: "Odometer"}
  scooter_lock: {name: "Scooter Lock"}
  # ... only the entities you name are created ...
```

Every option and every entity key is documented in the
**[component README](components/librescoot_ble_client/README.md)** — start there for anything beyond
the walkthrough below.

#### Quick start

**1. Flash.** Put your Wi-Fi, an API key, an OTA password and the scooter's Bluetooth MAC into
`secrets.yaml`, then flash [`librescoot-ble-client-example.yaml`](librescoot-ble-client-example.yaml)
once over USB (`esphome run …`). After that it updates itself over Wi-Fi. Add the ESP to Home
Assistant — it is discovered automatically. **Control and all sensors work from here on.**

**2. Pair.** The scooter must be **on and unlocked**, and you need to see its dashboard.

1. Press **BLE Pairing Start** — the only thing that starts pairing.
2. Read the **6-digit code** off the dashboard (10–15 s until it appears).
3. Type it into **BLE Pairing Passkey**.
4. Press **BLE Pairing Send Passkey**.

**The code expires ~30 s after it appears**, so do 2–4 in one go; if it expires the scooter shows a
new one. **BLE Pairing Required** turns *OK* only once the bond really exists. From then on the ESP
reconnects on its own.

**3. Update the scooter's firmware.** Pick a channel (**OTA channel**) and a method
(**OTA Update Method**: `full` always applies, `delta` is a small patch against the immediately
preceding release). When a release is available, **OTA MDB Update** / **OTA DBC Update** offer it —
press **Install**. The scooter's two boards update one at a time and it reboots after each; the
component waits that out. Nothing is ever installed on its own.

#### What you get

Lock/unlock, seatbox, blinkers, alarm, USB/UMS mode, navigation and power-management controls;
battery, CBB and aux-battery telemetry, odometer, operating state, versions and RSSI; the freeform
extended-command channel; and firmware updates for both scooter boards straight from GitHub
releases, with resume, self-healing transfers and progress/throughput sensors.

#### Which ESP32 — and do I need the Home Assistant integration?

| | ESP32 classic | ESP32-S3 (PSRAM) |
| :--- | :---: | :---: |
| Control, all sensors, pairing | ✅ | ✅ |
| Check for firmware updates | ✅ | ✅ |
| Firmware download **via Home Assistant** | ✅ (the only way) | ✅ (selectable) |
| Firmware download **direct from GitHub** | ❌ (not enough memory) | ✅ (default) |
| Home Assistant companion integration needed for updates | **Yes** | No — but supported |

The classic cannot hold the GitHub-CDN TLS session next to the Bluetooth stack, so the companion
integration downloads the release and serves the bytes over the local network. On an S3 the byte
source is a runtime choice (**OTA Source**), so it can use the relay too. **Everything that is not a
firmware download works without the integration on both boards.**

> **On an ESP32-S3, add `CONFIG_BT_BLE_50_FEATURES_SUPPORTED: n`** to the board's
> `framework: sdkconfig_options:`. The S3's Bluetooth-5 controller otherwise issues an *Extended*
> Create Connection that the scooter rejects during the scan→connect switch, looping on status-133
> connect errors. Forcing the legacy BLE-4.2 connect fixes it; the classic never needs this (MTU 247
> and Data Length Extension are unaffected).

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

