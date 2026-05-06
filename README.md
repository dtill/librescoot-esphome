# librescoot-esphome
ESPHome Components and configurations for UNU Scooter Pro with opensource [Librescoot Firmware](https://github.com/librescoot)


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

---

## Example .yaml files:

### [MDB nRF-BLE-Client](librescoot-nrf-ble-client-example.yaml)

BLE-Client for Unu-Scooter Pro with Librescoot FW and nRF >v2.0.0-ls.
Uses ESPHome [BLE Client](https://esphome.io/components/ble_client/) and exposes available characteristics as sensors.
More info about the provided characteristics at [LibreScoot Tech Reference - Bluetooth Interface Documentation](https://reference.librescoot.org/bluetooth/)

#### Pairing ESP32 with Scooter Pro nRF via ESPHome BLE

An ESP32 board running ESPHome connects to the Scooter Pro's nRF BLE chip over Bluetooth. The connection is secured with a one-time 6-digit passkey that gets generated during pairing.

Setup:

1. Flash the ESPHome [nRF-BLE-Client](librescoot-nrf-ble-client-example.yaml) onto your ESP32. Set all secrets in your ESPHome-Device-Builders `secrets.yaml` especially your scooter's BLE MAC address.
2. Add the ESP32 device to Home Assistant via the ESPHome integration.

Pairing:

1. Turn on the scooter into parked mode. When the ESP32 first connects to the scooter, it will ask for a passkey. To enter it:
2. Open Home Assistant and go to **Settings → Developer Tools → Actions** (or go directly to `/config/developer-tools/action` in your browser).
3. In the action search field, type **"librescoot"** — you'll see the `passkey_reply` action appear.
4. Select it. A field for a 6-digit code will show up.
5. Check scooters dashboard (DBC) for the passkey the scooter is expecting, enter it in HA, and hit **Perform Action**.
6. Once accepted, the connection is established. The bond is saved — future reconnects happen automatically.

#### Using the Extended Command

The `Extended Command` text input allows arbitrary commands to be sent to the nRF. After sending, responses are collected for 20 seconds and concatenated into the `Command Response Log` sensor.

Example — set the cellular APN:

- **Command:** `config:apn <value>` (e.g. `config:apn web.vodafone.de`)
- **Expected reply (in `Command Response Log`):** `config:ok`

#### Exposed Entities

| Entity                   | Type           | Unit | Description                                                                 |
| ------------------------ | -------------- | ---- | --------------------------------------------------------------------------- |
| Status                   | Text Sensor    | —    | Vehicle state (e.g. parked, ready).                                         |
| Seatbox                  | Text Sensor    | —    | Open/closed state of the seatbox.                                           |
| Handlebar Lock           | Text Sensor    | —    | Lock/unlock state of the handlebar.                                         |
| Power State              | Text Sensor    | —    | Current power-management state.                                             |
| Power Mux Selected Input | Text Sensor    | —    | Currently selected power input source.                                      |
| Odometer                 | Sensor         | km   | Total distance travelled (converted from meters).                           |
| Battery 1 SoC            | Sensor         | %    | State of charge, main battery slot 1.                                       |
| Battery 1 Cycles         | Sensor         | —    | Charge-cycle count, main battery slot 1.                                    |
| Battery 1 State          | Text Sensor    | —    | State string for main battery slot 1.                                       |
| Battery 1 Present        | Binary Sensor  | —    | Whether main battery slot 1 is occupied.                                    |
| Battery 2 SoC            | Sensor         | %    | State of charge, main battery slot 2.                                       |
| Battery 2 Cycles         | Sensor         | —    | Charge-cycle count, main battery slot 2.                                    |
| Battery 2 State          | Text Sensor    | —    | State string for main battery slot 2.                                       |
| Battery 2 Present        | Binary Sensor  | —    | Whether main battery slot 2 is occupied.                                    |
| Aux Battery Voltage      | Sensor         | V    | Voltage of the auxiliary 12 V battery.                                      |
| Aux Battery Level        | Sensor         | %    | State of charge of the auxiliary battery.                                   |
| Aux Charge Status        | Text Sensor    | —    | Charge state of the auxiliary battery.                                      |
| CBB Battery Level        | Sensor         | %    | State of charge of the connectivity battery.                                |
| CBB Charge Status        | Text Sensor    | —    | Charge state of the connectivity battery.                                   |
| iMX Software Version     | Text Sensor    | —    | Software version of the i.MX (MDB).                                         |
| nRF Version              | Text Sensor    | —    | Firmware version of the nRF52 chip.                                         |
| Navigation Active        | Binary Sensor  | —    | Whether navigation is currently active.                                     |
| UMS Status               | Binary Sensor  | —    | USB Mass Storage active state.                                              |
| BLE Connection           | Binary Sensor  | —    | Connectivity status of the BLE link.                                        |
| RSSI                     | Sensor         | dBm  | Signal strength of the BLE link.                                            |
| Blinker                  | Select         | —    | Set turn signal: `off`, `left`, `right`, `both`.                            |
| Seatbox Open             | Button         | —    | Unlock/open the seatbox.                                                    |
| Hibernate                | Button         | —    | Put the scooter into hibernation.                                           |
| Wakeup                   | Button         | —    | Wake the scooter from hibernation.                                          |
| Reboot                   | Button         | —    | Soft-reboot the MDB.                                                        |
| Hard Reboot              | Button         | —    | Hard-reboot the MDB.                                                        |
| BLE Remove Bond          | Button         | —    | Remove the existing BLE bond (disabled by default; use only for re-pairing).|
| Extended Command         | Text Input     | —    | Send an arbitrary command string to the nRF (see above).                    |
| Last Command Response    | Text Sensor    | —    | Most recent raw response chunk from an extended command.                    |
| Command Response Log     | Text Sensor    | —    | Concatenated response collected during the 20 s window after a command.     |

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


