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



### CBB monitoring via i2c
tba
### DBC monitoring via i2c and UART
tba


