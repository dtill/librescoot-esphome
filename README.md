# librescoot-esphome
ESPHome Components and configurations for UNU Scooter Pro with opensource [Librescoot Firmware](https://github.com/librescoot)

---

## Example .yaml files:

### [nRF-BLE-Client](librescoot-nrf-ble-client-example.yaml)

BLE-Client for Unu-Scooter Pro with Librescoot FW and nRF >v2.0.0-ls.
Uses ESPHome [BLE Client](https://esphome.io/components/ble_client/) and exposes available characteristics as sensors.
More info about the provided characteristics at [LibreScoot Tech Reference - Bluetooth Interface Documentation](https://reference.librescoot.org/bluetooth/)

#### Pairing ESP32 with Scooter Pro nRF via ESPHome BLE

An ESP32 board running ESPHome connects to the Scooter Pro's nRF BLE chip over Bluetooth. The connection is secured with a one-time 6-digit passkey that gets generated during pairing.

Setup:

1. Flash the ESPHome [nRF-BLE-Client](librescoot-nrf-ble-client-example.yaml) onto your ESP32. Set all secrets in your ESPHome-Device-Builders `secrets.yaml` especially your scooter's BLE MAC address.
2. Add the ESP32 device to Home Assistant via the ESPHome integration.

Pairing:

1. Turn on the scooter into parked mode.
When the ESP32 first connects to the scooter, it will ask for a passkey. To enter it:

2. Open Home Assistant and go to **Settings → Developer Tools → Actions** (or go directly to `/config/developer-tools/action` in your browser).
3. In the action search field, type **"librescoot"** — you'll see the `passkey_reply` action appear.
4. Select it. A field for a 6-digit code will show up.
5. Check scooters dashboard (DBC) for the passkey the scooter is expecting, enter it in HA, and hit **Perform Action**.
6. Once accepted, the connection is established. The bond is saved — future reconnects happen automatically.



### [CBB monitoring via i2c](librescoot-cbb-example.yaml)
The UNU-CBB battery board features a **MAX17301** fuel gauge. Integrating this chip into ESPHome requires a custom C++ header [librescoot-cbb-max17301.h](librescoot-cbb-max17301.h) because the chip uses a specific 16-bit register map and a dual-addressing scheme that standard components (like the MAX17043) do not support.

#### I2C Addressing & Memory Mapping
The MAX17301 organizes its registers into two virtual "pages," each represented by a different 7-bit I2C address. The header logic automatically handles switching between these addresses based on the requested register:

| Page | 7-bit Addr (ESPHome) | 8-bit Addr (Arduino) | Function |
| :--- | :--- | :--- | :--- |
| **Primary** | `0x36` | `0x6C` | Real-time data (Voltage, SOC, Current) |
| **Extended**| `0x0B` | `0x16` | Configuration & NVRAM (NRSense) |

The ESPHome implementation uses the 7-bit address. If a register address is `> 0xFF` (e.g., NRSense at `0x1CF`), the code automatically communicates with address `0x0B`.

#### Framework Specifics: ESP32 ESP-IDF vs. ESP8266
The implementation differs depending on the hardware framework used in your YAML:

*   **ESP32 (ESP-IDF Framework):** 
    This project uses the native **ESP-IDF** framework (non-Arduino). In this mode, we interact directly with the ESPHome `I2CBus` object. Instead of using high-level Arduino-style `read_register` helpers, we perform manual bus transactions:
    1.  `write()`: Sends the 8-bit register address to the bus.
    2.  `read()`: Fetches 2 bytes from the chip.
    3.  **Endianness**: The MAX17301 uses Little Endian. Bytes are reassembled manually: `(buffer[1] << 8) | buffer[0]`.

*   **ESP8266 (Arduino Framework):**
    While possible, the Arduino framework uses the `Wire` library abstraction. The `I2CBus` calls would differ slightly, but for performance and stability on the ESP32, the ESP-IDF framework is preferred.

#### Data Conversion & Accuracy
The system translates raw hexadecimal register values into human-readable units:

*   **SOC (State of Charge):** Derived from the ModelGauge m5 algorithm. It is highly stable and accounts for battery aging.
*   **VFSOC (Voltage-based SOC):** A theoretical charge level based strictly on the current voltage. Useful for diagnostics but prone to "voltage sag" under load.
*   **Voltage:** Calculated with a resolution of `78.125 µV` per LSB.
*   **Current:** Calculated using the internal `NRSense` calibration value. The formula used is:  
    `Current (mA) = (Raw_Value * 1.5625 µV) / NRSense_Value`.

#### ESPHome Software Architecture
The integration is split into three parts to ensure a clean YAML structure:

1.  **Header File (`.h`):** Contains the `MAX17301` class logic, handling all I2C communication and raw data processing.
2.  **Polling Interval:** An `interval: 10s` block in the YAML triggers the `update()` method of the C++ class. This centralizes I2C traffic to one burst every 10 seconds.
3.  **Template Sensors:** Standard ESPHome sensors fetch the calculated values from the C++ class. This allows the use of `device_class: battery`, `voltage`, and `current`, which enables dynamic MDI icons and native behavior in Home Assistant.

#### Console Output Example
The implementation mirrors the [UNU-CBB-Battery](https://github.com/Julinho666/UNU-CBB-Battery) Arduino-Scetch serial output for debugging purposes:

`SOC: 99% VFSOC: 51%, Current: -1.50mA, Charging: No, Voltage: 3.72V`

### DBC monitoring via i2c and UART
tba


