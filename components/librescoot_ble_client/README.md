# Librescoot-BLE-Client (`librescoot_ble_client`)

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
An ESPHome external component that connects to the **LibreScoot / unu Scooter Pro** firmware
through the nRF52 BLE chip and controls the most common phone-app functions and a little more —
exposing every scooter characteristic as a Home Assistant entity.

Unlike the pure-YAML `ble_client` configuration, this component **owns the BLE client
itself** — the connection, the passkey pairing flow, every characteristic parser, the
extended-command engine, the BLE link manager and the OTA-status diagnostics all live in
C++. The YAML only *names* the entities it wants.

Protocol reference: <https://reference.librescoot.org/latest/bluetooth/>

---

## Installation

```yaml
external_components:
  - source:
      type: local
      path: my_components          # directory containing librescoot_ble_client/
    components: [librescoot_ble_client]
```

### Required BLE plumbing (stack-level, stays in YAML)

```yaml
esp32_ble:
  io_capability: keyboard_only     # required for the scooter's passkey pairing
esp32_ble_tracker:
```

`io_capability: keyboard_only` is a global BLE-stack setting and must be declared here;
everything else is owned by the component.

---

## Configuration

```yaml
librescoot_ble_client:
  id: librescoot_ble_client_hub
  mac_address: !secret librescoot_nrf_ble_mac_addr
  time_id: sntp_time               # optional, enables the clock-set entities

  status:
    name: "Status"
  odometer:
    name: "Odometer"
  # ... only the entities you name are created ...
```

| Option | Type | Description |
| :--- | :--- | :--- |
| `id` | ID | Component id (used by the `passkey_reply` action). |
| `mac_address` | MAC, **required** | The scooter's nRF BLE MAC address. |
| `time_id` | ID, optional | A `time` source (e.g. `sntp`); required only for **System Time sync with ESP** and **System Time Set UTC ISO-8601**. |

Entity keys are **opt-in**: an entity is instantiated only if its key is present with a
`name`. Each key accepts the usual entity options (`name`, `id`, `icon`, `entity_category`,
`disabled_by_default`, …); sensible `entity_category`/`icon` defaults are baked in, so a
bare `name:` is enough.

---

## Pairing

Identical to the YAML `ble_client` flow — the component performs it internally.

1. Flash the config; add the device to Home Assistant via the ESPHome integration.
2. Power the scooter into parked mode. On first connect the scooter asks for a passkey.
3. In Home Assistant open **Settings → Developer Tools → Actions**, search for
   `passkey_reply`, enter the 6-digit code shown on the scooter dashboard, and
   **Perform Action**.
4. The bond is stored on the ESP; future reconnects are automatic. Flashing new firmware
   to the same chip keeps the bond.

Wire the action to the component in YAML:

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

> Switching to a new ESP chip: press **BLE Remove Bond** (enable it in HA first) *and*
> remove the bond on the scooter, or the new chip cannot re-pair.

---

## Entities

All read entities become *unknown* while the BLE link is down and repopulate on reconnect.
"Update" is the poll interval; `on connect` values are fetched once per connection over the
extended-command channel, `on notify` are pushed by the scooter, `—` marks action/control
entities with no periodic state. Reads are serialized (one GATT read in flight at a time)
and every polled characteristic is read once immediately on connect.

### Telemetry (read)

| Key | Entity | Type | Update | Description |
| :--- | :--- | :--- | :--- | :--- |
| `status` | Status | text_sensor | 5 s | Operating state (`ready-to-drive`, `parked`, `stand-by`, …). |
| `seatbox` | Seatbox | text_sensor | 5 s | Seatbox open/closed. |
| `handlebar_lock` | Handlebar Lock | text_sensor | 5 s | Handlebar lock state. |
| `power_state` | Power State | text_sensor | 5 s | Power-management state. |
| `power_mux` | MDB Power Mux Selected Input | text_sensor | 600 s | Selected power input. |
| `odometer` | Odometer | sensor (km) | 120 s | Total distance (from metres). |
| `battery_1_soc` / `battery_2_soc` | Battery n SoC | sensor (%) | 60 s | Main battery charge (unknown when unplugged). |
| `battery_1_cycles` / `battery_2_cycles` | Battery n Cycles | sensor | 600 s | Charge cycles (unknown when unplugged). |
| `battery_1_state` / `battery_2_state` | Battery n State | text_sensor | 120 s | Battery state string (unknown when unplugged). |
| `battery_1_present` / `battery_2_present` | Battery n Present | binary_sensor | 10 s | Slot occupied. |
| `aux_voltage` | Aux Battery Voltage | sensor (V) | 60 s | 12 V aux battery voltage (u16 LE mV). |
| `aux_level` | Aux Battery Level | sensor (%) | 60 s | Aux battery charge. |
| `aux_charge_status` | Aux Charge Status | text_sensor | 120 s | Aux charge state. |
| `cbb_level` | CBB Battery Level | sensor (%) | 60 s | Connectivity battery charge. |
| `cbb_charge_status` | CBB Charge Status | text_sensor | 120 s | CBB charge state. |
| `cbb_remaining` / `cbb_full` | CBB Remaining / Full Capacity | sensor (Ah) | 600 s | u32 LE µAh → Ah. |
| `cbb_cell` | CBB Cell Voltage | sensor (V) | 120 s | u32 LE µV → V. |
| `navigation_active` | Navigation Active | binary_sensor | 60 s | Navigation running. |
| `ums_status` | UMS Status | binary_sensor | 60 s | USB mass-storage active. |
| `maps_available` | Maps Available | binary_sensor | on connect | Offline maps present. |
| `navigation_available` | Navigation Available | binary_sensor | on connect | Navigation service available. |
| `keycard_count` | Keycard Count | text_sensor | on connect | Registered keycards. |

### Versions

| Key | Entity | Update | Description |
| :--- | :--- | :--- | :--- |
| `sw_mdb` | SW MDB | 1800 s + on connect | i.MX (MDB) version. |
| `sw_nrf` | SW nRF | 1800 s + on connect | nRF52 firmware version. |
| `sw_dbc` | SW DBC | on connect | Dashboard version (`status:version:dbc`). |
| `sw_esp` | SW ESP | static | ESPHome build of this bridge. |

### BLE link

| Key | Entity | Type | Update | Description |
| :--- | :--- | :--- | :--- | :--- |
| `ble_connection` | BLE Connection | binary_sensor | 1 s | Connected to the scooter. |
| `ble_presence` | BLE Presence | binary_sensor | 1 s | Connected, or advertisement seen while scanning. |
| `rssi` | BLE RSSI | sensor (dBm) | 60 s | Link signal strength. |
| `ble_link_mode` | BLE Link Mode | select | — | `disconnect` / `scan` / `auto` / `always`; persisted across reboots. Any OTA in progress pins the link up. |

**Link modes.** `always` keeps the link up and reconnects immediately. `auto` keeps it up
but, on every disconnect, *releases* it for a 20 s grace window so a phone (or any other
central) can win the reconnect race and take the scooter. `scan` keeps only the scanner
running (presence, no connection); `disconnect` releases the radio entirely.

**Connect-on-demand.** In `scan`/`disconnect` (or during an `auto` yield) the link is down,
but triggering any control entity still works: the component transparently brings the link
up, replays the action once connected, waits for the reply (a short window, longer while a
command response is still arriving), then restores the configured mode — releasing the link
again. Optimistic UI state updates immediately; the BLE write follows on connect.

### Lock & controls

| Key | Entity | Type | Description |
| :--- | :--- | :--- | :--- |
| `scooter_lock` | Scooter Lock | lock | Lock = `stand-by`, unlock = `ready-to-drive`; reflected from operating state, defaults LOCKED. |
| `blinker` | Blinker | select | `off` / `left` / `right` / `both`. |
| `usb_mode` | USB Mode | select | `Normal` / `Mass Storage`, reflected from UMS status. |
| `seatbox_open` | Seatbox Open | button | Open the seatbox. |
| `hibernate` / `wakeup` | Hibernate / Wakeup | button | Power management. |
| `reboot_mdb` / `reboot_mdb_hard` | Reboot MDB / (hard) | button | Reboot the MDB. |

### Alarm & navigation

| Key | Entity | Type |
| :--- | :--- | :--- |
| `alarm_enabled` / `alarm_armed` | Alarm Enabled / Armed | switch (optimistic) |
| `alarm_start` / `alarm_stop` | Alarm Start / Stop | button |
| `navigation_set` | Navigation Set to | text (`lat,lon[,name]`) |
| `navigation_clear` | Navigation Clear | button |
| `cancel_hibernate` | Cancel Hibernate | button |

### Configuration

| Key | Entity | Type | Description |
| :--- | :--- | :--- | :--- |
| `cellular_apn` | Cellular APN | text | Read back on connect; edit sends `set:cellular.apn`. |
| `pm_scheduled_hibernate_enabled` | PM Scheduled Hibernation Enabled | switch | Read back on connect. |
| `pm_scheduled_hibernate_cron` | PM Scheduled Hibernation Cron | text | Cron expression. |
| `pm_scheduled_hibernate_duration` | PM Scheduled Hibernation Duration | text | e.g. `5h30m`. |
| `ota_channel` | OTA channel | select | `undefined` / `stable` / `testing` / `nightly`. |

### System, time & diagnostics

| Key | Entity | Type | Description |
| :--- | :--- | :--- | :--- |
| `system_time_sync` | System Time sync with ESP | button | Set the clock to the ESP's SNTP time (needs `time_id`). |
| `system_time_iso` | System Time Set UTC ISO-8601 | text | Set the clock from `2026-07-26T18:45:30Z`. |
| `refresh` | A-Refresh Sensor States | button | Re-poll everything + re-run on-connect queries. |
| `ble_remove_bond` | BLE Remove Bond | button | Remove the bond (set `disabled_by_default: true`). |
| `restart_esp` | Restart ESPHome Device | button | Reboot the ESP bridge. |

### Extended command

The nRF exposes a text command channel (write `9a590401`, notify `9a590402`). The
**Command** input sends an arbitrary string; the reply is collected with a terminator-aware
engine (`:ok`, `:error:`, a standalone line, or a `…:count:<n>` header + *n* lines), 20 s
timeout fallback.

| Key | Entity | Type | Update | Description |
| :--- | :--- | :--- | :--- | :--- |
| `command` | Command | text | — | Arbitrary command (prefilled `cap:list`). |
| `command_last_response` | Command last response | text_sensor | on command | Correlated reply (255-char HA limit). |
| `command_response` | Command response | text_sensor | on notify | Raw notification feed. |

### OTA diagnostics (service `9a590500`)

| Key | Entity | Type | Update | Description |
| :--- | :--- | :--- | :--- | :--- |
| `ota_status` | OTA Status | text_sensor | on notify | Decoded status (`Idle`, `Installing 45%`, `Complete: verified, queued`, …). |
| `ota_status_request` | OTA Status Request | button | — | `STATUS_REQ`. |
| `ota_abort` | OTA Abort | button | — | Abort an OTA session (user cancel). |

> This component covers the **OTA status/diagnostics** only. The full BLE-OTA firmware
> transfer engine (`START`/`DATA`/`COMPLETE`, HTTPS asset fetch) is a separate concern.

### Firmware update check (`update` entities)

| Key | Entity | Type | Description |
| :--- | :--- | :--- | :--- |
| `mdb_update` | OTA MDB Firmware | update | GitHub release check for the MDB component. |
| `dbc_update` | OTA DBC Firmware | update | GitHub release check for the DBC component. |

Each entity queries the GitHub releases API itself (TLS, in a worker task) and compares the
newest release for the **selected channel** against the running version — MDB from
`9a59a041`, DBC from `status:version:dbc`. The channel comes from the `OTA channel` select,
which **reflects the running channel** parsed from the MDB version (`nightly-…` → `nightly`).
`nightly`/`testing` pick the newest tag by timestamp; `stable` uses `/releases/latest`. The
comparison is case-insensitive (the BLE version reports a lowercase `t` in the timestamp, the
tag an uppercase `T`); when up to date the published `latest_version` is set equal to the
installed one so Home Assistant's own string compare agrees. The **release notes** (the GitHub
release `body`) are fetched and shown as `release_summary` in the HA update card. Checks run
when the versions are first known and every ~6 h; HA's "check for updates" triggers one.

**Certificates.** Instead of the ~100 kB Mozilla bundle, the two exact GitHub root CAs are
pinned in `github_ca.h` (USERTrust ECC for `api.github.com`, ISRG Root X1 for the
`objects.githubusercontent.com` asset host). Disable the bundle in the YAML:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_MBEDTLS_CERTIFICATE_BUNDLE: n
```

If GitHub ever moves a host to a different CA, add that root to `github_ca.h`.

### Installing firmware (BLE-OTA transfer)

`perform()` (the update entity's **Install**) resolves the latest release's **delta** assets
(MDB + DBC) from the GitHub API and transfers both to the scooter over BLE, one after another.
Bytes come from the **`OTA Source URL`** (default GitHub; `<base>/<tag>/<asset-name>` layout).

The transfer is **stage-only by default** — it streams the whole DATA phase and stops *before*
COMPLETE, so nothing is installed. Turning on **`OTA Allow Install`** (a switch, off at boot)
makes the next transfer send COMPLETE, at which point the scooter verifies the SHA-256 (sent in
START, taken from the GitHub asset `digest`) against the staged file and queues the install.
Installing firmware onto a road vehicle is a deliberate act — the switch re-arms itself (back
to off) after each install.

**Direct GitHub download needs an S3+PSRAM board.** On the ESP32-classic the RSA-4096 handshake
to `objects.githubusercontent.com` runs out of contiguous heap while BLE is active
(`api.github.com` metadata uses ECC and is fine). Point **`OTA Source URL`** at a local HTTP
mirror to drop TLS from the bulk download — this is faster (~2×) and reliable (0 rewinds), and
is the recommended path on the classic.

Local mirror "[range-server](tools/range_server.py)"for testing / classic-board installs:

```bash
mkdir -p mirror/<tag> && cd mirror
# fetch the real bundles so the SHA verifies (only needed for a real COMPLETE/install):
gh release download <tag> -R librescoot/librescoot -p '*mdb*.delta' -p '*dbc*.delta' -D <tag>
python3 ../tools/range_server.py         # Range-capable; python -m http.server does NOT resume
```

Then set **`OTA Source URL`** to `http://<host>:8000` and press Install. The `ota_test` /
`ota_abort` API actions drive the engine with explicit `url`/`size`/`sha256`/`bundle_id`/
`component` for pure-local testing without GitHub.

---

## Framework

ESP-IDF on ESP32. The component extends `esp32_ble_client::BLEClientBase`, so it registers
a BLE connection slot and participates in scanning like any `ble_client`.

## Example log output

```
[I][librescoot_ble_client]: Scooter connected
[I][librescoot_ble_client]: Negotiated ATT_MTU = 247
[I][librescoot_ble_client]: Service discovery complete. OTA service present
[I][librescoot_ble_client]: TX ext cmd: status:version:dbc
[D][ota]: OTA_STATUS raw: 84 06 00 00
```
