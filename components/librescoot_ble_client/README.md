# Librescoot-BLE-Client

An ESPHome component that connects an ESP32 to your **unu Scooter Pro (LibreScoot firmware)**
over Bluetooth and puts the scooter into Home Assistant: lock, blinkers, seatbox, alarm,
batteries, odometer — and **firmware updates for the scooter**, over Bluetooth, from the
LibreScoot GitHub releases.

**You need:** ESPHome 2026.9 or newer · an ESP32 or ESP32-S3 · a scooter on LibreScoot firmware ·
for firmware updates on a plain ESP32 the
[Home Assistant integration](../../homeassistant-integration/README.md).

## Contents

- [Quick start](#quick-start) — [Flash](#1-flash) · [Pair](#2-pair) · [Done](#3-done)
- [Configuration](#configuration)
- [Pairing](#pairing)
- [Controls](#controls)
- [Sensors](#sensors)
- [Bluetooth link](#bluetooth-link)
- [Alarm](#alarm)
- [Dashboard power](#dashboard-power)
- [OTA Updates](#ota-updates) — [Install the next release](#install-the-next-release) ·
  [Update unattended](#update-unattended-ota-auto-update) · [A specific version](#a-specific-version) ·
  [Delta or full](#delta-or-full) · [Chained delta updates](#chained-delta-updates) ·
  [Direct GitHub download (ESP32-S3)](#direct-github-download-esp32-s3) · [Entities](#ota-entities) ·
  [When something goes wrong](#when-something-goes-wrong)
- [Command](#command)
- [Under the hood](#under-the-hood)

---

## Quick start

### 1. Flash

1. Copy [`librescoot-ble-client-example.yaml`](../../librescoot-ble-client-example.yaml).
2. Put your Wi-Fi, an API key and the scooter's Bluetooth MAC into `secrets.yaml`.
3. `esphome run librescoot-ble-client-example.yaml` — once over USB. Later updates go over Wi-Fi.
4. Home Assistant finds the ESP by itself: *Settings → Devices & Services → ESPHome → Add*.

### 2. Pair

Switch the scooter **on and unlock it**. You need to see its dashboard.

1. Press **BLE Pairing Start**.
2. After 10–15 s the dashboard shows a **6-digit code**.
3. Type it into **BLE Pairing Passkey** and press **BLE Pairing Send Passkey** — within 30 s.

**BLE Pairing Required** goes to *OK*: paired. The ESP now reconnects on its own, also after new
ESPHome builds.

If it did not work: the code expired (the dashboard shows a new one — use that) · the code was
wrong (the field is cleared, try again) · the scooter was off or asleep (switch it on first).

### 3. Done

All entities you named in the YAML are on the ESP's device page in Home Assistant. Controls work
immediately; the update entities fill in after the first release check (a few minutes).

---

## Configuration

```yaml
librescoot_ble_client:
  id: librescoot_ble_client_hub
  mac_address: !secret librescoot_nrf_ble_mac_addr
  time_id: sntp_time          # optional, for the clock-set entities

  status:
    name: "Status"
  odometer:
    name: "Odometer"
  # … an entity exists only if you name it
```

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `mac_address` | required | The scooter's Bluetooth MAC. |
| `time_id` | — | A `time` source. Needed only for **System Time sync with ESP** and **System Time Set UTC ISO-8601**. |
| `update_check_interval` | `6h` | How often to look for a new release. |
| `github_token` | — | Optional GitHub token (read-only). Lifts the release-check limit from 60 to 5000 per hour. |
| `dbc_auto_power` | `true` | Switch the dashboard on when a dashboard update needs it: to hand the update over, to apply it, and to read its version. |
| `link_auto_hold` | `3min` | Link mode `auto`: how long to hold the connection before letting a phone in for ~20 s. |
| `link_interval` | `5min` | Link mode `interval`: how often to connect and refresh the sensors. |
| `presence_timeout` | `60s` | How long **BLE Presence** stays on after the scooter was last heard. |

Options for testing and special setups are listed under [Under the hood](#under-the-hood).
The component configures everything board-specific itself; do not add `web_server` — it takes
memory the firmware transfer needs, and Home Assistant shows everything it would.

---

## Pairing

The ESP never pairs by itself. It connects to a paired scooter silently and leaves an unpaired
one alone until you press **BLE Pairing Start** — a code appears on the dashboard only when you
asked for it.

**Pair, or pair again after a lost bond:** the [three steps](#2-pair) from the quick start.

**Move to a new ESP:** press **BLE Pairing Delete** on the old ESP, remove the ESP in the scooter's
phone app, then pair the new ESP. A scooter that still remembers the old ESP refuses the new one.

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| BLE Pairing Start | button | Start pairing. | `pair_scooter` |
| BLE Pairing Passkey | text | The code from the dashboard. Typing alone does not send it. | `ble_passkey` |
| BLE Pairing Send Passkey | button | Send the code. | `ble_send_code` |
| BLE Pairing Required | problem | *Problem*: scooter nearby but not paired. *OK*: paired and connected. | `pairing_required` |
| BLE Passkey Required | problem | On while the dashboard shows a code. | `passkey_required` |
| BLE Pairing Delete | button | Forget the scooter on the ESP. | `ble_remove_bond` |
| BLE Scooter nRF MAC | text sensor | The scooter this ESP is configured for. | `scooter_mac` |

---

## Controls

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| Scooter Lock | lock | Lock / unlock. Shows the real state; never unlocks by itself. | `scooter_lock` |
| Blinker | select | `off` / `left` / `right` / `both` | `blinker` |
| Seatbox Open | button | | `seatbox_open` |
| Hibernate / Wakeup | button | Power the scooter down / up. | `hibernate` / `wakeup` |
| Reboot MDB / Reboot MDB (hard) | button | Restart the scooter's computer (soft / power-cycle). | `reboot_mdb` / `reboot_mdb_hard` |
| MDB USB Mode | select | `Normal` / `Mass Storage` | `usb_mode` |
| Navigation Set to / Navigation Clear | text / button | Destination as `lat,lon[,name]`. | `navigation_set` / `navigation_clear` |
| Cancel Hibernate | button | | `cancel_hibernate` |
| Cellular APN | text | | `cellular_apn` |
| PM Scheduled Hibernation Enabled / Cron / Duration | switch / text | Scheduled hibernation. | `pm_scheduled_hibernate_enabled` / `_cron` / `_duration` |
| System Time sync with ESP / Set UTC ISO-8601 | button / text | Set the scooter's clock. | `system_time_sync` / `system_time_iso` |
| A-Refresh Sensor States | button | Re-read every sensor now. | `refresh` |
| Restart ESPHome Device | button | | `restart_esp` |

Switches and selects show what the scooter reports, not just what was last pressed.

---

## Sensors

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| Status | text | `parked`, `ready-to-drive`, `stand-by`, … | `status` |
| Seatbox / Handlebar Lock | text | | `seatbox` / `handlebar_lock` |
| Power State | text | | `power_state` |
| Odometer | km | | `odometer` |
| Battery 1 / 2 SoC · Cycles · State · Present | % / count / text / binary | The two traction batteries. Unknown while the slot is empty. | `battery_1_soc`, `battery_1_cycles`, `battery_1_state`, `battery_1_present`, `battery_2_…` |
| Aux Battery Voltage · Level · Aux Charge Status · Aux Charger | V / % / text / binary | The 12 V auxiliary battery and its charger. | `aux_voltage`, `aux_level`, `aux_charge_status`, `aux_charger` |
| CBB Battery Level · Charge Status · Remaining · Full Capacity · Cell Voltage | % / text / Ah / V | The connectivity battery. | `cbb_level`, `cbb_charge_status`, `cbb_remaining`, `cbb_full`, `cbb_cell` |
| MDB Power Mux Selected Input | text | | `power_mux` |
| Navigation Active · Navigation Available · Navigation Maps Available | binary | | `navigation_active`, `navigation_available`, `maps_available` |
| Keycard Count | text | | `keycard_count` |
| MDB UMS Status | binary | USB mass-storage mode active. | `ums_status` |
| SW MDB · SW DBC · SW nRF · SW ESP | text | Installed versions: scooter computer, dashboard, Bluetooth chip, this ESP. | `sw_mdb`, `sw_dbc`, `sw_nrf`, `sw_esp` |

Sensors read *unknown* while the ESP is not connected to the scooter.

---

## Bluetooth link

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| BLE Connection | binary | Connected right now. | `ble_connection` |
| BLE Presence | binary | Connected or nearby. | `ble_presence` |
| BLE RSSI | dBm | Signal strength, also while not connected. | `rssi` |
| BLE Link Mode | select | Who gets the scooter's single Bluetooth connection: | `ble_link_mode` |

| BLE Link Mode | Behaviour |
| :--- | :--- |
| `always` | Stay connected. Alarm events arrive instantly. The phone app cannot connect meanwhile. |
| `auto` (default) | Stay connected, but let go for ~20 s every `link_auto_hold` so a phone can connect. |
| `interval` | Connect every `link_interval`, refresh the sensors, disconnect. |
| `scan` | Do not connect; only notice whether the scooter is nearby. |
| `disconnect` | Off. |

A control you use while not connected is carried out as soon as the ESP connects. A firmware
transfer always keeps the connection.

---

## Alarm

The scooter's alarm arms itself when parked and reacts to motion, the seatbox, the handlebar and
the buttons — first blinkers, then the horn. The ESP shows the state and reports each trigger.

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| Alarm Status | text | `disabled` · `disarmed` · `delay-armed` · `armed` · `level-1-triggered` · `level-2-triggered` · `seatbox-access` | `alarm_status` |
| Alarm Triggered | binary | On while the alarm is going off. | `alarm_triggered` |
| Alarm Trigger | event | One event per trigger. Type = what set it off: `motion`, `seatbox`, `handlebar_position`, `handlebar_lock`, `brake_left`, `brake_right`, `horn_button`, `seatbox_button`. A trigger while the ESP was not connected is delivered on the next connect. | `alarm_trigger` |
| Alarm Last Trigger / Alarm Last Trigger Time | text | What and when, last time. | `alarm_last_trigger` / `alarm_last_trigger_time` |
| Alarm Armed | switch | Arm / disarm. | `alarm_armed` |
| Alarm Enabled | switch | The scooter's alarm setting. With it off, arming does nothing. | `alarm_enabled` |
| Alarm Start / Alarm Stop | button | Sound / silence the alarm now. | `alarm_start` / `alarm_stop` |
| Alarm Arm / Alarm Disarm | button | For older firmware without alarm state. | `alarm_arm` / `alarm_disarm` |

Needs scooter firmware with nRF **v2.11.0-ls** or newer. For instant alarm events use link mode
`always`.

---

## Dashboard power

The scooter switches its dashboard off while parked. **DBC Power** switches it on (~15 s) or off
without changing the scooter's state; the scooter switches it off again by itself later. A
dashboard firmware update needs the dashboard on — the ESP does that for you (`dbc_auto_power`).

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| DBC Power | switch | Dashboard on / off. | `dbc_power` |
| DBC Ready | binary | The dashboard has booted. | `dbc_ready` |

Needs nRF **v2.11.0-ls** or newer.

---

## OTA Updates

The ESP checks the LibreScoot GitHub releases and shows what is available in two update
entities: **OTA MDB Update** (the scooter's computer) and **OTA DBC Update** (the dashboard).
Press *Install* and the ESP transfers the firmware to the scooter over Bluetooth.

- Updates install **one at a time**.
- After an MDB update the **scooter reboots** by itself.
- A DBC update is handed to the dashboard, which must be on: the ESP switches it on before the
  handoff and again afterwards so the update applies. The scooter does not reboot.
- A transfer takes 5–60 minutes depending on size and range. It survives the scooter driving
  away and continues where it stopped.
- Nothing installs without you pressing *Install* or switching **OTA Auto Update** on.

### Install the next release

1. **OTA channel** → `stable`, `testing` or `nightly`.
2. **OTA Update Method** → `delta` (small, default) or `full` (whole image).
3. **OTA MDB Update** / **OTA DBC Update** show the next release with its notes. Press *Install*.
4. Follow **OTA Status**. When it says `Installed` and **SW MDB** / **SW DBC** show the new
   version, install the other one if it is offered.

### Update unattended (OTA Auto Update)

1. Set **OTA channel** and **OTA Update Method**.
2. Switch **OTA Auto Update** on.

The ESP installs one release after the other — scooter computer first, dashboard second — and
waits out each reboot until the scooter is current. Before every step it waits a few minutes
(2 after switching on, 5 after a scooter reboot), switches the dashboard on and asks both parts
for their versions; it never decides on a cached version. It switches itself **off** when done,
on the first failed install, and after every ESP restart. Switch it on in the evening, check
**SW MDB** / **SW DBC** in the morning.

### A specific version

1. Type the release tag into **OTA Version** (for example `nightly-20260909T063445`).
2. Press **OTA MDB Install** or **OTA DBC Install**.

With method `delta` the tag must be the release right after the installed one. For a version
further ahead use [Chained delta updates](#chained-delta-updates) or method `full`.

### Delta or full?

| | `delta` | `full` |
| :--- | :--- | :--- |
| Size | 0.5–7 MB | ~160 MB |
| Transfer | minutes | hours |
| Fits | only the release right after the installed one | any installed version |
| Use when | you keep the scooter current | the scooter is far behind, or a delta was refused |

The ESP checks a delta against the installed version before transferring it and refuses a
mismatch (`OTA Status: Error: delta needs …`).

### Chained delta updates

Several releases behind but no appetite for a full image: the Home Assistant integration merges
all deltas between the installed version and your target into one — one transfer, one install,
one reboot.

You need:

- the [Home Assistant integration](../../homeassistant-integration/README.md), and
  **OTA Source HA Relay** showing *Connected*
- **OTA Source** = `HA relay`
- **OTA Update Method** = `delta`
- **OTA Update Method delta-chaining** switched on

Then:

1. Type the target release into **OTA Version**.
2. Press **OTA MDB Install** (or **OTA DBC Install**).
3. **OTA Status** shows `Relay: building …` while Home Assistant prepares the bundle (up to a few
   minutes), then the usual transfer and install.

Chaining only happens for a target you typed in. The update entities and **OTA Auto Update**
always go one release at a time.

### Direct GitHub download (ESP32-S3)

On an **ESP32-S3** the ESP can download the firmware from GitHub by itself, so updates work
**without Home Assistant**. Set **OTA Source** to `direct GitHub` (the default on an S3).

| | `HA relay` (plain ESP32 and S3) | `direct GitHub` (S3 only) |
| :--- | :--- | :--- |
| Needs | the Home Assistant integration | nothing but the ESP |
| Chained delta updates | yes | no |

### OTA entities

| Entity | Type | Meaning | YAML key |
| :--- | :--- | :--- | :--- |
| OTA MDB Update / OTA DBC Update | update | Available release, release notes, install progress. | `mdb_update` / `dbc_update` |
| OTA MDB Install / OTA DBC Install | button | Install now — **OTA Version** if set, else the next release. | `ota_mdb_update` / `ota_dbc_update` |
| OTA Version | text | Target release. Prefilled with the next one. | `ota_version` |
| OTA channel | select | `stable` / `testing` / `nightly` | `ota_channel` |
| OTA Update Method | select | `delta` / `full` | `ota_update_method` |
| OTA Update Method delta-chaining | switch | See above. Off after every ESP restart. | `ota_delta_chaining` |
| OTA Auto Update | switch | See above. Off after every ESP restart. | `ota_auto_update` |
| OTA Stage Only | switch | Transfer but do not install (for testing). Off after every ESP restart. | `ota_stage_only` |
| OTA Source | select | `HA relay` / `direct GitHub` | `ota_source` |
| OTA Source URL | text | Where the firmware bytes come from. Set by the integration. | `ota_source_url` |
| OTA Source HA Relay | binary | The Home Assistant integration is reachable. | `ha_integration` |
| OTA Status | text | `Idle` · `Relay: building …` · `Uploading MDB 45%` · `Installing 30%` · `Pending reboot` · `Installed` · `Error: …` | `ota_status` |
| OTA Upload ETA | text | Remaining transfer time. | `ota_eta` |
| OTA Speed BLE Upload / OTA Speed Download | kB/s | Live transfer rates. | `ota_ble_upload_speed` / `ota_download_speed` |
| OTA Target Transferred · OTA BLE Bytes Total · OTA Auto-Resume Count | numbers | Transfer statistics. | `ota_target_transferred`, `ota_ble_bytes_total`, `ota_auto_resume_count` |
| OTA Status Request | button | Ask the scooter for its update state. | `ota_status_request` |
| OTA Abort | button | Stop the transfer. | `ota_abort` |
| OTA Reboot Required | problem | On when the scooter has not rebooted 20 min after an install. | `reboot_required` |

### When something goes wrong

| OTA Status says | Meaning | Do this |
| :--- | :--- | :--- |
| `Error: delta needs <tag>` | This delta does not fit the installed version. | Install the next release first, use chained delta updates, or method `full`. |
| `Error: chain: …` | Home Assistant could not merge the deltas (the text says why). | Pick a nearer target, or method `full`. |
| `Auto-resume #n (…)` | Bluetooth dropped some bytes. The transfer continues by itself. | Nothing. Closer is faster. |
| `Pending reboot` for a long time / **OTA Reboot Required** on | The scooter did not reboot after an MDB update. | Press **Reboot MDB (hard)**. |
| `Installing — waiting for scooter reboot…` after a DBC update | The dashboard is off, so the update has not applied yet. | Switch **DBC Power** on (`dbc_auto_power` does this by itself). |
| No update shown although there is a newer release | Scooter out of range, link mode `scan`/`disconnect`, or the scooter is still busy with the last install. | Bring it in range, choose `auto`/`always`, or wait. |
| Install refused: no firmware source | Neither the integration nor direct GitHub is available. | Install the [Home Assistant integration](../../homeassistant-integration/README.md), or use an ESP32-S3. |

---

## Command

Send any command of the scooter's text command channel (`cap:list` lists them). The reply
appears in **Command last response** (one line) and **Command response** (every line).

| Entity | Type | YAML key |
| :--- | :--- | :--- |
| Command | text | `command` |
| Command last response | text sensor | `command_last_response` |
| Command response | text sensor | `command_response` |

---

## Under the hood

Optional. The Bluetooth protocol is documented at <https://reference.librescoot.org/>.

**Extra options**

| Option | Default | Meaning |
| :--- | :--- | :--- |
| `github_repo` | `librescoot/librescoot` | Where firmware releases come from. |
| `ota_auto_resume` | `true` | Continue an interrupted transfer where the scooter left off. |
| `ota_source_default` | by board | `relay` or `github` — the initial **OTA Source**. |
| `use_cert_bundle` / `ca_certificate` | by board | TLS roots for GitHub. |
| `firmware_source` | — | For developers: an HTTP server on your network that serves the release files instead of GitHub. |
| `scooter_filter` | `scooter` | Name fragment that identifies a scooter when scanning. |

**What the component sets up per board.** From the chip and whether `psram:` is configured: with
PSRAM it downloads from GitHub with the Mozilla root bundle; without PSRAM it uses the Home
Assistant relay, pins the two GitHub roots itself, trims the Wi-Fi buffers, reserves one
Bluetooth connection slot and sends API state messages one at a time. On an ESP32-S3 it forces
the legacy Bluetooth connect (the BLE-5 extended connect loops against this scooter). Anything
set explicitly in `sdkconfig_options`, `api:` or `esp32_ble:` wins.

**Memory on a plain ESP32.** A transfer runs with ~40 kB of RAM to spare. No `web_server`, and
only the entities you use. A device that suddenly refuses Home Assistant connections or restarts
mid-transfer is short of RAM.

**Transfers.** `START` → windowed `DATA` → `COMPLETE` over the scooter's OTA service, SHA-256
from the GitHub release, resume from the scooter's staged offset, 120-byte chunks on a plain
ESP32. A delta's base version is read out of the file before any byte moves; a merged bundle is
checked against the official release's image hash. Bluetooth is the bottleneck: 2–4 kB/s.

**Versions.** The scooter computer's version comes from a Bluetooth characteristic and is
cross-checked with `status:version:mdb`; the dashboard's from `status:version:dbc`.

**Testing without GitHub.** `tools/range_server.py` serves the release files from your computer;
point **OTA Source URL** at it. `esphome.<node>_ota_test` (url, size, bundle, component) and
`esphome.<node>_ota_abort` drive the transfer engine directly.
