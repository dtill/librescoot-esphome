# lsc_battery_nfc — ESPHome external component

Drop-in ESPHome component for talking to a **unu Scooter Pro main battery** through a **PN532 NFC reader** on I²C. Reads BMS telemetry (voltage, level, health, temperatures, faults, serial, …) and exposes a high-current-path switch plus a seatbox-state switch that mirror the [librescoot battery-service](https://github.com/librescoot/battery-service) Go FSM at a useful fidelity.

Foreign NFC tags (door fobs, transit cards, etc.) are detected but commands are inhibited — only a tag that identifies as an LSC battery can be controlled.

---

## Quick start

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/danieltill/librescoot-esphome
      ref: main
    components: [lsc_battery_nfc]
    # The component lives in a subfolder of the repo, so point ESPHome at it:
    path: esphome-test-env/esphome_configs/my_components

i2c:
  sda: D2
  scl: D1
  scan: true
  id: lsc_i2c_bus_1

# Stock PN532 component — used only for chip init (SAMConfig). Set its
# update_interval high so it doesn't poll for tags; lsc_battery_nfc drives
# polling itself.
pn532_i2c:
  id: pn532_rdr
  i2c_id: lsc_i2c_bus_1
  address: 0x24
  update_interval: 60min

lsc_battery_nfc:
  id: lsc_battery
  pn532_id: pn532_rdr
  update_interval: 2s
  heartbeat_interval: 10s
  keep_active_on_seatbox_open: false

  # All sub-entities are opt-in: declare the key with a name to expose, omit to skip.
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

A full example with every sub-entity is in [`../../librescoot-batt-nfc-cffbde-phase2.yaml`](../../librescoot-batt-nfc-cffbde-phase2.yaml).

---

## Features

* **Auto-detects LSC batteries** — validates by reading the BMS state field at NTAG page `0xC4`; refuses to write commands to any other tag.
* **Live telemetry** — voltage (mV), current (mA), level (%), state-of-health (%), state (`asleep` / `idle` / `active`), cycle count, four temperature probes, capacity, serial, firmware, manufacturing date, manufacturer, fault code with human-readable description.
* **High-Current Path switch** — turning ON queues the wake sequence (`INSERTED → SEATBOX_CLOSED → ON`) and starts the active-keep heartbeat. Turning OFF stops the keep-alive so the BMS times back out to idle. Safety: restore mode is `ALWAYS_OFF` so a reboot never comes back with the high-current path on.
* **Seatbox Closed switch** — persistent across reboots. Closing drives the normal heartbeat (`CLOSED + ON/OFF + HEARTBEAT_SCOOTER`). Opening triggers a dedicated maintenance loop (`OFF` until BMS confirms idle, then `OPENED ↔ INSERTED` every ~800 ms) and forces the high-current path off as a safety override — unless `keep_active_on_seatbox_open: true`.
* **Full NFC Dump button** — diagnostic; walks the entire NTAG user-memory area (`0x04..0xE0`) in TagInfo-style hex+ASCII with fresh-inlist retry on failure so the dump completes even when the PN532 momentarily glitches.
* **State-mismatch recovery** — re-sends `INSERTED` only on persistent disagreement (≥ 2 consecutive heartbeats) to avoid log spam from one-tick races right after a user toggle.
* **De-duplicated state pushes** — `publish_state()` is only called when a value actually changed, so the per-poll `[S]`-level log stays quiet.

---

## Requirements

* ESPHome ≥ 2026.4.x (tested on `2026.4.5`).
* An I²C-capable ESP board. Tested on ESP8266 NodeMCU; ESP32 should work without changes (the PN532 part is hardware-agnostic).
* A **PN532 NFC board**. The **HW-147 PN-NFC v3** (red, large square antenna) is strongly recommended — the battery's NFC coil sits behind the pack housing and needs a stronger antenna than the small round-antenna breakouts can reliably provide.
* The stock ESPHome `pn532_i2c` component (auto-loaded as a dependency).

### Wiring

| ESP Pin | Signal | PN532 (HW-147) |
| :--- | :--- | :--- |
| `D2` | SDA | SDA |
| `D1` | SCL | SCL |
| `3V3` | VCC | VCC (3.3 V — do not feed 5 V) |
| `GND` | GND | GND |

The PN532 chip is hardwired to I²C address **`0x24`**. Make sure the board's mode-select switches/jumpers are set for I²C; if a bus scan shows different addresses (`0x21` + `0x41` without `0x24`) the chip is in SPI/UART mode or SDA/SCL are crossed.

---

## Configuration reference

### Top-level options

| Key | Default | Description |
| :--- | :--- | :--- |
| `id` | auto | Component ID for use in lambdas / automations. |
| `pn532_id` | **required** | ID of a `pn532_i2c:` block. The PN532 chip is used for SAMConfig init only — this component drives polling on its own. |
| `update_interval` | `2s` | Base polling interval. Auto-switches to **`800 ms`** while the seatbox-open maintenance loop runs, restored on exit. |
| `heartbeat_interval` | `10s` | Minimum interval between `CLOSED + ON/OFF + HEARTBEAT_SCOOTER` write cycles when the seatbox is closed. |
| `keep_active_on_seatbox_open` | `false` | If `true`, opening the seatbox does NOT force the high-current path off — heartbeat continues with `OPENED` instead of `CLOSED`. Matches the Go FSM's `ShouldKeepActiveOnSeatboxOpen()` semantics. |

### Sub-entities

Every sensor / switch / button is optional. Declare the key under `lsc_battery_nfc:` with a `name:` to expose it; omit it to skip. ESPHome's standard sub-schema options (`id:`, `entity_category:`, `icon:`, `filters:` …) all work.

#### Primary sensors

| Key | Type | Unit | Notes |
| :--- | :--- | :--- | :--- |
| `state` | text_sensor | text | `asleep` / `idle` / `active` |
| `voltage` | sensor | mV | Pack voltage |
| `current` | sensor | mA | Negative = discharge |
| `level` | sensor | % | Charge percentage |
| `health` | sensor | % | State of health |
| `cycle_count` | sensor | — | `total_increasing` for HA statistics |

#### Diagnostic sensors

Default `entity_category: diagnostic` so HA hides them from the main dashboard.

| Key | Type | Unit | Notes |
| :--- | :--- | :--- | :--- |
| `temp_0` … `temp_3` | sensor | °C | Four cell-pack temperature probes |
| `remaining_capacity` | sensor | mAh | |
| `full_capacity` | sensor | mAh | |
| `fault_code` | sensor | — | Raw uint16 |
| `fault_description` | text_sensor | text | Human-readable mapping of `fault_code` |
| `low_soc` | binary_sensor | — | |
| `serial` | text_sensor | text | 16-char ASCII serial |
| `firmware` | text_sensor | text | `"major.minor"` |
| `manufacturing_date` | text_sensor | text | `YYYY-MM-DD` |
| `manufacturer` | text_sensor | text | Factory code, e.g. `TWS`; read once per battery UID |
| `tag_present` | binary_sensor | — | True while any NFC tag is in the reader's field |
| `battery_detected` | binary_sensor | — | True only when the present tag is a validated LSC battery |

#### Controls

| Key | Type | Notes |
| :--- | :--- | :--- |
| `high_current_path` | switch | Restore mode `ALWAYS_OFF` (safety). ON queues wake sequence + active-keep heartbeat; OFF stops keep-alive. |
| `seatbox_closed` | switch | Restore mode `RESTORE_DEFAULT_ON` (persists across reboots; defaults to `closed` on fresh flash). Entity category `config`. |
| `full_dump` | button | Diagnostic; logs the entire NTAG user memory (~30 s). |
| `manual_refresh` | button | Diagnostic; forces tag re-validation and an immediate heartbeat on the next update. |

---

## How it works — FSM brief

Mirrors [`battery-service/battery/fsm/state_machine.go`](https://github.com/librescoot/battery-service/blob/main/battery/fsm/state_machine.go) at a polling-loop fidelity. Three paths:

### Heartbeat (seatbox closed)

Every `heartbeat_interval` (default 10 s):

1. `CMD_SEATBOX_CLOSED` (`0x4D4B4D4B`)
2. `CMD_ON` (`0x50505050`) if user wants high-current path on, otherwise `CMD_OFF` (`0xCAFEF00D`)
3. `CMD_HEARTBEAT_SCOOTER` (`0x534E4A41`) — only when effective state is ON. Empirically required to keep the BMS in `active` between 10 s heartbeats; without it the BMS decays back to idle ~5 s after `CMD_ON`.

If the BMS state read disagrees with the user's intent for 2+ consecutive heartbeats, `CMD_INSERTED` (`0x44414E41`) is re-sent as recovery.

### Wake-up (fresh tag arrival, seatbox open, battery not active)

One-shot sequence: `CMD_SEATBOX_OPENED` → 2 s delay (3 s if the battery was asleep) → `CMD_INSERTED`. After this the FSM falls through to either the heartbeat or the seatbox-open loop depending on the seatbox state.

### Seatbox-open maintenance loop (seatbox open, `!keep_active_on_seatbox_open`)

Runs **instead of** the heartbeat. Polling rate is dropped from 2 s to **800 ms** so the BMS sees a continuous OPENED/INSERTED conversation (this is what keeps the battery's status LED bar lit instead of dimming between commands).

1. **Deactivate**: send `CMD_OFF` every tick until a subsequent status read confirms `idle`. Mirrors Go's `StateCondOff` loop.
2. **First OPENED**: send `CMD_SEATBOX_OPENED` (Go uses a 2 s `justOpened` delay here; our 800 ms tick is the practical equivalent given polling overhead).
3. **Loop**: alternate `CMD_INSERTED` ↔ `CMD_SEATBOX_OPENED` once per tick.

The user's `enabled` (high-current intent) flag is **preserved** across this — closing the seatbox automatically resumes the active heartbeat without the user having to re-toggle the switch.

### Foreign tag handling

A tag whose STATUS1 state word is not one of the three known LSC battery state words (`ASLEEP`/`IDLE`/`ACTIVE`) is marked `Battery Detected = false` and all command writes are inhibited. The component keeps polling so the user sees the tag come and go, but nothing is written.

---

## NFC protocol details

The battery exposes an NT3H2111 NTAG surface over NFC Type-4. Telemetry sits at fixed NTAG pages (16 bytes per page block):

| Page | Block | Contents |
| :--- | :--- | :--- |
| `0x04` | MFR | Factory metadata (manufacturer code, build date, serial …) |
| `0xC0` | STATUS0 | voltage / current / FW / capacities / fault code / temp0-1 / SOH / low-SOC |
| `0xC4` | STATUS1 | 4-byte BMS state word + serial part 1 |
| `0xC8` | STATUS2 | serial part 2 / mfg date / cycle count / temp2-3 |
| `0xCC` | COMMAND | 4-byte little-endian command (write-only) |

### BMS state words (read at STATUS1 bytes `[0..3]`, little-endian uint32)

| Value | Meaning |
| :--- | :--- |
| `0xA4983474` | `asleep` |
| `0xB9164828` | `idle` |
| `0xC6583518` | `active` (high-current path enabled) |

### Commands (written 4-byte little-endian at PAGE_COMMAND)

| Command | Value | Purpose |
| :--- | :--- | :--- |
| `CMD_ON` | `0x50505050` | Enable high-current path |
| `CMD_OFF` | `0xCAFEF00D` | Disable high-current path |
| `CMD_INSERTED` | `0x44414E41` | "I'm in the scooter" notification (state-correction poke) |
| `CMD_SEATBOX_OPENED` | `0x48525259` | Seatbox just opened |
| `CMD_SEATBOX_CLOSED` | `0x4D4B4D4B` | Seatbox just closed |
| `CMD_HEARTBEAT_SCOOTER` | `0x534E4A41` | Active-keep ping (defined in the Go FSM but unused there — empirically required for our polling cadence) |

PN532 access uses the standard C++ "Rob" idiom to reach upstream `pn532::PN532`'s protected `read_mifare_ultralight_bytes_` / `write_mifare_ultralight_page_` / inlist / RF-off primitives without modifying the upstream source. See the namespace `rob` block in [`lsc_battery_nfc.cpp`](lsc_battery_nfc.cpp).

---

## Log output examples

Steady state, seatbox closed, high-current on (one summary line per 2 s poll, heartbeat every 10 s):

```
[D][lsc_battery_nfc]: BATTERY STATE=active V=57.892V I=0mA LVL=100% RCAP=33000mAh SOH=100% CYCLES=80 SN=T-UNU22103075007 FAULT=OK(0)
[D][lsc_battery_nfc]: heartbeat cycle (seatbox=closed, enabled=on)
[D][lsc_battery_nfc]:   cmd 0x4D4B4D4B (SEATBOX_CLOSED) -> OK
[I][lsc_battery_nfc]:   cmd 0x50505050 (ON) -> OK
[D][lsc_battery_nfc]:   cmd 0x534E4A41 (HEARTBEAT_SCOOTER) -> OK
```

User presses High-Current Path → ON while battery is idle:

```
[I][lsc_battery_nfc]: user request: high-current ON
[I][lsc_battery_nfc]: queued wake-seq: INSERTED → SEATBOX_CLOSED → ON
[I][lsc_battery_nfc]:   cmd 0x44414E41 (INSERTED) -> OK
[I][lsc_battery_nfc]:   cmd 0x4D4B4D4B (SEATBOX_CLOSED) -> OK
[I][lsc_battery_nfc]:   cmd 0x50505050 (ON) -> OK
```

User toggles Seatbox Closed → Open while battery is active (safety override forces deactivation, then maintenance loop):

```
[I][lsc_battery_nfc]: user request: seatbox OPEN
[I][lsc_battery_nfc]:   entering seatbox-open maintenance loop
[I][lsc_battery_nfc]: seatbox-open init: cmd 0xCAFEF00D (OFF) -> OK (waiting for idle)
[I][lsc_battery_nfc]: seatbox-open: battery confirmed idle, advancing to OPENED/INSERTED loop
[I][lsc_battery_nfc]: seatbox-open: cmd 0x48525259 (SEATBOX_OPENED, first) -> OK
[D][lsc_battery_nfc]: seatbox-open loop: cmd 0x44414E41 (INSERTED) -> OK
[D][lsc_battery_nfc]: seatbox-open loop: cmd 0x48525259 (SEATBOX_OPENED) -> OK
…
```

Seatbox Closed → Closed (loop exits, heartbeat resumes; the `enabled` flag was preserved so the battery wakes back up on the next cycle):

```
[I][lsc_battery_nfc]: user request: seatbox CLOSED
[I][lsc_battery_nfc]:   exiting seatbox-open loop, resuming heartbeat
[D][lsc_battery_nfc]: heartbeat cycle (seatbox=closed, enabled=on)
[D][lsc_battery_nfc]:   cmd 0x4D4B4D4B (SEATBOX_CLOSED) -> OK
[I][lsc_battery_nfc]:   cmd 0x50505050 (ON) -> OK
```

Foreign tag (correctly rejected):

```
[W][lsc_battery_nfc]: Tag UID=04-1B-6F-C2-7C-67-80 is not an LSC battery — writes inhibited
```

Full NFC dump excerpt:

```
[I][lsc_battery_nfc.full_dump]: heartbeat + status reads suppressed during dump
[I][lsc_battery_nfc.full_dump]: === NFC dump  UID=04-34-5B-1A-0F-6A-80  pages 0x04..0xE0 ===
[I][lsc_battery_nfc.full_dump]: [0x04] 54 57 53 20  20 20 20 20  20 20 20 20  20 20 20 20  |TWS             |
[I][lsc_battery_nfc.full_dump]: [0x0C] 54 2D 55 4E  55 32 32 31  30 33 30 37  35 30 30 37  |T-UNU22103075007|
[I][lsc_battery_nfc.full_dump]: [0xC0] 24 E2 00 00  02 1C E8 80  E8 80 00 00  19 19 64 00  |$.............d.|
[I][lsc_battery_nfc.full_dump]: === END DUMP ===
```

### Suggested logger config

Tuning to keep the steady-state output readable:

```yaml
logger:
  level: DEBUG
  baud_rate: 0   # disable serial logging, save RAM on ESP8266
  logs:
    i2c.arduino: NONE
    pn532_i2c: NONE
    pn532.mifare_ultralight: NONE
    pn532: INFO
    sensor: WARN
    text_sensor: WARN
    switch: WARN
    binary_sensor: WARN
```

---

## Troubleshooting

| Symptom | Likely cause / fix |
| :--- | :--- |
| Bus scan shows `0x21` + `0x41` but not `0x24` | PN532 is in SPI/UART mode, or SDA/SCL crossed. Check mode-select jumpers; verify wiring. |
| Battery shows `Battery Detected: false` despite tag in field | Tag is being read but its state word isn't a known LSC value. Trigger a Full NFC Dump and check page `0xC4` bytes 0-3 against the BMS state table above. |
| `Battery State` flickers `active ↔ idle` between heartbeats | Some other writer is touching the BMS, or `HEARTBEAT_SCOOTER` writes are failing. Check the heartbeat log lines for `-> FAIL`. |
| LED bar on battery dims during seatbox-open | Polling rate didn't drop to 800 ms. Confirm the `entering seatbox-open maintenance loop` line in the log; check that `keep_active_on_seatbox_open` is `false`. |
| OTA upload fails after switching seatbox to open | The 800 ms polling rate uses more I²C bandwidth and slows down WiFi event handling. Close the seatbox switch before OTA. |
