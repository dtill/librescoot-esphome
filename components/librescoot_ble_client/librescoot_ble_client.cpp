#include "librescoot_ble_client.h"
#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "github_ca.h"

namespace esphome {
namespace librescoot_ble_client {

static const char *const TAG = "librescoot_ble_client";

// "auto" link mode: how long to keep the link released after a disconnect so another
// central (the phone app) can win the reconnect race before we try again.
static const uint32_t LINK_YIELD_MS = 20000;
// How long a deliberate pairing stays armed after 'Pair Scooter'. Generous so the user has time to
// read the passkey off the dashboard and type it into Home Assistant; extended on each PASSKEY_REQ.
static const uint32_t PAIR_ARM_WINDOW_MS = 300000;  // 5 min
// "interval" link mode: how long to stay connected per cycle — long enough for on_connected_ to
// read every characteristic once (reads are serialised, ~25 chars) plus the on-connect queries.
static const uint32_t INTERVAL_DWELL_MS = 30000;  // 30 s

// Connect-on-demand: how long to hold the link after running the queued action(s) to let a
// response arrive, the cap while still awaiting a command reply, and the connect timeout.
static const uint32_t ONDEMAND_RESP_MS = 4000;
static const uint32_t ONDEMAND_RESP_CAP_MS = 15000;
static const uint32_t ONDEMAND_CONNECT_TIMEOUT_MS = 30000;

// GITHUB_ROOTS is defined in github_ca.h (byte-exact, generated from the verified roots).

// Base UUID of every LibreScoot nRF service/characteristic.
static espbt::ESPBTUUID uuid(const char *s) { return espbt::ESPBTUUID::from_raw(std::string(s)); }

static const char *const SVC_CONTROL = "9a590000-6e67-5d0d-aab9-ad9126b66f91";
static const char *const CH_CONTROL = "9a590001-6e67-5d0d-aab9-ad9126b66f91";   // lock/blinker/seatbox
static const char *const CH_POWER = "9a590002-6e67-5d0d-aab9-ad9126b66f91";     // hibernate/wakeup/reboot
static const char *const SVC_STATUS = "9a590020-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_POWERSTATE = "9a5900a0-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_MAINBAT = "9a5900e0-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_AUX = "9a590040-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_CBB = "9a590060-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_POWERMUX = "9a590100-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_SCOOTERINFO = "9a59a040-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_SYSINFO = "9a59a000-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_EXT = "9a590400-6e67-5d0d-aab9-ad9126b66f91";
static const char *const CH_EXT = "9a590401-6e67-5d0d-aab9-ad9126b66f91";
static const char *const CH_EXT_RESP = "9a590402-6e67-5d0d-aab9-ad9126b66f91";
static const char *const SVC_OTA = "9a590500-6e67-5d0d-aab9-ad9126b66f91";
static const char *const CH_OTA_DATA = "9a590501-6e67-5d0d-aab9-ad9126b66f91";
static const char *const CH_OTA_CONTROL = "9a590502-6e67-5d0d-aab9-ad9126b66f91";
static const char *const CH_OTA_STATUS = "9a590503-6e67-5d0d-aab9-ad9126b66f91";

static uint32_t u32le(const uint8_t *v) {
  return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
}

static bool ieq(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i]))
      return false;
  return true;
}

static std::string mac_to_str(uint64_t m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t) (m >> 40), (uint8_t) (m >> 32),
           (uint8_t) (m >> 24), (uint8_t) (m >> 16), (uint8_t) (m >> 8), (uint8_t) m);
  return b;
}

// Case-insensitive substring test (for matching a scooter's advertised name against the filter).
static bool icontains(const std::string &hay, const std::string &needle) {
  if (needle.empty() || needle.size() > hay.size())
    return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
    size_t j = 0;
    for (; j < needle.size(); j++)
      if (tolower((unsigned char) hay[i + j]) != tolower((unsigned char) needle[j]))
        break;
    if (j == needle.size())
      return true;
  }
  return false;
}

// BLE reports the version with a lowercase 't' in the timestamp (nightly-20260730t200638);
// the GitHub release tag uses uppercase 'T'. Convert so a DBC "catch up to MDB" target resolves.
static std::string version_to_tag(const std::string &v) {
  std::string t = v;
  for (size_t i = 1; i + 1 < t.size(); i++)
    if (t[i] == 't' && isdigit((unsigned char) t[i - 1]) && isdigit((unsigned char) t[i + 1]))
      t[i] = 'T';
  return t;
}

static std::string fmt_size(uint32_t bytes) {
  char b[24];
  if (bytes >= 1024u * 1024)
    snprintf(b, sizeof(b), "%.1f MB", (double) bytes / (1024.0 * 1024.0));
  else if (bytes >= 1024)
    snprintf(b, sizeof(b), "%u KB", (unsigned) ((bytes + 512) / 1024));
  else
    snprintf(b, sizeof(b), "%u B", (unsigned) bytes);
  return b;
}

// Rough BLE-OTA transfer estimate at ~12 kB/s (the measured ESP32-classic rate).
static std::string fmt_est_time(uint32_t total_bytes) {
  uint32_t secs = total_bytes / 12000u;
  char b[24];
  if (secs >= 3600)
    snprintf(b, sizeof(b), "~%uh %um", (unsigned) (secs / 3600), (unsigned) ((secs % 3600) / 60));
  else if (secs >= 60)
    snprintf(b, sizeof(b), "~%u min", (unsigned) ((secs + 30) / 60));
  else
    snprintf(b, sizeof(b), "<1 min");
  return b;
}

static std::string channel_of(const std::string &version) {
  size_t dash = version.find('-');
  std::string pre = dash == std::string::npos ? version : version.substr(0, dash);
  if (pre == "nightly" || pre == "testing" || pre == "stable")
    return pre;
  return "stable";  // semver tags (e.g. v1.1.0) are the stable channel
}

std::string LibrescootBleClient::clean_str_(const uint8_t *data, uint16_t len) {
  std::string s(reinterpret_cast<const char *>(data), len);
  size_t z = s.find('\0');
  if (z != std::string::npos)
    s.resize(z);
  return s;
}

// ---------------------------------------------------------------------------
// setup / char table
// ---------------------------------------------------------------------------
void LibrescootBleClient::build_char_table_() {
  auto add = [this](CharId id, const char *svc, const char *ch, bool notify, uint32_t interval) {
    this->chars_.push_back(CharEntry{id, uuid(svc), uuid(ch), 0, {}, notify, false, 0, interval, 0});
  };
  bool p1 = bat1_soc_ || bat1_cycles_ || bat1_state_ || bat1_present_;
  bool p2 = bat2_soc_ || bat2_cycles_ || bat2_state_ || bat2_present_;
  if (p1)
    add(CharId::BAT1_PRESENT, SVC_MAINBAT, "9a5900e3-6e67-5d0d-aab9-ad9126b66f91", false, 10000);
  if (bat1_soc_)
    add(CharId::BAT1_SOC, SVC_MAINBAT, "9a5900e9-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (bat1_cycles_)
    add(CharId::BAT1_CYCLES, SVC_MAINBAT, "9a5900e6-6e67-5d0d-aab9-ad9126b66f91", false, 600000);
  if (bat1_state_)
    add(CharId::BAT1_STATE, SVC_MAINBAT, "9a5900e2-6e67-5d0d-aab9-ad9126b66f91", false, 120000);
  if (p2)
    add(CharId::BAT2_PRESENT, SVC_MAINBAT, "9a5900ef-6e67-5d0d-aab9-ad9126b66f91", false, 10000);
  if (bat2_soc_)
    add(CharId::BAT2_SOC, SVC_MAINBAT, "9a5900f5-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (bat2_cycles_)
    add(CharId::BAT2_CYCLES, SVC_MAINBAT, "9a5900f2-6e67-5d0d-aab9-ad9126b66f91", false, 600000);
  if (bat2_state_)
    add(CharId::BAT2_STATE, SVC_MAINBAT, "9a5900ee-6e67-5d0d-aab9-ad9126b66f91", false, 120000);

  if (aux_voltage_)
    add(CharId::AUX_VOLTAGE, SVC_AUX, "9a590041-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (aux_level_)
    add(CharId::AUX_LEVEL, SVC_AUX, "9a590044-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (aux_charge_)
    add(CharId::AUX_CHARGE, SVC_AUX, "9a590043-6e67-5d0d-aab9-ad9126b66f91", false, 120000);

  if (cbb_level_)
    add(CharId::CBB_LEVEL, SVC_CBB, "9a590061-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (cbb_remaining_)
    add(CharId::CBB_REMAINING, SVC_CBB, "9a590063-6e67-5d0d-aab9-ad9126b66f91", false, 600000);
  if (cbb_full_)
    add(CharId::CBB_FULL, SVC_CBB, "9a590064-6e67-5d0d-aab9-ad9126b66f91", false, 600000);
  if (cbb_cell_)
    add(CharId::CBB_CELL, SVC_CBB, "9a590065-6e67-5d0d-aab9-ad9126b66f91", false, 120000);
  if (cbb_charge_)
    add(CharId::CBB_CHARGE, SVC_CBB, "9a590072-6e67-5d0d-aab9-ad9126b66f91", false, 120000);

  if (odometer_)
    add(CharId::ODOMETER, SVC_SCOOTERINFO, "9a59a042-6e67-5d0d-aab9-ad9126b66f91", false, 120000);
  if (nav_active_)
    add(CharId::NAV_ACTIVE, SVC_SCOOTERINFO, "9a59a044-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (ums_status_ || usb_mode_)
    add(CharId::UMS_STATUS, SVC_SCOOTERINFO, "9a59a045-6e67-5d0d-aab9-ad9126b66f91", false, 60000);
  if (sw_mdb_ || mdb_update_ || dbc_update_)
    add(CharId::SW_MDB, SVC_SCOOTERINFO, "9a59a041-6e67-5d0d-aab9-ad9126b66f91", false, 1800000);
  if (sw_nrf_)
    add(CharId::SW_NRF, SVC_SYSINFO, "9a59a001-6e67-5d0d-aab9-ad9126b66f91", false, 1800000);

  if (power_mux_)
    add(CharId::POWER_MUX, SVC_POWERMUX, "9a590101-6e67-5d0d-aab9-ad9126b66f91", false, 600000);
  if (status_ || scooter_lock_)
    add(CharId::STATUS, SVC_STATUS, "9a590021-6e67-5d0d-aab9-ad9126b66f91", false, 5000);
  if (seatbox_)
    add(CharId::SEATBOX, SVC_STATUS, "9a590022-6e67-5d0d-aab9-ad9126b66f91", false, 5000);
  if (handlebar_)
    add(CharId::HANDLEBAR, SVC_STATUS, "9a590023-6e67-5d0d-aab9-ad9126b66f91", false, 5000);
  if (power_state_)
    add(CharId::POWER_STATE, SVC_POWERSTATE, "9a5900a1-6e67-5d0d-aab9-ad9126b66f91", false, 5000);

  bool uses_ext = command_text_ || cmd_response_ || cmd_last_response_ || keycard_count_ || sw_dbc_ ||
                  maps_available_ || nav_available_ || apn_text_ || pm_sched_hib_ || pm_cron_text_ ||
                  pm_duration_text_ || usb_mode_ || ota_channel_ || alarm_enabled_ || alarm_armed_;
  if (uses_ext) {
    add(CharId::CMD_RESPONSE, SVC_EXT, CH_EXT_RESP, true, 0);
    add(CharId::EXT_CMD, SVC_EXT, CH_EXT, false, 0);
  }
  // OTA service: status notify + control/data write targets. Registered whenever the OTA
  // status entity or the update entity is present (the transfer engine needs all three).
  bool uses_ota = ota_status_ || mdb_update_ || dbc_update_;
  if (uses_ota) {
    add(CharId::OTA_STATUS, SVC_OTA, CH_OTA_STATUS, true, 0);
    add(CharId::OTA_DATA, SVC_OTA, CH_OTA_DATA, false, 0);
  }

  // Write targets (handles resolved on discovery; harmless if the firmware lacks them).
  add(CharId::CTRL_CMD, SVC_CONTROL, CH_CONTROL, false, 0);
  add(CharId::POWER_CMD, SVC_CONTROL, CH_POWER, false, 0);
  add(CharId::OTA_CONTROL, SVC_OTA, CH_OTA_CONTROL, false, 0);
}

CharEntry *LibrescootBleClient::find_char_(CharId id) {
  for (auto &e : this->chars_)
    if (e.id == id)
      return &e;
  return nullptr;
}

void LibrescootBleClient::setup() {
  BLEClientBase::setup();
  this->build_char_table_();

  this->link_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("librescoot_ble_client_link_mode"));
  uint8_t idx = 2;  // default: auto
  this->link_pref_.load(&idx);
  static const char *const MODES[] = {"disconnect", "scan", "auto", "always", "interval"};
  this->link_mode_str_ = MODES[idx <= 4 ? idx : 2];

  if (this->scooter_lock_ != nullptr)
    this->scooter_lock_->publish_state(lock::LOCK_STATE_LOCKED);
  if (this->cmd_response_ != nullptr)
    this->cmd_response_->publish_state("(no command run yet)");
  if (this->cmd_last_response_ != nullptr)
    this->cmd_last_response_->publish_state("(no command run yet)");
  if (this->command_text_ != nullptr)
    this->command_text_->publish_state("cap:list");
  if (this->ota_channel_ != nullptr)
    this->ota_channel_->publish_state("undefined");
  if (this->ota_method_ != nullptr)
    this->ota_method_->publish_state(this->ota_method_str_);
  if (this->passkey_required_ != nullptr)
    this->passkey_required_->publish_state(false);
  if (this->pairing_required_sensor_ != nullptr)
    this->pairing_required_sensor_->invalidate_state();  // unknown until a connection is confirmed
  if (this->reboot_required_ != nullptr)
    this->reboot_required_->publish_state(false);
  if (this->sw_esp_ != nullptr)
    this->sw_esp_->publish_state(ESPHOME_VERSION);
  if (this->scooter_mac_sensor_ != nullptr)
    this->scooter_mac_sensor_->publish_state(this->get_address() != 0 ? mac_to_str(this->get_address()) : "not set");
  if (this->ha_integration_ != nullptr)
    this->ha_integration_->publish_state(false);  // updated from the first relay ping (see loop)
  if (this->link_mode_ != nullptr)
    this->link_mode_->publish_state(this->link_mode_str_);

  // Restore the OTA source URL from NVS so the download source + relay reachability keep working
  // after an ESP-only reboot (HA may not restart to re-push it).
  this->url_pref_ = global_preferences->make_preference<UrlPref>(fnv1_hash("librescoot_ble_client_ota_url"));
  UrlPref up{};
  if (this->url_pref_.load(&up) && up.url[0] != '\0')
    this->ota_source_url_.assign(up.url, strnlen(up.url, sizeof(up.url)));

  if (this->ota_source_url_text_ != nullptr)
    this->ota_source_url_text_->publish_state(this->ota_source_url_);

  // OTA byte source mode (github/relay): NVS-persisted; else the compile-time default (chip-based or
  // YAML `ota_source:`). github mode reconstructs the URL from github_base_(); relay keeps the
  // persisted/integration URL restored just above.
  this->ota_source_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("librescoot_ble_client_ota_source"));
  uint8_t osv;
  std::string osmode = this->ota_source_default_;
  if (this->ota_source_pref_.load(&osv))
    osmode = osv ? "github" : "relay";
  this->apply_ota_source_(osmode, false);  // don't re-persist on boot

  // Reachability of the HA-integration relay runs on the scheduler, not loop(): BLEClientBase
  // disables our loop() whenever the BLE link is idle, but this check must keep working then.
  this->set_interval("hi_check", 5000, [this]() { this->service_integration_check_(); });
  // Same reason (loop() disabled while idle): watch for the configured scooter being in range but
  // unbonded, so we can flag "pairing required" without ever auto-pairing.
  this->set_interval("pairing_watch", 2000, [this]() { this->service_pairing_watch_(); });
  // Drives the "interval" link mode (connect → refresh all sensors → release, every N minutes).
  // Also a scheduler timer so it fires while the link is down and loop() is disabled.
  this->set_interval("link_interval", 2000, [this]() { this->service_link_interval_(); });

  // Allocate the OTA ring buffer once, while the heap is fresh (a runtime alloc fails on the
  // classic board once TLS has fragmented the heap). HW-adaptive: with PSRAM (S3) use the full
  // 64-chunk window; on the classic use 32 chunks to spare the internal heap. The ring bounds
  // the in-flight window; the negotiated window is capped to fit.
  if (this->ota_status_ != nullptr || this->mdb_update_ != nullptr || this->dbc_update_ != nullptr) {
    bool has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    this->ota_cap_ = (has_psram ? 64u : 32u) * 240u;
    this->ota_buf_ = (uint8_t *) heap_caps_malloc(this->ota_cap_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (this->ota_buf_ == nullptr)
      this->ota_buf_ = (uint8_t *) malloc(this->ota_cap_);
    if (this->ota_buf_ == nullptr) {
      ESP_LOGW(TAG, "OTA ring buffer alloc failed; OTA transfer disabled");
      this->ota_cap_ = 0;
    } else {
      ESP_LOGCONFIG(TAG, "OTA ring: %u B (%s)", (unsigned) this->ota_cap_, has_psram ? "PSRAM" : "internal");
    }
  }

  this->apply_link_state_();
}

void LibrescootBleClient::dump_config() {
  ESP_LOGCONFIG(TAG, "Librescoot BLE Client:");
  BLEClientBase::dump_config();
  ESP_LOGCONFIG(TAG, "  Characteristics: %u   Link mode: %s", (unsigned) this->chars_.size(),
                this->link_mode_str_.c_str());
}

void LibrescootBleClient::set_enabled(bool enabled) {
  if (enabled == this->enabled_)
    return;
  this->enabled_ = enabled;
  if (!enabled) {
    ESP_LOGI(TAG, "Disabling BLE client (link handed back)");
    this->disconnect();
  }
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------
void LibrescootBleClient::loop() {
  BLEClientBase::loop();
  const uint32_t now = millis();

  // BLEClientBase disables our loop() whenever the BLE client is IDLE (e.g. disconnected, or the
  // scooter's single link is held by another central). But the GitHub update-check worker and the
  // OTA engine publish their results *here* in loop(), so keep it running while any such work is
  // pending — otherwise a check/transfer started while disconnected would never be applied.
  if (this->gh_running_ || this->gh_done_ || this->ota_state_ != OtaState::IDLE ||
      this->ota_awaiting_version_ || this->ota_resolve_running_ || !this->ota_jobs_.empty())
    this->enable_loop();

  // Presence + connection (once a second).
  if (now - this->last_pres_pub_ms_ >= 1000) {
    this->last_pres_pub_ms_ = now;
    bool conn = this->connected();
    if (this->ble_connection_ != nullptr)
      this->ble_connection_->publish_state(conn);
    if (this->ble_presence_ != nullptr)
      this->ble_presence_->publish_state(conn || (now - this->last_adv_ms_ < this->presence_timeout_ms_));
  }

  // HA-integration reachability is serviced from a scheduler interval (service_integration_check_),
  // not here — loop() is disabled by BLEClientBase while the BLE link is idle.

  // GitHub update check: consume a finished worker, then schedule the next one.
  if (this->gh_done_) {
    this->gh_done_ = false;
    this->apply_check_result_();
    this->gh_running_ = false;
  }
  // Require a valid installed MDB version before checking: the multi-release changelog is built
  // for the range (installed MDB → channel latest), so checking before the version is known would
  // aggregate only the latest release. (DBC alone isn't enough — it reads "unknown" in stand-by.)
  if ((this->mdb_update_ != nullptr || this->dbc_update_ != nullptr) && !this->gh_running_ &&
      now >= this->next_check_ms_ && !this->mdb_version_.empty() && !ieq(this->mdb_version_, "unknown")) {
    this->next_check_ms_ = now + this->update_check_interval_;  // provisional; shortened on failure
    this->request_update_check();
  }

  // BLE-OTA transfer engine (consumer side).
  this->ota_step_();

  // Install: when the resolve worker finishes, queue ONLY the requested component's transfer
  // (the scooter reboots after each install, so MDB and DBC are separate install actions).
  if (this->ota_resolve_done_) {
    this->ota_resolve_done_ = false;
    this->ota_resolve_running_ = false;
    this->ota_jobs_.clear();
    if (this->ota_cancel_) {
      this->ota_cancel_ = false;  // aborted while the resolve was in flight — discard the result
    } else {
      std::string base = this->rs_source_ + "/" + this->rs_tag_ + "/";
      auto bundle_id = [this](const std::string &name) -> std::string {
        const std::string suf = ".mender";
        if (this->ota_method_str_ == "full" && name.size() > suf.size() &&
            name.compare(name.size() - suf.size(), suf.size(), suf) == 0)
          return name.substr(0, name.size() - suf.size());
        return name;
      };
      bool mdb = this->ota_install_component_ == 0;
      bool ok = mdb ? this->rs_mdb_ok_ : this->rs_dbc_ok_;
      if (ok) {
        const std::string &nm = mdb ? this->rs_mdb_name_ : this->rs_dbc_name_;
        const std::string &sha = mdb ? this->rs_mdb_sha_ : this->rs_dbc_sha_;
        uint32_t sz = mdb ? this->rs_mdb_size_ : this->rs_dbc_size_;
        this->ota_jobs_.push_back({base + nm, sha, bundle_id(nm), sz, this->ota_install_component_});
        ESP_LOGI("ota", "install: %s %s queued for %s", mdb ? "MDB" : "DBC", nm.c_str(), this->rs_tag_.c_str());
      } else {
        ESP_LOGW("ota", "install: no %s %s asset for %s", mdb ? "MDB" : "DBC",
                 this->ota_method_str_.c_str(), this->rs_tag_.c_str());
        if (this->ota_status_ != nullptr)
          this->ota_status_->publish_state(std::string("Error: version not found: ") + this->rs_tag_);
      }
    }
  }
  this->ota_kick_next_job_();

  // Post-install: wait for the reboot and the installed component's new version via BLE.
  if (this->ota_awaiting_version_) {
    const std::string &t = this->ota_await_tag_;
    const std::string &v = this->ota_install_component_ == 0 ? this->mdb_version_ : this->dbc_version_;
    if (!v.empty() && ieq(v, t)) {
      ESP_LOGI("ota", "%s now reports %s — install confirmed",
               this->ota_install_component_ == 0 ? "MDB" : "DBC", t.c_str());
      this->ota_awaiting_version_ = false;
      if (this->ota_status_ != nullptr)
        this->ota_status_->publish_state("Installed");
      this->ota_settle_update_entity_();
    } else if (now >= this->ota_await_until_ms_) {
      ESP_LOGW("ota", "install: new version not confirmed in time; resetting");
      this->ota_awaiting_version_ = false;
      this->ota_settle_update_entity_();
    }
  }

  // 30 s after the scooter reported "booting", force a full re-read (fresh data + confirm a
  // new firmware version). Skipped if the link isn't up yet — the on-connect read covers that.
  if (this->boot_reread_at_ms_ != 0 && now >= this->boot_reread_at_ms_) {
    this->boot_reread_at_ms_ = 0;
    if (this->state() == espbt::ClientState::ESTABLISHED) {
      ESP_LOGI(TAG, "post-boot: re-reading all sensors");
      this->refresh_();
    }
  }

  // Stuck "pending reboot" watchdog. While the scooter reports pending-reboot, re-request the
  // status every 30 s (read-only) so a clear is noticed promptly; if it stays pending for
  // >20 min, raise the reboot-required problem so the HA integration can offer a manual restart.
  // We do NOT infer "done" from version equality — a reboot can still be genuinely pending even
  // when both versions already read equal (e.g. a DBC install that just finished).
  if (this->ota_scooter_phase_ == 0x02 && this->state() == espbt::ClientState::ESTABLISHED) {
    if (this->ota_pending_since_ms_ == 0)
      this->ota_pending_since_ms_ = now;
    if (this->ota_pending_last_req_ms_ == 0 || now - this->ota_pending_last_req_ms_ >= 30000) {
      this->ota_pending_last_req_ms_ = now;
      const uint8_t d[] = {0x05};  // STATUS_REQ — read-only
      this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
    }
    if (!this->reboot_problem_ && now - this->ota_pending_since_ms_ >= 20UL * 60 * 1000) {
      this->reboot_problem_ = true;
      if (this->reboot_required_ != nullptr)
        this->reboot_required_->publish_state(true);
      ESP_LOGW(TAG, "scooter pending-reboot for >20 min — manual restart may be needed");
    }
  }

  // If the OTA phase never resolved (0xFF) — e.g. the on-connect STATUS_REQ was missed on a flaky
  // link — keep asking every 15 s while connected. Otherwise the idle-gate stays shut and a real,
  // available update is never offered until someone presses STATUS_REQ by hand.
  if (this->ota_scooter_phase_ == 0xFF && this->state() == espbt::ClientState::ESTABLISHED &&
      this->find_char_(CharId::OTA_CONTROL) != nullptr) {
    if (this->ota_status_retry_ms_ == 0 || now - this->ota_status_retry_ms_ >= 15000) {
      this->ota_status_retry_ms_ = now;
      const uint8_t d[] = {0x05};  // STATUS_REQ — read-only
      this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
    }
  }

  // "auto" yield expired → re-evaluate the link. Must go through apply_link_state_ (not a bare
  // set_enabled(true)) so a pairing block is honoured: otherwise the yield would reconnect every
  // 20 s into the same failed pairing, spamming the scooter and flapping "Pairing Required".
  if (this->yield_until_ms_ != 0 && now >= this->yield_until_ms_) {
    this->yield_until_ms_ = 0;
    this->apply_link_state_();
  }

  // Connect-on-demand state machine (runs while connecting too).
  if (this->ondemand_active_) {
    if (!this->ondemand_ran_ && this->state() == espbt::ClientState::ESTABLISHED &&
        this->ondemand_run_at_ms_ != 0 && now >= this->ondemand_run_at_ms_) {
      ESP_LOGI(TAG, "connect-on-demand: running %u queued action(s)", (unsigned) this->ondemand_actions_.size());
      auto actions = std::move(this->ondemand_actions_);
      this->ondemand_actions_.clear();
      for (auto &a : actions)
        a();
      this->ondemand_ran_ = true;
      this->ondemand_disc_at_ms_ = now + ONDEMAND_RESP_MS;
    } else if (this->ondemand_ran_) {
      // Hold the link until the response window passes and no command reply is still
      // pending, capped so a missing reply can't keep us connected forever.
      bool window_over = now >= this->ondemand_disc_at_ms_ && !this->cmd_collecting_;
      bool capped = now >= this->ondemand_disc_at_ms_ + ONDEMAND_RESP_CAP_MS;
      if (window_over || capped) {
        ESP_LOGI(TAG, "connect-on-demand: done, restoring link mode");
        this->end_ondemand_();
      }
    } else if (!this->ondemand_ran_ && now >= this->ondemand_deadline_ms_) {
      ESP_LOGW(TAG, "connect-on-demand: timed out before connecting");
      this->end_ondemand_();
    }
  }

  if (this->state() != espbt::ClientState::ESTABLISHED)
    return;

  // Paced extended-command query queue (on connect / refresh).
  if (!this->pending_queries_.empty() && now >= this->next_query_ms_) {
    this->send_ext_query_(this->pending_queries_.front());
    this->pending_queries_.erase(this->pending_queries_.begin());
    this->next_query_ms_ = now + 1500;
  }

  // Extended-command collection timeout (fallback for a missing terminator).
  if (this->cmd_collecting_ && now - this->cmd_started_ms_ > 20000)
    this->cmd_collecting_ = false;

  // RSSI poll.
  if (this->rssi_ != nullptr && now - this->last_rssi_ms_ >= 60000) {
    this->last_rssi_ms_ = now;
    esp_ble_gap_read_rssi(this->get_remote_bda());
  }

  // Serialize characteristic reads: Bluedroid processes one GATT read at a time, so we
  // issue the next only after the previous READ_CHAR_EVT (or a 2 s safety timeout).
  // A forced (on-connect / refresh) read stays "due" every 1.2 s until it actually
  // succeeds, so a dropped read is retried instead of waiting a whole interval; give up
  // after 8 attempts so a quirky characteristic can't block the rotation forever.
  if (this->read_in_flight_ && now - this->read_issued_ms_ > 2000)
    this->read_in_flight_ = false;
  if (!this->read_in_flight_ && now - this->last_read_issue_ms_ >= 20) {
    for (auto &e : this->chars_) {
      if (e.interval_ms == 0 || e.handle == 0)
        continue;
      bool due = e.force ? (now - e.last_ms >= 1200) : (now - e.last_ms >= e.interval_ms);
      if (due) {
        esp_ble_gattc_read_char(this->get_gattc_if(), this->get_conn_id(), e.handle, ESP_GATT_AUTH_REQ_NONE);
        e.last_ms = now;
        if (e.force && ++e.tries >= 8)
          e.force = false;
        this->last_read_issue_ms_ = now;
        this->read_in_flight_ = true;
        this->read_issued_ms_ = now;
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// BLE events
// ---------------------------------------------------------------------------
bool LibrescootBleClient::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  // Let the base class run the connection/discovery state machine first.
  if (!BLEClientBase::gattc_event_handler(event, gattc_if, param))
    return false;

  switch (event) {
    case ESP_GATTC_CFG_MTU_EVT:
      this->att_mtu_ = param->cfg_mtu.mtu;
      ESP_LOGI(TAG, "Negotiated ATT_MTU = %d", param->cfg_mtu.mtu);
      break;
    case ESP_GATTC_CONGEST_EVT:
      this->ota_congested_ = param->congest.congested;  // pause/resume OTA_DATA writes
      if (this->ota_state_ == OtaState::STREAMING)
        ESP_LOGD("ota", "congest %s", param->congest.congested ? "ON" : "off");
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      for (auto &e : this->chars_) {
        auto *chr = this->get_characteristic(e.service, e.chr);
        if (chr == nullptr) {
          e.handle = 0;
          continue;
        }
        e.handle = chr->handle;
        e.props = chr->properties;
        if (e.notify)
          esp_ble_gattc_register_for_notify(this->get_gattc_if(), this->get_remote_bda(), chr->handle);
      }
      auto *ota = this->find_char_(CharId::OTA_CONTROL);
      this->ota_present_ = ota != nullptr && ota->handle != 0;
      ESP_LOGI(TAG, "Service discovery complete. OTA service %s", this->ota_present_ ? "present" : "absent");
      this->on_connected_();
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      this->read_in_flight_ = false;  // allow the next serialized read
      if (param->read.status != ESP_GATT_OK)
        break;
      for (auto &e : this->chars_) {
        if (e.handle != 0 && e.handle == param->read.handle) {
          e.force = false;  // this characteristic is confirmed read
          e.tries = 0;
          this->handle_char_value_(e.id, param->read.value, param->read.value_len);
          break;
        }
      }
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      for (auto &e : this->chars_) {
        if (e.handle != 0 && e.handle == param->notify.handle) {
          this->handle_char_value_(e.id, param->notify.value, param->notify.value_len);
          break;
        }
      }
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT: {
      for (auto &e : this->chars_)
        e.handle = 0;
      this->pending_queries_.clear();
      this->ota_handle_disconnect_();  // resume a transfer, or keep waiting through an install
      this->mark_unknown_();
      // "auto" link mode: release the link for a grace window on every disconnect so a
      // phone (or any other central) can win the reconnect race and take the scooter.
      // "always" reconnects immediately (base auto_connect); OTA always pins the link up.
      if (this->link_mode_str_ == "auto" && !this->ota_active_ && !this->pairing_armed_() &&
          this->get_address() != 0) {
        this->set_enabled(false);
        this->yield_until_ms_ = millis() + LINK_YIELD_MS;
        ESP_LOGI(TAG, "auto: releasing link for %u s so another app can connect", LINK_YIELD_MS / 1000);
      }
      break;
    }
    default:
      break;
  }
  return true;
}

void LibrescootBleClient::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  BLEClientBase::gap_event_handler(event, param);
  switch (event) {
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      // A PASSKEY_REQ means a FRESH pairing is starting (no valid bond) — a valid bond re-encrypts
      // silently without this event. Only proceed when pairing was armed deliberately.
      if (this->check_addr(param->ble_security.auth_cmpl.bd_addr)) {
        if (this->pairing_armed_()) {
          this->pair_arm_until_ms_ = millis() + PAIR_ARM_WINDOW_MS;  // keep armed while actively pairing
          ESP_LOGW(TAG, "!!! PIN REQUIRED !!! enter the passkey shown on the scooter dashboard");
          ESP_LOGW(TAG, "    (HA pairing Repair, or the '<node>_passkey_reply' action).");
          if (this->passkey_required_ != nullptr)
            this->passkey_required_->publish_state(true);
        } else {
          // Unrequested (lost/stale bond) — refuse and release the link so the scooter isn't
          // spammed with pairing prompts. The user re-bonds intentionally via 'Pair Scooter'.
          esp_ble_passkey_reply(param->ble_security.auth_cmpl.bd_addr, false, 0);
          this->pairing_blocked_ = true;
          if (this->passkey_required_ != nullptr)
            this->passkey_required_->publish_state(true);
          ESP_LOGW(TAG, "Scooter wants to (re)pair but pairing is NOT armed — refused, link "
                        "released. Press 'Pair Scooter' (or the HA pairing Repair) to bond.");
          this->set_enabled(false);
        }
      }
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (this->check_addr(param->ble_security.auth_cmpl.bd_addr)) {
        if (param->ble_security.auth_cmpl.success) {
          this->pair_arm_until_ms_ = 0;  // bonded — disarm
          this->pairing_blocked_ = false;
          if (this->passkey_required_ != nullptr)
            this->passkey_required_->publish_state(false);
        } else if (this->pairing_armed_()) {
          // A deliberate pairing attempt failed (e.g. a wrong code, or a session timeout). Stay
          // armed for the rest of the window so the next PASSKEY_REQ is still honoured and the user
          // can simply try again — do NOT block. (Disarming here was the bug that broke pairing.)
          ESP_LOGW(TAG, "Pairing attempt failed (reason %d) — still armed, try the passkey again.",
                   param->ble_security.auth_cmpl.fail_reason);
        } else {
          // Unrequested pairing/auth FAILED (lost or stale bond — the scooter no longer knows us).
          // Stop the retry loop: release the link so the scooter isn't spammed with pairing/auth
          // attempts. The user re-bonds deliberately with 'Pair Scooter'.
          ESP_LOGW(TAG, "Unrequested pairing failed (reason %d) — link released so the scooter isn't "
                        "spammed. Press 'Pair Scooter' (or the HA pairing Repair) to (re)bond.",
                   param->ble_security.auth_cmpl.fail_reason);
          this->pairing_blocked_ = true;
          this->set_enabled(false);
        }
      }
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      if (this->check_addr(param->ble_security.auth_cmpl.bd_addr))
        ESP_LOGI(TAG, "Scooter displays passkey: %06lu", (unsigned long) param->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
      if (this->rssi_ != nullptr && param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS)
        this->rssi_->publish_state(param->read_rssi_cmpl.rssi);
      break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      ESP_LOGI(TAG, "conn params updated: status=%d interval=%.1f ms latency=%d timeout=%d ms",
               param->update_conn_params.status, param->update_conn_params.conn_int * 1.25f,
               param->update_conn_params.latency, param->update_conn_params.timeout * 10);
      break;
    default:
      break;
  }
}

bool LibrescootBleClient::parse_device(const espbt::ESPBTDevice &device) {
  if (device.address_uint64() == this->get_address()) {
    this->last_adv_ms_ = millis();
    // Publish the advertisement RSSI (throttled) so the signal to the CONFIGURED scooter is visible
    // even while disconnected — the connection-RSSI read only works once connected. Lets the pairing
    // dialog show whether the scooter is actually in range and how strong.
    if (this->rssi_ != nullptr && millis() - this->last_rssi_pub_ms_ > 3000) {
      this->last_rssi_pub_ms_ = millis();
      this->rssi_->publish_state(device.get_rssi());
    }
  }
  // Scooter discovery: while in scan mode, collect nearby adverts whose local name matches the
  // filter and POST them to the HA integration (see publish_discovered_). Runs before the
  // enabled_ gate so it works in scan mode (client disabled, scanner on).
  if (this->link_mode_str_ == "scan" && icontains(device.get_name(), this->scooter_filter_)) {
    uint64_t mac = device.address_uint64();
    int rssi = device.get_rssi();
    auto it = this->discovered_.find(mac);
    bool changed = false;
    if (it == this->discovered_.end()) {
      if (this->discovered_.size() < 12) {
        this->discovered_[mac] = rssi;
        changed = true;
      }
    } else if (rssi > it->second + 4 || rssi < it->second - 4) {
      it->second = rssi;
      changed = true;
    }
    uint32_t now = millis();
    if (changed && now - this->discovered_pub_ms_ > 1500) {
      this->discovered_pub_ms_ = now;
      this->publish_discovered_();
    }
  }
  if (!this->enabled_)
    return false;
  return BLEClientBase::parse_device(device);
}

void LibrescootBleClient::publish_discovered_() {
  auto fmt = [](uint64_t m, int rssi, bool own) {
    char e[32];
    snprintf(e, sizeof(e), "%02X:%02X:%02X:%02X:%02X:%02X@%d%s", (uint8_t) (m >> 40), (uint8_t) (m >> 32),
             (uint8_t) (m >> 24), (uint8_t) (m >> 16), (uint8_t) (m >> 8), (uint8_t) m, rssi, own ? "*" : "");
    return std::string(e);
  };
  std::string out;
  // Always list this ESP's own configured scooter first, marked with '*' — so the HA picker shows
  // it even when a fresh scan didn't re-hear its advert (rssi -127 = not heard this pass).
  uint64_t own = this->get_address();
  if (own != 0) {
    auto it = this->discovered_.find(own);
    out += fmt(own, it != this->discovered_.end() ? it->second : -127, true);
  }
  std::vector<std::pair<uint64_t, int>> v(this->discovered_.begin(), this->discovered_.end());
  std::sort(v.begin(), v.end(), [](const std::pair<uint64_t, int> &a, const std::pair<uint64_t, int> &b) {
    return a.second > b.second;  // strongest signal first (likely the nearest)
  });
  for (auto &p : v) {
    if (p.first == own)
      continue;  // already listed
    std::string e = fmt(p.first, p.second, false);
    if (out.size() + e.size() + 1 > 480)  // keep the POST body well under the listener's 512 cap
      break;
    if (!out.empty())
      out += ",";
    out += e;
  }
  this->disc_body_ = out;
  // POST to the HA integration's listener while scanning (that's when its picker reads it).
  if (this->link_mode_str_ == "scan" && !this->disc_post_running_) {
    this->disc_post_running_ = true;
    if (xTaskCreate(&LibrescootBleClient::post_discovered_task_, "librescoot_scan", 4096, this, 4, nullptr) != pdPASS)
      this->disc_post_running_ = false;
  }
}

void LibrescootBleClient::post_discovered_task_(void *arg) {
  static_cast<LibrescootBleClient *>(arg)->post_discovered_();
  vTaskDelete(nullptr);
}

void LibrescootBleClient::post_discovered_() {
  // Turn the OTA relay URL (http://host:port/ota/<secret>) into the scan endpoint (…/scan/<secret>).
  std::string url = this->ota_source_url_;
  size_t p = url.find("/ota/");
  if (url.rfind("http://", 0) == 0 && p != std::string::npos) {
    url.replace(p, 5, "/scan/");
    std::string body = this->disc_body_;  // snapshot
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 3000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c != nullptr) {
      esp_http_client_set_post_field(c, body.c_str(), body.size());
      esp_http_client_perform(c);  // fire-and-forget; the integration stores the latest
      esp_http_client_cleanup(c);
    }
  }
  this->disc_post_running_ = false;
}

void LibrescootBleClient::on_connected_() {
  // Force an immediate first read of every polled characteristic. Reset the retry
  // counter and the last-read timestamp too: without this a characteristic that gave
  // up during an earlier (flappy) link — or one the refresh button targets after a
  // give-up — could never be re-read, and long-interval reads (versions, cycle counts)
  // would stay empty until their full interval elapsed.
  for (auto &e : this->chars_) {
    e.force = true;
    e.tries = 0;
    e.last_ms = 0;
  }
  this->last_read_issue_ms_ = 0;
  this->read_in_flight_ = false;

  // Ask the scooter for its current OTA status on every connect. It doesn't reliably notify a
  // "pending reboot" on its own, and we must know it to suppress a bogus "update available"
  // after the ESP reboots while the scooter is still pending its own reboot.
  if (this->find_char_(CharId::OTA_CONTROL) != nullptr) {
    const uint8_t d[] = {0x05};  // STATUS_REQ
    this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
    this->ota_status_retry_ms_ = millis();  // first unknown-phase retry waits 15 s after this one
  }

  // Queue the read-only on-connect queries (only those with a target entity).
  this->pending_queries_.clear();

  // Connect-on-demand session: skip the query fetch so the command channel stays free for
  // the queued action; run the action shortly after discovery settles.
  if (this->ondemand_active_) {
    this->ondemand_run_at_ms_ = millis() + 800;
    return;
  }

  if (this->keycard_count_ != nullptr)
    this->pending_queries_.push_back("keycard:count");
  if (this->sw_dbc_ != nullptr || this->dbc_update_ != nullptr || this->mdb_update_ != nullptr)
    this->pending_queries_.push_back("status:version:dbc");
  if (this->maps_available_ != nullptr)
    this->pending_queries_.push_back("status:maps-available");
  if (this->nav_available_ != nullptr)
    this->pending_queries_.push_back("status:navigation-available");
  if (this->apn_text_ != nullptr)
    this->pending_queries_.push_back("get:cellular.apn");
  if (this->pm_sched_hib_ != nullptr)
    this->pending_queries_.push_back("get:pm.scheduled-hibernate-enabled");
  if (this->pm_cron_text_ != nullptr)
    this->pending_queries_.push_back("get:pm.scheduled-hibernate-cron");
  if (this->pm_duration_text_ != nullptr)
    this->pending_queries_.push_back("get:pm.scheduled-hibernate-duration");
  this->next_query_ms_ = millis() + 3000;
}

void LibrescootBleClient::refresh_() {
  this->on_connected_();
  // Also poke the OTA status (it's notify-only, so a plain re-poll wouldn't refresh it).
  if (this->find_char_(CharId::OTA_CONTROL) != nullptr) {
    const uint8_t d[] = {0x05};  // STATUS_REQ
    this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
  }
}

// ---------------------------------------------------------------------------
// value parsing
// ---------------------------------------------------------------------------
void LibrescootBleClient::handle_char_value_(CharId id, uint8_t *v, uint16_t len) {
  switch (id) {
    case CharId::BAT1_PRESENT:
      this->present1_ = len > 0 && v[0] != 0;
      if (this->bat1_present_ != nullptr)
        this->bat1_present_->publish_state(this->present1_);
      break;
    case CharId::BAT2_PRESENT:
      this->present2_ = len > 0 && v[0] != 0;
      if (this->bat2_present_ != nullptr)
        this->bat2_present_->publish_state(this->present2_);
      break;
    case CharId::BAT1_SOC:
      if (this->bat1_soc_ != nullptr)
        this->bat1_soc_->publish_state(this->present1_ && len > 0 ? (float) v[0] : NAN);
      break;
    case CharId::BAT2_SOC:
      if (this->bat2_soc_ != nullptr)
        this->bat2_soc_->publish_state(this->present2_ && len > 0 ? (float) v[0] : NAN);
      break;
    case CharId::BAT1_CYCLES:
      if (this->bat1_cycles_ != nullptr)
        this->bat1_cycles_->publish_state(this->present1_ && len > 0 ? (float) v[0] : NAN);
      break;
    case CharId::BAT2_CYCLES:
      if (this->bat2_cycles_ != nullptr)
        this->bat2_cycles_->publish_state(this->present2_ && len > 0 ? (float) v[0] : NAN);
      break;
    case CharId::BAT1_STATE:
      if (this->bat1_state_ != nullptr)
        this->bat1_state_->publish_state(this->present1_ ? clean_str_(v, len) : "unknown");
      break;
    case CharId::BAT2_STATE:
      if (this->bat2_state_ != nullptr)
        this->bat2_state_->publish_state(this->present2_ ? clean_str_(v, len) : "unknown");
      break;
    case CharId::AUX_VOLTAGE:
      if (this->aux_voltage_ != nullptr)
        this->aux_voltage_->publish_state(len >= 2 ? (float) ((uint16_t) v[0] | ((uint16_t) v[1] << 8)) / 1000.0f
                                                   : NAN);
      break;
    case CharId::AUX_LEVEL:
      if (this->aux_level_ != nullptr)
        this->aux_level_->publish_state(len > 0 ? (float) v[0] : NAN);
      break;
    case CharId::CBB_LEVEL:
      if (this->cbb_level_ != nullptr)
        this->cbb_level_->publish_state(len > 0 ? (float) v[0] : NAN);
      break;
    case CharId::CBB_REMAINING:
      if (this->cbb_remaining_ != nullptr)
        this->cbb_remaining_->publish_state(len >= 4 ? (float) u32le(v) / 1000000.0f : NAN);
      break;
    case CharId::CBB_FULL:
      if (this->cbb_full_ != nullptr)
        this->cbb_full_->publish_state(len >= 4 ? (float) u32le(v) / 1000000.0f : NAN);
      break;
    case CharId::CBB_CELL:
      if (this->cbb_cell_ != nullptr)
        this->cbb_cell_->publish_state(len >= 4 ? (float) u32le(v) / 1000000.0f : NAN);
      break;
    case CharId::ODOMETER:
      if (this->odometer_ != nullptr)
        this->odometer_->publish_state(len >= 4 ? (float) u32le(v) / 1000.0f : NAN);
      break;
    case CharId::NAV_ACTIVE:
      if (this->nav_active_ != nullptr)
        this->nav_active_->publish_state(len > 0 && v[0] != 0);
      break;
    case CharId::UMS_STATUS: {
      bool on = len > 0 && v[0] != 0;
      if (this->ums_status_ != nullptr)
        this->ums_status_->publish_state(on);
      if (this->usb_mode_ != nullptr)
        this->usb_mode_->publish_state(on ? "Mass Storage" : "Normal");
      break;
    }
    case CharId::POWER_MUX:
      if (this->power_mux_ != nullptr)
        this->power_mux_->publish_state(clean_str_(v, len));
      break;
    case CharId::SEATBOX:
      if (this->seatbox_ != nullptr)
        this->seatbox_->publish_state(clean_str_(v, len));
      break;
    case CharId::HANDLEBAR:
      if (this->handlebar_ != nullptr)
        this->handlebar_->publish_state(clean_str_(v, len));
      break;
    case CharId::POWER_STATE: {
      std::string ps = clean_str_(v, len);
      if (this->power_state_ != nullptr)
        this->power_state_->publish_state(ps);
      // Scooter (re)booting → re-read everything 30 s later so we get fresh data (and confirm a
      // new firmware version after an OTA). Applies always, not just during an update.
      if (ps == "booting" && this->boot_reread_at_ms_ == 0)
        this->boot_reread_at_ms_ = millis() + 30000;
      break;
    }
    case CharId::SW_MDB:
      this->set_mdb_version_(clean_str_(v, len));
      break;
    case CharId::SW_NRF:
      if (this->sw_nrf_ != nullptr)
        this->sw_nrf_->publish_state(clean_str_(v, len));
      break;
    case CharId::AUX_CHARGE:
      if (this->aux_charge_ != nullptr)
        this->aux_charge_->publish_state(clean_str_(v, len));
      break;
    case CharId::CBB_CHARGE:
      if (this->cbb_charge_ != nullptr)
        this->cbb_charge_->publish_state(clean_str_(v, len));
      break;
    case CharId::STATUS: {
      std::string s = clean_str_(v, len);
      if (this->status_ != nullptr)
        this->status_->publish_state(s);
      if (this->scooter_lock_ != nullptr && millis() >= this->lock_suppress_until_) {
        // Reflect the lock from the operating state only: "parked" and "ready-to-drive" are
        // UNLOCKED; "hibernating"/"booting"/"stand-by"/"hop-on" (and anything else, incl. unknown)
        // are LOCKED. Handlebar lock is NOT a reliable proxy (it can be open in stand-by).
        bool unlocked = (s == "ready-to-drive" || s == "parked");
        this->scooter_lock_->publish_state(unlocked ? lock::LOCK_STATE_UNLOCKED
                                                     : lock::LOCK_STATE_LOCKED);
      }
      break;
    }
    case CharId::CMD_RESPONSE: {
      std::string line;
      for (uint16_t i = 0; i < len; i++) {
        char c = (char) v[i];
        if (c == 0)
          break;
        if ((uint8_t) c >= 0x20)
          line += c;
      }
      if (line.empty())
        break;
      if (this->cmd_response_ != nullptr)
        this->cmd_response_->publish_state(line);
      this->parse_cmd_response_(line);
      break;
    }
    case CharId::OTA_STATUS:
      this->parse_ota_status_(v, len);
      break;
    default:
      break;
  }
}

void LibrescootBleClient::parse_cmd_response_(const std::string &line) {
  // Typed responses — parsed always so on-connect queries and manual commands both work.
  if (line.rfind("status:version:dbc:", 0) == 0) {
    this->set_dbc_version_(line.substr(19));
  } else if (line.rfind("keycard:count:", 0) == 0) {
    if (this->keycard_count_ != nullptr)
      this->keycard_count_->publish_state(line.substr(14));
  } else if (line.rfind("status:maps-available:", 0) == 0) {
    if (this->maps_available_ != nullptr)
      this->maps_available_->publish_state(line.find(":true") != std::string::npos);
  } else if (line.rfind("status:navigation-available:", 0) == 0) {
    if (this->nav_available_ != nullptr)
      this->nav_available_->publish_state(line.find(":true") != std::string::npos);
  } else if (line.rfind("get:cellular.apn:", 0) == 0) {
    if (this->apn_text_ != nullptr)
      this->apn_text_->publish_state(line.substr(17));
  } else if (line.rfind("get:pm.scheduled-hibernate-enabled:", 0) == 0) {
    if (this->pm_sched_hib_ != nullptr)
      this->pm_sched_hib_->publish_state(line.find(":true") != std::string::npos);
  } else if (line.rfind("get:pm.scheduled-hibernate-cron:", 0) == 0) {
    if (this->pm_cron_text_ != nullptr)
      this->pm_cron_text_->publish_state(line.substr(32));
  } else if (line.rfind("get:pm.scheduled-hibernate-duration:", 0) == 0) {
    if (this->pm_duration_text_ != nullptr)
      this->pm_duration_text_->publish_state(line.substr(36));
  }

  // Aggregation + terminator detection only while collecting.
  if (!this->cmd_collecting_)
    return;
  if (!this->cmd_log_.empty())
    this->cmd_log_ += "\n";
  this->cmd_log_ += line;
  std::string shown = this->cmd_log_.size() > 255 ? (this->cmd_log_.substr(0, 249) + "...") : this->cmd_log_;
  if (this->cmd_last_response_ != nullptr)
    this->cmd_last_response_->publish_state(shown);

  size_t cpos = line.find(":count:");
  if (cpos != std::string::npos) {
    this->cmd_expected_lines_ = atoi(line.c_str() + cpos + 7);
    this->cmd_lines_seen_ = 0;
  } else if (this->cmd_expected_lines_ > 0) {
    this->cmd_lines_seen_++;
    if (this->cmd_lines_seen_ >= this->cmd_expected_lines_)
      this->cmd_done_ = true;
  } else {
    this->cmd_done_ = true;
  }
  if (this->cmd_done_)
    this->cmd_collecting_ = false;
}

void LibrescootBleClient::parse_ota_status_(uint8_t *x, uint16_t len) {
  uint8_t op0 = len > 0 ? x[0] : 0xFF;
  // Routine ACKs (0x82) during streaming are throttled by file size; milestones always show.
  this->ota_report_now_ = (op0 != 0x82) || this->ota_report_due_();
  if (this->ota_report_now_) {
    std::string h;
    char b[4];
    for (uint16_t i = 0; i < len; i++) {
      snprintf(b, sizeof(b), "%02X ", x[i]);
      h += b;
    }
    ESP_LOGD("ota", "OTA_STATUS raw: %s", h.c_str());
  }
  std::string s = "unknown";
  if (len > 0) {
    uint8_t op = x[0];
    if (op == 0x84 && len >= 3) {  // INSTALL_PROGRESS
      const char *ph = "?";
      switch (x[1]) {
        case 0x00: ph = "Verifying"; break;
        case 0x01: ph = "Installing"; break;
        case 0x02: ph = "Pending reboot"; break;
        case 0x03: ph = "Rebooting"; break;
        case 0x04: ph = "Success"; break;
        case 0x05: ph = "Failed"; break;
        case 0x06: ph = "Idle"; break;
      }
      s = ph;
      // The scooter itself is mid-install / pending-reboot (survives an ESP reboot): while it
      // is, don't offer an "update available" — it's already on its way. Cleared on a terminal
      // phase, on a timeout, or when the versions come in sync.
      if (x[1] != this->ota_scooter_phase_) {  // re-evaluate only on a phase change (avoid spam)
        this->ota_scooter_phase_ = x[1];
        if (x[1] != 0x02) {  // left pending-reboot → clear the stuck-reboot watchdog
          this->ota_pending_since_ms_ = 0;
          this->ota_pending_last_req_ms_ = 0;
          if (this->reboot_problem_) {
            this->reboot_problem_ = false;
            if (this->reboot_required_ != nullptr)
              this->reboot_required_->publish_state(false);
          }
        }
        this->refresh_update_availability_();
      }
      if (x[1] == 0x01) {
        char p[8];
        snprintf(p, sizeof(p), " %u%%", x[2]);
        s += p;
      }
      if (len >= 4 && x[3] > 0 && len >= (uint16_t) (4 + x[3])) {
        s += ": ";
        s.append(reinterpret_cast<const char *>(&x[4]), x[3]);
      }
    } else if (op == 0x81 && len >= 2) {  // START_ACK
      const char *st = "?";
      switch (x[1]) {
        case 0x00: st = "resuming"; break;
        case 0x01: st = "fresh transfer"; break;
        case 0x10: st = "no staging space"; break;
        case 0x11: st = "busy"; break;
        case 0x12: st = "bad parameters"; break;
        case 0x13: st = "install in progress"; break;
      }
      s = std::string("Start ack: ") + st;
    } else if (op == 0x82 && len >= 6) {  // ACK
      uint32_t a = u32le(&x[2]);
      char buf[48];
      snprintf(buf, sizeof(buf), "Ack %u bytes%s", (unsigned) a, (x[1] & 0x01) ? " (rewind)" : "");
      s = buf;
    } else if (op == 0x83 && len >= 2) {  // COMPLETE_ACK
      const char *st = "?";
      switch (x[1]) {
        case 0x00: st = "verified, queued"; break;
        case 0x01: st = "SHA-256 mismatch"; break;
        case 0x02: st = "size mismatch"; break;
        case 0x03: st = "queueing failed"; break;
      }
      s = std::string("Complete: ") + st;
    } else if (op == 0x85) {
      s = "Aborted";
    } else if (op == 0x86 && len >= 2) {  // ERROR
      const char *c = "?";
      switch (x[1]) {
        case 0x01: c = "iMX suspended"; break;
        case 0x02: c = "USOCK overflow"; break;
        case 0x03: c = "internal error"; break;
        case 0x04: c = "staging write failed"; break;
        case 0x05: c = "no staging space"; break;
        case 0x06: c = "no active session"; break;
      }
      s = std::string("Error: ") + c;
      if (len >= 3 && x[2] > 0 && len >= (uint16_t) (3 + x[2])) {
        s += " - ";
        s.append(reinterpret_cast<const char *>(&x[3]), x[2]);
      }
    }
  }
  // During streaming, ota_progress_() owns the OTA Status text ("Transferring X%"); don't
  // also spam it with a per-ACK "Ack N bytes". Milestones (START_ACK/COMPLETE/ERROR/…) publish.
  if (this->ota_status_ != nullptr && op0 != 0x82)
    this->ota_status_->publish_state(s);

  bool active = false;
  if (len > 0) {
    uint8_t o = x[0];
    if (o == 0x81 && len >= 2)
      active = (x[1] == 0x00 || x[1] == 0x01);
    else if (o == 0x82)
      active = true;
    else if (o == 0x83 && len >= 2)
      active = (x[1] == 0x00);
    else if (o == 0x84 && len >= 2)
      active = (x[1] <= 0x03);
  }
  if (active != this->ota_active_) {
    this->ota_active_ = active;
    this->apply_link_state_();
  }

  // Drive the transfer state machine (no-op unless a transfer is in progress).
  this->ota_handle_status_(x, len);
}

void LibrescootBleClient::mark_unknown_() {
  sensor::Sensor *sensors[] = {bat1_soc_,      bat1_cycles_,   bat2_soc_, bat2_cycles_, aux_voltage_, aux_level_,
                               cbb_level_,     cbb_remaining_, cbb_full_, cbb_cell_,    odometer_,    rssi_};
  for (auto *s : sensors)
    if (s != nullptr)
      s->publish_state(NAN);

  binary_sensor::BinarySensor *bins[] = {bat1_present_, bat2_present_, nav_active_,
                                         ums_status_,   maps_available_, nav_available_};
  for (auto *b : bins)
    if (b != nullptr)
      b->invalidate_state();

  text_sensor::TextSensor *texts[] = {status_,     seatbox_,   handlebar_,  power_state_, power_mux_,
                                      bat1_state_, bat2_state_, sw_mdb_,     sw_nrf_,      sw_dbc_,
                                      aux_charge_, cbb_charge_, keycard_count_};
  for (auto *t : texts)
    if (t != nullptr)
      t->publish_state("unknown");

  this->present1_ = false;
  this->present2_ = false;
}

// ---------------------------------------------------------------------------
// writes
// ---------------------------------------------------------------------------
bool LibrescootBleClient::write_raw_(CharId id, const uint8_t *data, size_t len) {
  if (this->state() != espbt::ClientState::ESTABLISHED) {
    // Not connected: copy the payload and replay it once the link comes up.
    std::vector<uint8_t> buf(data, data + len);
    this->queue_ondemand_([this, id, buf]() { this->write_now_(id, buf.data(), buf.size()); });
    return true;
  }
  return this->write_now_(id, data, len);
}

bool LibrescootBleClient::write_now_(CharId id, const uint8_t *data, size_t len) {
  CharEntry *e = this->find_char_(id);
  if (e == nullptr || e->handle == 0) {
    ESP_LOGW(TAG, "Write target not available (characteristic absent or not discovered)");
    return false;
  }
  esp_gatt_write_type_t wt =
      (e->props & ESP_GATT_CHAR_PROP_BIT_WRITE) ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
  esp_err_t err = esp_ble_gattc_write_char(this->get_gattc_if(), this->get_conn_id(), e->handle, len,
                                           const_cast<uint8_t *>(data), wt, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write_char failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void LibrescootBleClient::queue_ondemand_(std::function<void()> action) {
  this->ondemand_actions_.push_back(std::move(action));
  if (this->ondemand_active_)
    return;  // a session is already coming up; the action will run with it
  if (this->get_address() == 0) {
    ESP_LOGW(TAG, "connect-on-demand: no MAC configured, cannot run action");
    this->ondemand_actions_.clear();
    return;
  }
  this->ondemand_active_ = true;
  this->ondemand_ran_ = false;
  this->ondemand_run_at_ms_ = 0;
  this->ondemand_deadline_ms_ = millis() + ONDEMAND_CONNECT_TIMEOUT_MS;
  this->yield_until_ms_ = 0;  // cancel any pending auto-yield
  ESP_LOGI(TAG, "connect-on-demand: bringing the link up to run a queued action");
  this->set_enabled(true);
}

void LibrescootBleClient::end_ondemand_() {
  this->ondemand_active_ = false;
  this->ondemand_ran_ = false;
  this->ondemand_run_at_ms_ = 0;
  this->ondemand_actions_.clear();
  this->apply_link_state_();  // restore the configured mode (disconnect/scan releases the link)
}

void LibrescootBleClient::write_str_(CharId id, const std::string &s) {
  this->write_raw_(id, reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

void LibrescootBleClient::send_ext_query_(const std::string &q) {
  std::string c = q;
  c.push_back('\0');
  this->write_raw_(CharId::EXT_CMD, reinterpret_cast<const uint8_t *>(c.data()), c.size());
}

void LibrescootBleClient::run_command_(const std::string &cmd) {
  this->cmd_log_.clear();
  this->cmd_collecting_ = true;
  this->cmd_done_ = false;
  this->cmd_expected_lines_ = 0;
  this->cmd_lines_seen_ = 0;
  this->cmd_started_ms_ = millis();
  ESP_LOGI(TAG, "TX ext cmd: %s", cmd.c_str());
  this->send_ext_query_(cmd);
}

// ---------------------------------------------------------------------------
// link mode
// ---------------------------------------------------------------------------
void LibrescootBleClient::apply_link_state_() {
  this->yield_until_ms_ = 0;  // an explicit re-evaluation cancels any pending auto-yield
  bool has_mac = this->get_address() != 0;
  // pairing_blocked_: the scooter wanted to (re)pair but pairing wasn't armed — stay disconnected
  // so we don't hammer it with pairing requests. Cleared by the Pair action.
  // Continuous-link modes keep the connection up; "interval" connects only during its refresh
  // dwell; "scan"/"disconnect" never auto-connect. OTA and an active pairing always pin it up.
  bool continuous = this->link_mode_str_ == "auto" || this->link_mode_str_ == "always";
  bool want = has_mac && !this->pairing_blocked_ &&
              (this->ota_active_ || this->interval_refreshing_ || continuous);
  this->set_enabled(want);
}

void LibrescootBleClient::apply_ota_source_(const std::string &mode, bool persist) {
  this->ota_source_mode_ = (mode == "github") ? "github" : "relay";
  if (this->ota_source_mode_ == "github") {
    // github mode owns the URL — point it straight at the GitHub download base.
    this->ota_source_url_ = this->github_base_();
    if (this->ota_source_url_text_ != nullptr)
      this->ota_source_url_text_->publish_state(this->ota_source_url_);
  }
  // relay mode: leave ota_source_url_ as the HA integration set it (persisted / re-pushed).
  if (this->ota_source_select_ != nullptr)
    this->ota_source_select_->publish_state(this->ota_source_mode_ == "github" ? "direct GitHub" : "HA relay");
  if (persist) {
    uint8_t v = (this->ota_source_mode_ == "github") ? 1 : 0;
    this->ota_source_pref_.save(&v);
  }
  this->hi_next_check_ms_ = millis();  // re-evaluate relay reachability for the new source
  ESP_LOGI(TAG, "OTA byte source = %s (%s)", this->ota_source_mode_.c_str(), this->ota_source_url_.c_str());
}

void LibrescootBleClient::save_link_mode_() {
  uint8_t idx = 2;
  if (this->link_mode_str_ == "disconnect")
    idx = 0;
  else if (this->link_mode_str_ == "scan")
    idx = 1;
  else if (this->link_mode_str_ == "auto")
    idx = 2;
  else if (this->link_mode_str_ == "always")
    idx = 3;
  else if (this->link_mode_str_ == "interval")
    idx = 4;
  this->link_pref_.save(&idx);
}

// ---------------------------------------------------------------------------
// GitHub update check
// ---------------------------------------------------------------------------
void LibrescootBleClient::reflect_channel_(const std::string &mdb_version) {
  this->channel_ = channel_of(mdb_version);
  if (this->ota_channel_ != nullptr &&
      (this->channel_ == "nightly" || this->channel_ == "testing" || this->channel_ == "stable"))
    this->ota_channel_->publish_state(this->channel_);
}

void LibrescootBleClient::set_mdb_version_(const std::string &v) {
  this->mdb_version_ = v;
  this->reflect_channel_(v);
  if (this->sw_mdb_ != nullptr)
    this->sw_mdb_->publish_state(v);
  this->publish_current_();
  // When the installed MDB version first becomes known (boot) or changes (after an install),
  // re-run the update check so the changelog is aggregated for the correct installed→latest range.
  if (!v.empty() && !ieq(v, "unknown") && !ieq(version_to_tag(v), this->gh_current_tag_) &&
      (this->mdb_update_ != nullptr || this->dbc_update_ != nullptr))
    this->next_check_ms_ = millis();  // the loop starts the check (guarded by !gh_running_)
}

void LibrescootBleClient::set_dbc_version_(const std::string &v) {
  this->dbc_version_ = v;
  if (this->sw_dbc_ != nullptr)
    this->sw_dbc_->publish_state(v);
  this->publish_current_();
}

void LibrescootBleClient::publish_current_() {
  // Each board's update entity shows its own installed version.
  if (this->mdb_update_ != nullptr && !this->mdb_version_.empty())
    this->mdb_update_->set_current(this->mdb_version_);
  if (this->dbc_update_ != nullptr && !this->dbc_version_.empty())
    this->dbc_update_->set_current(this->dbc_version_);
  // Availability can change when a version arrives (e.g. after a reboot).
  this->refresh_update_availability_();
}

void LibrescootBleClient::request_update_check() {
  if (this->gh_running_)
    return;
  this->gh_channel_ = this->channel_;
  // Snapshot the installed MDB version (GitHub tag form) so the worker can list every release
  // between it and the channel latest — a multi-hop update shows all the skipped changelogs.
  this->gh_current_tag_ = this->mdb_version_.empty() ? "" : version_to_tag(this->mdb_version_);
  this->gh_running_ = true;
  this->gh_done_ = false;
  this->gh_ok_ = false;
  // The result is applied in loop(), which BLEClientBase disables while the client is idle — make
  // sure it runs so a check triggered while disconnected (e.g. HA "check for updates") completes.
  this->enable_loop();
  ESP_LOGI(TAG, "update check: querying GitHub for channel '%s'", this->gh_channel_.c_str());
  // 16 kB stack: cert-bundle TLS verification uses materially more stack than a pinned cert.
  BaseType_t ok = xTaskCreate(&LibrescootBleClient::github_task_, "librescoot_gh", 16384, this, 5, nullptr);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "update check: could not start worker task");
    this->gh_running_ = false;
  }
}

void LibrescootBleClient::github_task_(void *arg) {
  static_cast<LibrescootBleClient *>(arg)->github_fetch_();
  vTaskDelete(nullptr);
}

// --- HA-integration reachability: is the plain-HTTP OTA relay it configures actually up? -------
void LibrescootBleClient::request_integration_check_() {
  if (this->hi_running_)
    return;
  this->hi_running_ = true;
  this->hi_done_ = false;
  if (xTaskCreate(&LibrescootBleClient::check_integration_task_, "librescoot_hi", 4096, this, 4, nullptr) != pdPASS)
    this->hi_running_ = false;
}

void LibrescootBleClient::check_integration_task_(void *arg) {
  static_cast<LibrescootBleClient *>(arg)->check_integration_();
  vTaskDelete(nullptr);
}

// Scheduler tick (every 2 s): "pairing required" is true ONLY when a scooter is configured, its
// advertisement has been heard recently (it's genuinely in range) and pairing is blocked because an
// unrequested (re)pair was refused/failed — i.e. the configured scooter is present but not bonded.
// It clears as soon as the scooter leaves range, gets bonded, or pairing is armed via 'Pair Scooter'.
// BinarySensor::publish_state only emits on change, so publishing every tick is cheap.
void LibrescootBleClient::service_pairing_watch_() {
  if (this->pairing_required_sensor_ == nullptr)
    return;
  const uint32_t now = millis();
  bool configured = this->get_address() != 0;
  bool in_range = configured && (now - this->last_adv_ms_ < this->presence_timeout_ms_);
  // Tri-state, never falsely "OK": a live (encrypted) connection is the ONLY proof of a valid bond,
  // so OK is published solely while connected. In range but unbonded → Problem. Anything else (out
  // of range, or simply not connected yet this session) is genuinely unknown — publish nothing
  // certain, mark the state missing so HA shows "unknown" rather than a reassuring OK.
  if (this->connected()) {
    this->pairing_required_sensor_->publish_state(false);
  } else if (in_range && this->pairing_blocked_) {
    this->pairing_required_sensor_->publish_state(true);
  } else {
    this->pairing_required_sensor_->invalidate_state();
  }
}

// Scheduler tick: drive the "interval" link mode — connect, let on_connected_ read every sensor
// once during a short dwell, then release the link for link_interval_ms_. Never runs a cycle while
// pairing is blocked or an OTA is active. Runs off the scheduler so it fires with the link down.
void LibrescootBleClient::service_link_interval_() {
  if (this->link_mode_str_ != "interval")
    return;
  const uint32_t now = millis();
  if (this->interval_refreshing_) {
    if (now >= this->interval_release_at_ms_) {
      this->interval_refreshing_ = false;
      this->interval_next_ms_ = now + this->link_interval_ms_;
      ESP_LOGI(TAG, "interval: sensor refresh done — releasing link for %u s",
               (unsigned) (this->link_interval_ms_ / 1000));
      this->apply_link_state_();  // want=false now → link down until the next cycle
    }
  } else if (now >= this->interval_next_ms_ && !this->pairing_blocked_ && !this->ota_active_) {
    this->interval_refreshing_ = true;
    this->interval_release_at_ms_ = now + INTERVAL_DWELL_MS;
    ESP_LOGI(TAG, "interval: connecting to refresh all sensors");
    this->apply_link_state_();  // want=true (interval_refreshing_) → bring the link up
  }
}

// Scheduler tick (every 5 s): publish the last probe result + a throughput/capability hint, then
// launch the next probe when one is due. Runs independently of loop() (disabled while BLE is idle).
void LibrescootBleClient::service_integration_check_() {
  const uint32_t now = millis();
  if (this->hi_done_) {
    this->hi_done_ = false;
    this->hi_running_ = false;
    if (this->ha_integration_ != nullptr && this->integration_reachable_ != this->hi_last_) {
      this->hi_last_ = this->integration_reachable_;
      this->ha_integration_->publish_state(this->integration_reachable_);
    }
    if (!this->ota_download_capable_() && now - this->ota_hint_last_ms_ > 300000) {
      this->ota_hint_last_ms_ = now;
      ESP_LOGW(TAG, "OTA firmware download not possible: no HA-integration relay reachable and this "
                    "board can't download from GitHub directly (no cert bundle). Install/enable the "
                    "Home Assistant 'Librescoot-BLE-Client' integration, or use an S3+PSRAM board "
                    "with use_cert_bundle: true.");
    }
  }
  if (!this->hi_running_ && now >= this->hi_next_check_ms_) {
    this->hi_next_check_ms_ = now + 60000;
    this->request_integration_check_();
  }
}

void LibrescootBleClient::check_integration_() {
  bool reach = false;
  // Only an http:// source is the HA integration's relay (or a local mirror); an https source is
  // GitHub direct, where reachability is a TLS-capability question, not a ping.
  if (this->ota_source_url_.rfind("http://", 0) == 0) {
    esp_http_client_config_t cfg = {};
    cfg.url = this->ota_source_url_.c_str();
    cfg.timeout_ms = 3000;
    cfg.method = HTTP_METHOD_HEAD;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c != nullptr) {
      if (esp_http_client_open(c, 0) == ESP_OK)
        reach = true;  // the relay accepted the connection (any HTTP status) → integration is up
      esp_http_client_close(c);
      esp_http_client_cleanup(c);
    }
  }
  this->integration_reachable_ = reach;
  this->hi_done_ = true;
}

// Stream a GitHub API GET (TLS per the YAML-configured trust) through a per-byte sink, without
// buffering the whole payload. Returns true on HTTP 200.
bool LibrescootBleClient::github_http_stream_(const std::string &url,
                                              const std::function<void(char)> &sink) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  this->ota_http_tls_(&cfg);
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  esp_http_client_set_header(c, "User-Agent", "esphome-lsc-bluetooth-nrf");
  esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
  esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");
  bool ok = false;
  esp_err_t err = esp_http_client_open(c, 0);
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status == 200) {
      char buf[512];
      int r;
      uint32_t since_yield = 0;
      while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r; i++)
          sink(buf[i]);
        // The GitHub releases listing is large (~1 MB) and parsed byte-by-byte in this worker;
        // yield periodically so the WiFi/system tasks aren't starved during the long CPU-bound
        // parse — without it the read stalls on the S3.
        since_yield += r;
        if (since_yield >= 16384) {
          since_yield = 0;
          vTaskDelay(1);
        }
      }
      ok = true;
    } else {
      ESP_LOGW(TAG, "update check: HTTP %d", status);
    }
  } else {
    ESP_LOGW(TAG, "update check: connection failed (%s)", esp_err_to_name(err));
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return ok;
}

// Append one JSON-string char (escape-decoded) to dst, capped at cap. Returns false once full.
static void json_body_char(std::string &dst, char ch, bool &esc, int &uskip, size_t cap) {
  if (uskip > 0) { uskip--; return; }  // swallow the 4 hex digits of a \uXXXX escape
  if (esc) {
    esc = false;
    switch (ch) {
      case 'n': if (dst.size() < cap) dst += '\n'; break;
      case 't': if (dst.size() < cap) dst += ' '; break;
      case '"': if (dst.size() < cap) dst += '"'; break;
      case '\\': if (dst.size() < cap) dst += '\\'; break;
      case '/': if (dst.size() < cap) dst += '/'; break;
      case 'u': uskip = 4; break;  // non-ASCII: skip (bodies are mostly raw UTF-8)
      case 'r': break;             // drop CR
      default: if (dst.size() < cap) dst += ch; break;
    }
    return;
  }
  if (ch == '\\') { esc = true; return; }
  if (dst.size() < cap) dst += ch;
}

void LibrescootBleClient::github_fetch_() {
  const bool stable = this->gh_channel_ == "stable";
  const std::string base = "https://api.github.com/repos/" + this->github_repo_ + "/releases";
  const std::string prefix = this->gh_channel_ + "-";  // e.g. "nightly-"
  const std::string &curtag = this->gh_current_tag_;   // installed MDB version (tag form), may be empty

  std::string best;
  std::string summary;

  if (stable) {
    // Stable ships one release at a time via /latest; keep the single-release path.
    static const char NEEDLE[] = "\"tag_name\":\"";
    const size_t NLEN = sizeof(NEEDLE) - 1;
    size_t match = 0; bool cap = false; std::string cur;
    this->github_http_stream_(base + "/latest", [&](char ch) {
      if (cap) { if (ch == '"') { cap = false; best = cur; } else cur += ch; return; }
      if (ch == NEEDLE[match]) { if (++match == NLEN) { match = 0; cap = true; cur.clear(); } }
      else match = (ch == NEEDLE[0]) ? 1 : 0;
    });
  } else {
    // Listing channel (nightly/testing): the /releases listing carries tag_name AND body for
    // every release, newest first. Capture each (tag, body) so a multi-hop update can show every
    // skipped changelog. tag_name always precedes body within a release object.
    struct Rel { std::string tag; std::string body; };
    std::vector<Rel> rels;
    static const char TN[] = "\"tag_name\":\"";
    static const char BN[] = "\"body\":\"";
    size_t tm = 0, bm = 0;
    int mode = 0;  // 0 idle, 1 capturing tag, 2 capturing body
    bool esc = false; int uskip = 0;
    std::string cur_tag, cur_body;
    // Capture enough releases to cover a large multi-hop jump so NO intermediate version's changelog
    // is dropped (a small MAX_REL silently hid the oldest skipped releases entirely).
    const size_t MAX_REL = 40, PER_BODY = 400;
    this->github_http_stream_(base + "?per_page=40", [&](char ch) {
      if (mode == 1) {  // tags have no escapes
        if (ch == '"') mode = 0; else cur_tag += ch;
        return;
      }
      if (mode == 2) {
        if (!esc && uskip == 0 && ch == '"') {  // body closed → commit this release
          mode = 0;
          if (cur_tag.rfind(prefix, 0) == 0 && rels.size() < MAX_REL) {
            if (cur_body.size() >= PER_BODY) cur_body += "…";
            rels.push_back({cur_tag, cur_body});
          }
          cur_body.clear();
          return;
        }
        json_body_char(cur_body, ch, esc, uskip, PER_BODY);
        return;
      }
      // idle: watch for the next tag_name / body key
      tm = (ch == TN[tm]) ? tm + 1 : (ch == TN[0] ? 1 : 0);
      if (TN[tm] == 0) { mode = 1; cur_tag.clear(); tm = bm = 0; return; }
      bm = (ch == BN[bm]) ? bm + 1 : (ch == BN[0] ? 1 : 0);
      if (BN[bm] == 0) { mode = 2; cur_body.clear(); esc = false; uskip = 0; tm = bm = 0; }
    });

    for (auto &r : rels)
      if (r.tag > best) best = r.tag;  // newest = channel latest

    // Releases strictly newer than the installed version, up to the latest (newest first).
    std::vector<const Rel *> inc;
    for (auto &r : rels) {
      bool q = curtag.empty() ? (r.tag == best) : (r.tag > curtag && r.tag <= best);
      if (q) inc.push_back(&r);
    }
    if (inc.size() <= 1) {
      if (!inc.empty()) summary = inc.front()->body;
    } else {
      // Header first: how many releases and which are bundled into this jump.
      summary = std::to_string(inc.size()) + " releases since installed:\n";
      for (auto *r : inc) summary += "• " + r->tag + "\n";
      for (auto *r : inc) {
        summary += "\n### " + r->tag + "\n" + r->body + "\n";
        // Generous total cap so every skipped release's changelog is included on a multi-hop jump
        // (the bullet list above always lists ALL versions regardless; this bounds the bodies).
        if (summary.size() > 7000) { summary += "\n…"; break; }
      }
    }
  }

  // Asset sizes for the chosen release (and, for stable, its body) — from the single-release JSON.
  if (!best.empty()) {
    const std::string aext = (this->ota_method_str_ == "full") ? ".mender" : ".delta";
    const std::string tmdb = "librescoot-unu-mdb-" + best + aext;
    const std::string tdbc = "librescoot-unu-dbc-" + best + aext;
    static const char AN[] = "\"name\":\"";
    static const char AS[] = "\"size\":";
    static const char BN[] = "\"body\":\"";
    size_t an = 0, as = 0, bn = 0;
    int acap = 0;  // 0 idle, 1 name, 2 size
    bool bcap = false, bdone = false, esc = false; int uskip = 0;
    std::string aname, asize;
    uint32_t mdb_sz = 0, dbc_sz = 0;
    this->github_http_stream_(base + "/tags/" + best, [&](char ch) {
      if (acap == 1) {
        if (ch == '"') acap = 0; else aname += ch;
      } else if (acap == 2) {
        if (ch >= '0' && ch <= '9') { asize += ch; }
        else {
          acap = 0;
          uint32_t sz = (uint32_t) strtoul(asize.c_str(), nullptr, 10);
          if (aname == tmdb) mdb_sz = sz; else if (aname == tdbc) dbc_sz = sz;
          asize.clear();
        }
      } else {
        an = (ch == AN[an]) ? an + 1 : (ch == AN[0] ? 1 : 0);
        if (AN[an] == 0) { acap = 1; aname.clear(); an = as = 0; }
        else { as = (ch == AS[as]) ? as + 1 : (ch == AS[0] ? 1 : 0); if (AS[as] == 0) { acap = 2; asize.clear(); an = as = 0; } }
      }
      // Stable body extractor (listing channels already built the summary above).
      if (stable && !bdone) {
        if (bcap) {
          if (!esc && uskip == 0 && ch == '"') { bdone = true; bcap = false; }
          else json_body_char(summary, ch, esc, uskip, 1400);
          return;
        }
        bn = (ch == BN[bn]) ? bn + 1 : (ch == BN[0] ? 1 : 0);
        if (BN[bn] == 0) { bcap = true; summary.clear(); esc = false; uskip = 0; bn = 0; }
      }
    });
    if (stable && summary.size() >= 1400) summary += "\n…";
    this->gh_mdb_size_ = mdb_sz;
    this->gh_dbc_size_ = dbc_sz;
  }

  this->gh_latest_tag_ = best;
  this->gh_summary_ = summary;
  this->gh_ok_ = !best.empty();
  this->gh_done_ = true;  // consumed on the main loop
}

void LibrescootBleClient::apply_check_result_() {
  if (!this->gh_ok_ || this->gh_latest_tag_.empty()) {
    ESP_LOGW(TAG, "update check: no result; will retry");
    this->next_check_ms_ = millis() + 45 * 1000UL;  // retry soon (TLS can fail under heap pressure)
    return;
  }
  ESP_LOGI(TAG, "update check: latest '%s' for channel '%s'", this->gh_latest_tag_.c_str(),
           this->gh_channel_.c_str());
  this->refresh_update_availability_();
}

// Two-phase model (like the phone app): first bring DBC up to match MDB, THEN advance MDB to
// the channel latest. So while DBC is behind MDB, the DBC "catch up" is offered and MDB is held
// (shown up to date); once in sync, MDB advances and DBC is gated. The manual "OTA … Install"
// buttons ignore this gating.
void LibrescootBleClient::refresh_update_availability_() {
  const std::string &tag = this->gh_latest_tag_;  // channel latest (GitHub tag form)
  if (tag.empty())
    return;
  const std::string method = this->ota_method_str_;
  // While a component is transferring/installing/awaiting its reboot — OR the scooter itself
  // reports an install/pending-reboot — leave the entities as "up to date"; don't offer an
  // update that's already on its way. `ota_scooter_busy_` also covers the case where the ESP
  // rebooted but the scooter is still pending its own reboot.
  // Only offer updates when the scooter's OTA state is explicitly IDLE (phase 0x06). Any other
  // value — including "unknown" before we've heard back, and "pending reboot" — suppresses the
  // offer (HA derives "available" from latest != installed and ignores our state enum, so we
  // publish latest == current to truly hide it). The OTA Status text still shows the phase.
  if (this->ota_scooter_phase_ != 0x06) {
    if (this->mdb_update_ != nullptr && !this->mdb_version_.empty()) {
      this->mdb_update_->set_latest(this->mdb_version_, "LibreScoot MDB", "", "");
      this->mdb_update_->set_available(false);
    }
    if (this->dbc_update_ != nullptr && !this->dbc_version_.empty()) {
      this->dbc_update_->set_latest(this->dbc_version_, "LibreScoot DBC", "", "");
      this->dbc_update_->set_available(false);
    }
    return;
  }
  bool busy = this->ota_awaiting_version_ || this->ota_state_ != OtaState::IDLE;
  // A version of "unknown" is NOT a real installed version — the scooter reports it for the DBC
  // while in stand-by (the dashboard is off). Never offer an update against it, or HA would show
  // "unknown → <latest>" and prompt an install. Treated exactly like a missing version.
  bool have_mdb = !this->mdb_version_.empty() && !ieq(this->mdb_version_, "unknown");
  bool have_dbc = !this->dbc_version_.empty() && !ieq(this->dbc_version_, "unknown");
  bool dbc_behind_mdb = have_mdb && have_dbc && !ieq(this->dbc_version_, this->mdb_version_);
  uint32_t total = this->gh_mdb_size_ + this->gh_dbc_size_;
  std::string tot = total ? ("  ·  total (MDB+DBC): " + fmt_size(total)) : "";

  // MDB → channel latest, but only when DBC has caught up (DBC-first).
  if (this->mdb_update_ != nullptr && !(busy && this->ota_active_update_ == this->mdb_update_)) {
    if (!have_mdb) {
      // Installed version unknown → publish latest == current so no update is offered.
      if (!this->mdb_version_.empty()) {
        this->mdb_update_->set_latest(this->mdb_version_, "LibreScoot MDB", "", "");
        this->mdb_update_->set_available(false);
      }
    } else {
      bool avail = !ieq(this->mdb_version_, tag) && !dbc_behind_mdb;
      std::string url = "https://github.com/" + this->github_repo_ + "/releases/tag/" + tag;
      std::string sum = this->gh_summary_;
      if (avail && this->gh_mdb_size_)
        sum = "**MDB:** " + fmt_size(this->gh_mdb_size_) + "  ·  est. " + fmt_est_time(this->gh_mdb_size_) +
              " (" + method + ")" + tot + (sum.empty() ? "" : "\n\n" + sum);
      this->mdb_update_->set_latest(avail ? tag : this->mdb_version_, "LibreScoot MDB", avail ? url : "", sum);
      this->mdb_update_->set_available(avail);
    }
  }

  // DBC → align to whatever MDB currently runs (target = MDB version).
  if (this->dbc_update_ != nullptr && !(busy && this->ota_active_update_ == this->dbc_update_)) {
    if (!have_dbc) {
      // DBC version unknown (typically: scooter in stand-by) → never offer; latest == current.
      if (!this->dbc_version_.empty()) {
        this->dbc_update_->set_latest(this->dbc_version_, "LibreScoot DBC", "", "");
        this->dbc_update_->set_available(false);
      }
    } else {
      bool avail = dbc_behind_mdb;
      bool to_latest = avail && ieq(this->mdb_version_, tag);  // MDB already at latest -> size known
      std::string target = avail ? this->mdb_version_ : this->dbc_version_;
      std::string url = to_latest ? ("https://github.com/" + this->github_repo_ + "/releases/tag/" + tag) : "";
      std::string sum;
      if (to_latest && this->gh_dbc_size_)
        sum = "**DBC:** " + fmt_size(this->gh_dbc_size_) + "  ·  est. " + fmt_est_time(this->gh_dbc_size_) +
              " (" + method + ")" + tot + (this->gh_summary_.empty() ? "" : "\n\n" + this->gh_summary_);
      else if (avail)
        sum = "DBC → align to MDB (" + this->mdb_version_ + ")";
      this->dbc_update_->set_latest(target, "LibreScoot DBC", url, sum);
      this->dbc_update_->set_available(avail);
    }
  }
}

void LibrescootBleClient::ota_settle_update_entity_() {
  // Clear the installing component's progress bar, then re-evaluate availability from the
  // versions we know — no online check (the scheduled check runs on its own timer).
  if (this->ota_active_update_ != nullptr)
    this->ota_active_update_->clear_progress(false);
  this->refresh_update_availability_();
}

void LibrescootBleClient::perform_update(uint8_t component, bool force) {
  if (this->ota_state_ != OtaState::IDLE || this->ota_resolve_running_ || !this->ota_jobs_.empty() ||
      this->ota_awaiting_version_) {
    ESP_LOGW("ota", "install already in progress (abort first to start another)");
    return;
  }
  if (!this->ota_download_capable_()) {
    ESP_LOGW("ota", "cannot start: no firmware byte source. The HA 'Librescoot-BLE-Client' "
                    "integration's OTA relay isn't reachable and this board can't download from "
                    "GitHub directly (no cert bundle). Install/enable the integration, or use an "
                    "S3+PSRAM board with use_cert_bundle: true.");
    if (this->ota_status_ != nullptr)
      this->ota_status_->publish_state("Error: no byte source (HA integration/relay not reachable)");
    return;
  }
  this->ota_cancel_ = false;
  this->ota_resume_count_ = 0;
  this->ota_install_component_ = component;
  this->enable_loop();  // the OTA engine is driven from loop(); keep it running (see loop()).
  // Target: the OTA Version text if the user set one, else — for MDB the channel latest, for
  // DBC the MDB's currently-installed version (bring DBC up to match the board it runs on).
  std::string tag = this->ota_target_version_;
  if (tag.empty())
    tag = (component == 1) ? version_to_tag(this->mdb_version_) : this->gh_latest_tag_;
  if (tag.empty()) {
    ESP_LOGW("ota", "no target version — enter one in OTA Version or run the update check first");
    if (this->ota_status_ != nullptr)
      this->ota_status_->publish_state("Error: no target version");
    return;
  }
  this->ota_active_update_ = (component == 1) ? this->dbc_update_ : this->mdb_update_;
  // Resolve the asset for the target release in a worker (queries the GitHub API → verifies the
  // tag exists), then the loop kicks off the single transfer for this component.
  this->rs_tag_ = tag;
  this->rs_source_ = this->ota_source_url_;
  this->rs_mdb_ok_ = this->rs_dbc_ok_ = false;
  this->ota_resolve_running_ = true;
  this->ota_resolve_done_ = false;
  ESP_LOGI("ota", "install %s: resolving %s assets for '%s' (bytes from %s)",
           force ? "(forced)" : "", this->ota_method_str_.c_str(), this->rs_tag_.c_str(), this->rs_source_.c_str());
  if (xTaskCreate(&LibrescootBleClient::ota_resolve_task_, "librescoot_res", 16384, this, 5, nullptr) != pdPASS) {
    ESP_LOGW("ota", "could not start resolve task (an update check may be running) — try again");
    this->ota_resolve_running_ = false;
    if (this->ota_status_ != nullptr)
      this->ota_status_->publish_state("Error: busy, try again");
  }
}

// ---------------------------------------------------------------------------
// control entry points
// ---------------------------------------------------------------------------
void LibrescootBleClient::on_button(BtnAction a) {
  switch (a) {
    case BtnAction::SEATBOX_OPEN: this->write_str_(CharId::CTRL_CMD, "scooter:seatbox open"); break;
    case BtnAction::HIBERNATE: this->write_str_(CharId::POWER_CMD, "hibernate"); break;
    case BtnAction::WAKEUP: this->write_str_(CharId::POWER_CMD, "wakeup"); break;
    case BtnAction::REBOOT: this->write_str_(CharId::POWER_CMD, "reboot"); break;
    case BtnAction::REBOOT_HARD: this->write_str_(CharId::POWER_CMD, "hard-reboot"); break;
    case BtnAction::REMOVE_BOND:
      esp_ble_remove_bond_device(this->get_remote_bda());
      break;
    case BtnAction::PAIR:
      // Deliberate (re)pairing: clear any stale bond for a clean slate, arm the passkey flow for a
      // generous window, and reconnect so the scooter can bond. Disarmed once bonding succeeds or
      // the window expires.
      ESP_LOGI(TAG, "Pairing armed for %u s — clearing any stale bond and connecting to bond with "
                    "the scooter", PAIR_ARM_WINDOW_MS / 1000);
      esp_ble_remove_bond_device(this->get_remote_bda());
      this->pair_arm_until_ms_ = millis() + PAIR_ARM_WINDOW_MS;
      this->pairing_blocked_ = false;
      this->apply_link_state_();
      break;
    case BtnAction::OTA_STATUS_REQ: {
      const uint8_t d[] = {0x05};
      this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
      break;
    }
    case BtnAction::OTA_ABORT:
      // Full abort: stop the current session AND drop the rest of the queue (so DBC doesn't
      // start after aborting MDB), and cancel any post-install wait for the reboot/version.
      this->ota_user_abort();
      break;
    case BtnAction::SYSTIME_SYNC:
      if (this->time_ != nullptr && this->time_->now().is_valid())
        this->run_command_(std::string("time:set ") + to_string((long) this->time_->now().timestamp));
      break;
    case BtnAction::RESTART_ESP: App.safe_reboot(); break;
    case BtnAction::OTA_MDB_UPDATE:
      // Manual MDB install — always available; target = OTA Version if set, else channel latest.
      this->perform_update(0, true);
      break;
    case BtnAction::OTA_DBC_UPDATE:
      // Manual DBC install — always available; target = OTA Version if set, else the MDB version.
      this->perform_update(1, true);
      break;
    case BtnAction::ALARM_START: this->run_command_("alarm:start"); break;
    case BtnAction::ALARM_STOP: this->run_command_("alarm:stop"); break;
    case BtnAction::NAV_CLEAR: this->run_command_("nav:clear"); break;
    case BtnAction::CANCEL_HIBERNATE: this->run_command_("pm:hibernate-cancel"); break;
    case BtnAction::REFRESH: this->refresh_(); break;
  }
}

void LibrescootBleClient::on_select(LibrescootSelect *sel, const std::string &value) {
  switch (sel->kind()) {
    case SelKind::BLINKER:
      this->write_str_(CharId::CTRL_CMD, std::string("scooter:blinker ") + value);
      sel->publish_state(value);
      break;
    case SelKind::USB_MODE:
      // reflected from the UMS status, not optimistic
      this->run_command_(value == "Mass Storage" ? "usb:ums" : "usb:normal");
      break;
    case SelKind::LINK_MODE: {
      bool entering_scan = value == "scan" && this->link_mode_str_ != "scan";
      this->link_mode_str_ = value;  // set first so publish_discovered_'s scan-mode POST gate is true
      // Entering scan starts a fresh discovery pass — clear results, then POST this ESP's own
      // scooter (marked) immediately so the HA picker always has it, even before any advert.
      if (entering_scan) {
        this->discovered_.clear();
        this->publish_discovered_();
      }
      // Reset the interval state machine: a fresh "interval" selection refreshes right away; leaving
      // it cancels any in-progress dwell so apply_link_state_ can take the link down.
      this->interval_refreshing_ = false;
      this->interval_next_ms_ = millis();
      sel->publish_state(value);
      this->save_link_mode_();
      this->apply_link_state_();
      break;
    }
    case SelKind::OTA_CHANNEL:
      if (value != "undefined")
        this->run_command_(std::string("config:update-channel ") + value);
      sel->publish_state(value);
      break;
    case SelKind::OTA_METHOD:
      // Transfer method for the next install: "delta" (small patch, default) or "full"
      // (complete .mender image, hundreds of MB — an overnight transfer on this hardware).
      this->ota_method_str_ = value;
      sel->publish_state(value);
      break;
    case SelKind::OTA_SOURCE:
      // Byte source for the OTA download: "direct GitHub" (the ESP fetches from GitHub itself) or
      // "HA relay" (the Home Assistant integration serves the bytes over plain HTTP). apply_ota_source_
      // publishes the select + persists the mode; in github mode it also points the URL at GitHub.
      this->apply_ota_source_(value == "direct GitHub" ? "github" : "relay", true);
      break;
  }
}

void LibrescootBleClient::on_switch(SwKind k, bool state) {
  switch (k) {
    case SwKind::ALARM_ENABLED:
      this->run_command_(state ? "alarm:enable" : "alarm:disable");
      if (this->alarm_enabled_ != nullptr)
        this->alarm_enabled_->publish_state(state);
      break;
    case SwKind::ALARM_ARMED:
      this->run_command_(state ? "alarm:arm" : "alarm:disarm");
      if (this->alarm_armed_ != nullptr)
        this->alarm_armed_->publish_state(state);
      break;
    case SwKind::PM_SCHED_HIB:
      this->run_command_(state ? "set:pm.scheduled-hibernate-enabled true"
                               : "set:pm.scheduled-hibernate-enabled false");
      if (this->pm_sched_hib_ != nullptr)
        this->pm_sched_hib_->publish_state(state);
      break;
  }
}

void LibrescootBleClient::on_text(TxtKind k, const std::string &value) {
  switch (k) {
    case TxtKind::COMMAND:
      this->run_command_(value);
      if (this->command_text_ != nullptr)
        this->command_text_->publish_state(value);
      break;
    case TxtKind::NAV_DEST:
      this->run_command_(std::string("nav:dest ") + value);
      break;
    case TxtKind::CELLULAR_APN:
      this->run_command_(std::string("set:cellular.apn ") + value);
      if (this->apn_text_ != nullptr)
        this->apn_text_->publish_state(value);
      break;
    case TxtKind::PM_CRON:
      this->run_command_(std::string("set:pm.scheduled-hibernate-cron ") + value);
      if (this->pm_cron_text_ != nullptr)
        this->pm_cron_text_->publish_state(value);
      break;
    case TxtKind::PM_DURATION:
      this->run_command_(std::string("set:pm.scheduled-hibernate-duration ") + value);
      if (this->pm_duration_text_ != nullptr)
        this->pm_duration_text_->publish_state(value);
      break;
    case TxtKind::OTA_SOURCE_URL: {
      // In "direct GitHub" mode the OTA Source select owns the URL — ignore external writes (e.g. a
      // racing/stale HA-relay push) and reflect the real GitHub URL back, so mode and URL never
      // disagree. Only "HA relay" mode accepts an integration-supplied URL.
      if (this->ota_source_mode_ == "github") {
        if (this->ota_source_url_text_ != nullptr && value != this->ota_source_url_)
          this->ota_source_url_text_->publish_state(this->ota_source_url_);
        break;
      }
      this->ota_source_url_ = value;
      if (this->ota_source_url_text_ != nullptr)
        this->ota_source_url_text_->publish_state(value);
      UrlPref up{};
      strncpy(up.url, value.c_str(), sizeof(up.url) - 1);
      this->url_pref_.save(&up);  // survive an ESP-only reboot
      this->hi_next_check_ms_ = millis();  // the integration just (re)pointed us — re-check the relay
      break;
    }
    case TxtKind::OTA_VERSION:
      // Target release tag for OTA Update; blank = use the latest resolved release. Verified
      // against GitHub when the transfer is triggered (the resolve queries this exact tag).
      this->ota_target_version_ = value;
      break;
    case TxtKind::SYSTIME_ISO: {
      int y, mo, d, h, mi, se;
      if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &se) != 6) {
        ESP_LOGW(TAG, "SystemTime Set: expected 2026-07-26T18:45:30Z, got '%s'", value.c_str());
        break;
      }
      int yy = y - (mo <= 2);
      long era = (yy >= 0 ? yy : yy - 399) / 400;
      unsigned yoe = (unsigned) (yy - era * 400);
      unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
      unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      long days = era * 146097L + (long) doe - 719468;
      long epoch = days * 86400L + h * 3600L + mi * 60L + se;
      this->run_command_(std::string("time:set ") + to_string(epoch));
      break;
    }
  }
}

void LibrescootBleClient::on_lock(bool lock_it) {
  this->lock_suppress_until_ = millis() + 5000;
  this->write_str_(CharId::CTRL_CMD, lock_it ? "scooter:state lock" : "scooter:state unlock");
  if (this->scooter_lock_ != nullptr)
    this->scooter_lock_->publish_state(lock_it ? lock::LOCK_STATE_LOCKED : lock::LOCK_STATE_UNLOCKED);
}

void LibrescootBleClient::passkey_reply(uint32_t passkey) {
  if (passkey > 999999)
    return;
  ESP_LOGI(TAG, "Sending passkey %06lu", (unsigned long) passkey);
  esp_ble_passkey_reply(this->get_remote_bda(), true, passkey);
  // Optimistically clear the prompt; a wrong code triggers a fresh PASSKEY_REQ that re-sets it.
  if (this->passkey_required_ != nullptr)
    this->passkey_required_->publish_state(false);
}

// ---------------------------------------------------------------------------
// entity trampolines
// ---------------------------------------------------------------------------
void LibrescootButton::press_action() { this->parent_->on_button(this->action_); }
void LibrescootSelect::control(const std::string &value) { this->parent_->on_select(this, value); }
void LibrescootSwitch::write_state(bool state) { this->parent_->on_switch(this->kind_, state); }
void LibrescootText::control(const std::string &value) { this->parent_->on_text(this->kind_, value); }
void LibrescootLock::control(const lock::LockCall &call) {
  if (call.get_state() == lock::LOCK_STATE_UNLOCKED)
    this->parent_->on_lock(false);
  else if (call.get_state() == lock::LOCK_STATE_LOCKED)
    this->parent_->on_lock(true);
}

void LibrescootUpdate::check() { this->parent_->request_update_check(); }
void LibrescootUpdate::perform(bool force) { this->parent_->update_perform(this, force); }

void LibrescootBleClient::update_perform(LibrescootUpdate *u, bool force) {
  this->perform_update(u == this->dbc_update_ ? 1 : 0, force);
}

}  // namespace librescoot_ble_client
}  // namespace esphome

#endif  // USE_ESP32
