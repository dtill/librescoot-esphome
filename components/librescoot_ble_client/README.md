# Librescoot-BLE-Client (`librescoot_ble_client`)

An ESPHome external component that connects to the **LibreScoot / unu Scooter Pro** firmware
through the nRF52 BLE chip and controls the most common phone-app functions and a little more —
exposing every scooter characteristic as a Home Assistant entity.

Unlike the pure-YAML `ble_client` configuration, this component **owns the BLE client
itself** — the connection, the passkey pairing flow, every characteristic parser, the
extended-command engine, the BLE link manager and the full **BLE-OTA firmware-transfer
engine** all live in C++. The YAML only *names* the entities it wants.

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
| `github_repo` | string, `librescoot/librescoot` | Owner/name the firmware releases come from. |
| `update_check_interval` | time, `6h` | How often to poll GitHub for a newer release. |
| `stage_only` | bool, `false` | `true` = every transfer stops before `COMPLETE` (nothing installed). |
| `presence_timeout` | time, `60s` | **BLE Presence** stays Home if an advert was seen within this window. |
| `use_cert_bundle` | bool, `false` | Validate GitHub HTTPS against the ESP-IDF **Mozilla bundle** instead of the pinned roots (auto-enables `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`). Use on **PSRAM boards** for autonomous direct-GitHub downloads. |
| `ca_certificate` | string, optional | Explicit PEM root/chain for GitHub HTTPS (alternative to the bundle). |
| `firmware_source` | string, optional | Compile-time default byte source (e.g. a local mirror); the **OTA Source URL** entity still overrides at runtime. |
| `scooter_filter` | string, `scooter` | Case-insensitive substring an advertised BLE name must contain to count as a scooter in the scan. |

**Certificates come from YAML, not the component.** With neither `use_cert_bundle` nor
`ca_certificate` set, the two pinned GitHub roots in `github_ca.h` are the built-in fallback
(the classic uses this). On an **ESP32-S3 with PSRAM** the whole thing runs autonomously —
pairing and direct-from-GitHub OTA with no Home Assistant relay — with just `use_cert_bundle:
true` (plus `psram:` and `flash_size: 16MB` in the YAML). Do **not** set
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` (it stalls the update check); PSRAM is only needed for the
handshake — transfers **stream** through a small ring buffer regardless of image size.

Entity keys are **opt-in**: an entity is instantiated only if its key is present with a
`name`. Each key accepts the usual entity options (`name`, `id`, `icon`, `entity_category`,
`disabled_by_default`, …); sensible `entity_category`/`icon` defaults are baked in, so a
bare `name:` is enough.

---

## Pairing

The component performs pairing internally, but **never unprompted**. A bonded scooter
reconnects silently; the component only ever *initiates* fresh pairing when you ask it to
via the **Pair Scooter** button. If the scooter has lost its side of the bond (a firmware
update can reset it), an unrequested authentication attempt is refused and the link is
released rather than retried — so a scooter coming back into range is never spammed with
pairing requests. Re-bond deliberately:

1. Flash the config; add the device to Home Assistant via the ESPHome integration.
2. Power the scooter into parked mode.
3. Press **Pair Scooter** (or use the integration's pairing flow). This clears any stale
   bond and connects so the scooter shows a passkey; **Passkey Required** turns on.
4. In Home Assistant open **Settings → Developer Tools → Actions**, search for
   `passkey_reply`, enter the 6-digit code shown on the scooter dashboard, and
   **Perform Action**.
5. The bond is stored on the ESP; future reconnects are automatic and need no passkey.
   Flashing new firmware to the same chip keeps the bond.

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
| `scooter_mac` | Scooter MAC | text_sensor | on boot | The scooter MAC this ESP is configured for (`mac_address`), always shown — the HA integration reads it to preselect the connected scooter. |
| `ha_integration` | HA Integration | binary_sensor (connectivity) | 60 s | On = the Home Assistant **Librescoot-BLE-Client integration is active**, detected by pinging its plain-HTTP OTA relay (the `http://` OTA Source URL it sets). Off = no relay (autonomous/direct-GitHub, or the integration isn't installed). |

**OTA byte-source capability.** The component decides whether an OTA can even start: a plain-HTTP
source (the HA relay, or a local mirror) must be **reachable** (the `ha_integration` ping); an
`https://` source (GitHub) needs a working TLS path (`use_cert_bundle` / `ca_certificate` — a
PSRAM board, since the classic can't do the RSA-4096 CDN handshake). If neither is available,
**Install** is refused and a log line points at the HA integration — so on a classic without the
integration you get a clear "install the HA integration or use an S3+PSRAM board" hint rather than
a mid-transfer failure.

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
| `pair_scooter` | Pair Scooter | button | Arm a deliberate (re)pairing: clear any stale bond and connect so the scooter issues a passkey. The only trigger for fresh pairing. |
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
| `ota_status` | OTA Status | text_sensor | on notify | Decoded status (`Idle`, `Uploading MDB 45%`, `Installing — waiting for scooter reboot…`, `Pending reboot`, …). |
| `ota_eta` | OTA Upload ETA | text_sensor | on progress | Estimated time remaining for the running upload (`HH:MM:SS`). |
| `ota_status_request` | OTA Status Request | button | — | `STATUS_REQ`. |
| `ota_abort` | OTA Abort | button | — | Abort the current session **and** drop the rest of the queue (so DBC doesn't start after aborting MDB), and cancel any wait for the post-install reboot. |
| `reboot_required` | Reboot Required | binary_sensor (problem) | 30 s while pending | On when the scooter has reported **pending-reboot for >20 min**. `device_class: problem`, shown by default. The HA integration raises a "restart the scooter" Repair from it. See below. |

### Firmware update — two `update` entities, plus manual install (`update` service `9a590500`)

The scooter is **two** independently-flashed boards: the **MDB** (the i.MX main computer,
which also hosts the nRF/BLE receiver) and the **DBC** (the dashboard). Each is a Mender A/B
image with its own version, and **the scooter reboots after installing either one** — so a
single BLE session cannot flash both back-to-back. The component therefore exposes **one
`update` entity per board** plus **one manual install button per board**:

| Key | Entity | Type | Description |
| :--- | :--- | :--- | :--- |
| `mdb_update` | OTA MDB Update | update | MDB firmware check/install (i.MX version from `9a59a041`). |
| `dbc_update` | OTA DBC Update | update | DBC firmware check/install (dashboard version from `status:version:dbc`). |
| `ota_mdb_update` | OTA MDB Install | button | Manual MDB install — **always** works, ignores the availability gate. |
| `ota_dbc_update` | OTA DBC Install | button | Manual DBC install — **always** works, ignores the availability gate. |
| `ota_version` | OTA Version | text | Optional target tag (e.g. `nightly-20260803T062157`); empty = channel default. |
| `ota_update_method` | OTA Update Method | select | `delta` (small patch, default) or `full` (complete `.mender`, hundreds of MB). |
| `ota_source_url` | OTA Source URL | text | Byte source for the transfer (default GitHub; a local mirror drops TLS — see below). |

#### The update check

Both entities share one GitHub-releases query (TLS, in a worker task) that finds the newest
release for the **selected channel** and compares it against the running versions. The channel
comes from the `OTA channel` select, which **reflects the running channel** parsed from the MDB
version (`nightly-…` → `nightly`). `nightly`/`testing` pick the newest tag by timestamp;
`stable` uses `/releases/latest`. The comparison is case-insensitive (the BLE version reports a
lowercase `t` in the timestamp, the tag an uppercase `T`).

Checks run when the versions are first known and every ~6 h; **Home Assistant's "check for
updates" triggers one too** (the API `UPDATE_COMMAND_CHECK` reaches the entity's `check()`,
which starts the same query).

**Multi-hop changelog.** When the installed version is several releases behind, the release
notes shown in the HA card aggregate **every** release in between — a header
(`N releases since installed:` + a bullet list of tags) followed by each release's own notes,
newest first — so a jump that skips nightlies still shows what changed at every step. The MDB
card additionally prepends the download size and an install-time estimate.

#### How availability is decided (two-phase, DBC-first)

Mirroring the phone app, the component brings the **dashboard up to match the main board
first**, then advances the main board:

- **DBC update** is offered only when the DBC version is **behind the MDB** — a "catch up to
  the board" install; its default target is the **MDB's current version** (not the channel
  latest).
- **MDB update** is offered only when the MDB is behind the channel latest **and** the DBC has
  already caught up. While the DBC is behind, the MDB card is held at "up to date".

Both offers are additionally **gated on the scooter being idle**: a new update is shown only
while the scooter's OTA phase is `idle` (`0x06`). Any other phase — "unknown" (before the
first `STATUS_REQ` reply) or "pending reboot" after an install — hides the offer. Because Home
Assistant derives "update available" purely from `latest_version != installed_version`, the
entity publishes `latest == installed` to hide it; the live phase/percent still shows in the
**OTA Status** text. A `STATUS_REQ` is sent on every connect so the phase is known.

An **`unknown` installed version is never offered an update.** In stand-by the scooter reports
`status:version:dbc:unknown` (the dashboard is off), so the DBC's installed version reads
`unknown`; the component treats that as "no version" and publishes `latest == installed` so Home
Assistant shows no update. A component whose version is genuinely known and behind is still
offered normally.

The two manual **OTA … Install** buttons ignore all of the above and start immediately, using
`OTA Version` if set, else the channel latest (MDB) / the MDB version (DBC).

#### How the transfer works — and where MDB and DBC differ

**On the wire the two transfers are the same code path.** The component sends the same
`START` → `DATA` → `COMPLETE` sequence over the same three characteristics, with the same ring
buffer, HTTP producer task, sliding window, cumulative ACKs, go-back-N on REWIND, SHA-256 (from
the GitHub asset `digest`) in START, resume-on-reconnect and progress-bar handling. Only these
differ:

| | MDB | DBC |
| :--- | :--- | :--- |
| `component` byte in `START` | `0x00` | `0x01` |
| Bundle basename | `librescoot-unu-mdb-<tag>.<ext>` | `librescoot-unu-dbc-<tag>.<ext>` |
| "Installed version" source | `9a59a041` (i.MX) | `status:version:dbc` (dashboard) |
| Default install target | channel latest | the **MDB's** running version |
| Post-install confirmation | waits for `9a59a041` to report the target | waits for `status:version:dbc` to report the target |

**What the scooter does with each is different — and that part is entirely scooter-side:**

- **MDB:** the receiver queues the bundle on `scooter:update:mdb`; `update-service` runs
  `mender-update install` and the MDB **reboots itself after 3 minutes of sustained stand-by**.
  Its `pending-reboot` therefore clears on its own.
- **DBC:** the bundle is received on the **MDB first**, then handed off to the dashboard —
  `vehicle-service` forces dashboard power on, transfers the bundle to the dashboard's
  data-server, and the dashboard's own `update-service` installs it. The dashboard is **also**
  a Mender image, so it **also needs a reboot** to activate; but that reboot is gated by the
  vehicle state machine / dashboard power, not the 3-minute stand-by timer.

**Why "pending reboot" can stick, and the stuck-reboot watchdog.** The BLE `INSTALL_PROGRESS`
notification (`[0x84][phase][percent][msg]`) carries **no component field**, so the single
OTA_STATUS characteristic cannot say *which* board is pending. `pending-reboot` (phase `0x02`)
is a legitimate phase for **both** boards — both must reboot to switch Mender partitions. The
MDB auto-reboots after 3 min stand-by; a DBC install applies on the **next dashboard power
cycle**. But the OTA_STATUS characteristic is a **latch of the last BLE-OTA session's terminal
progress** — it is *not* re-synced to the scooter's live update state. So even after the
dashboard has rebooted and both versions match again, `STATUS_REQ` can keep returning
"pending reboot" (observed live: both boards on the new version, update-service back to idle,
yet the char still says pending-reboot). Because we **cannot** tell a genuine pending reboot
from a stale latch — and version equality is not proof either, since a reboot may still be
pending right after an install — the component does **not** guess. Instead, a **watchdog**
re-requests `STATUS_REQ` every 30 s while pending-reboot and, if the status stays pending for
**more than 20 minutes**, turns on the `reboot_required` problem sensor. The Home Assistant
integration surfaces that as a Repair offering to **restart the scooter**, which clears both
cases. The idle-gate above is left strict on purpose. (Firmware-side, the fix would be to reset
OTA_STATUS to idle when update-service clears the `ota` hash, or expose per-component status
over BLE — under discussion upstream.)

#### Staging vs installing, and the byte source

The transfer is **stage-only by default** (`stage_only: true`): it streams the entire DATA
phase and stops *before* `COMPLETE`, so nothing is installed — this exercises the whole state
machine safely. With `stage_only: false`, a completed transfer sends `COMPLETE`, the scooter
verifies the SHA-256 against the staged file and queues the real install. Installing firmware
onto a road vehicle is a deliberate act.

**Direct GitHub download needs an S3+PSRAM board.** On the ESP32-classic the RSA-4096 handshake
to `objects.githubusercontent.com` runs out of contiguous heap while BLE is active
(`api.github.com` metadata uses ECC and is fine). Point **`OTA Source URL`** at a local HTTP
mirror to drop TLS from the bulk download — faster (~2×), reliable (0 rewinds), and the
recommended path on the classic. Metadata (size/SHA) always still comes from the GitHub API.

```bash
mkdir -p mirror/<tag> && cd mirror
# fetch the real bundles so the SHA verifies (only needed for a real COMPLETE/install):
gh release download <tag> -R librescoot/librescoot -p '*mdb*.delta' -p '*dbc*.delta' -D <tag>
python3 ../tools/range_server.py         # Range-capable; python -m http.server does NOT resume
```

Then set **`OTA Source URL`** to `http://<host>:8000` and press an install button. The
`ota_test` / `ota_abort` API actions drive the engine with an explicit
`url`/`size`/`bundle`/`component` for pure-local testing without GitHub or version comparison.

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
