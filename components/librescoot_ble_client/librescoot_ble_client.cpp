#include "librescoot_ble_client.h"
#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
  if (sw_mdb_ || update_)
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
  bool uses_ota = ota_status_ || update_;
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
  static const char *const MODES[] = {"disconnect", "scan", "auto", "always"};
  this->link_mode_str_ = MODES[idx <= 3 ? idx : 2];

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
  if (this->sw_esp_ != nullptr)
    this->sw_esp_->publish_state(ESPHOME_VERSION);
  if (this->link_mode_ != nullptr)
    this->link_mode_->publish_state(this->link_mode_str_);

  if (this->ota_source_url_text_ != nullptr)
    this->ota_source_url_text_->publish_state(this->ota_source_url_);

  // Allocate the OTA ring buffer once, while the heap is fresh (a runtime alloc fails on the
  // classic board once TLS has fragmented the heap). HW-adaptive: with PSRAM (S3) use the full
  // 64-chunk window; on the classic use 32 chunks to spare the internal heap. The ring bounds
  // the in-flight window; the negotiated window is capped to fit.
  if (this->ota_status_ != nullptr || this->update_ != nullptr) {
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

  // Presence + connection (once a second).
  if (now - this->last_pres_pub_ms_ >= 1000) {
    this->last_pres_pub_ms_ = now;
    bool conn = this->connected();
    if (this->ble_connection_ != nullptr)
      this->ble_connection_->publish_state(conn);
    if (this->ble_presence_ != nullptr)
      this->ble_presence_->publish_state(conn || (now - this->last_adv_ms_ < 12000));
  }

  // GitHub update check: consume a finished worker, then schedule the next one.
  if (this->gh_done_) {
    this->gh_done_ = false;
    this->apply_check_result_();
    this->gh_running_ = false;
  }
  if (this->update_ != nullptr && !this->gh_running_ && now >= this->next_check_ms_ &&
      (!this->mdb_version_.empty() || !this->dbc_version_.empty())) {
    this->next_check_ms_ = now + this->update_check_interval_;  // provisional; shortened on failure
    this->request_update_check();
  }

  // BLE-OTA transfer engine (consumer side).
  this->ota_step_();

  // Install: when the resolve worker finishes, build the transfer queue (MDB then DBC).
  if (this->ota_resolve_done_) {
    this->ota_resolve_done_ = false;
    this->ota_resolve_running_ = false;
    this->ota_jobs_.clear();
    std::string base = this->rs_source_ + "/" + this->rs_tag_ + "/";
    // bundle_id sent in START: deltas use the full filename (incl. .delta); full images use
    // the basename stem — update-service appends .mender itself.
    auto bundle_id = [this](const std::string &name) -> std::string {
      const std::string suf = ".mender";
      if (this->ota_method_str_ == "full" && name.size() > suf.size() &&
          name.compare(name.size() - suf.size(), suf.size(), suf) == 0)
        return name.substr(0, name.size() - suf.size());
      return name;
    };
    if (this->rs_mdb_ok_)
      this->ota_jobs_.push_back({base + this->rs_mdb_name_, this->rs_mdb_sha_, bundle_id(this->rs_mdb_name_), this->rs_mdb_size_, 0});
    if (this->rs_dbc_ok_)
      this->ota_jobs_.push_back({base + this->rs_dbc_name_, this->rs_dbc_sha_, bundle_id(this->rs_dbc_name_), this->rs_dbc_size_, 1});
    if (this->ota_jobs_.empty()) {
      ESP_LOGW("ota", "install: no %s assets found for %s (version not on GitHub?)",
               this->ota_method_str_.c_str(), this->rs_tag_.c_str());
      if (this->ota_status_ != nullptr)
        this->ota_status_->publish_state(std::string("Error: version not found: ") + this->rs_tag_);
    } else {
      ESP_LOGI("ota", "install: %u transfer(s) queued for %s", (unsigned) this->ota_jobs_.size(),
               this->rs_tag_.c_str());
    }
  }
  this->ota_kick_next_job_();

  // "auto" yield expired → re-enable the client so it can reconnect.
  if (this->yield_until_ms_ != 0 && now >= this->yield_until_ms_) {
    this->yield_until_ms_ = 0;
    if (this->link_mode_str_ == "auto")
      this->set_enabled(true);
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
      this->mark_unknown_();
      // "auto" link mode: release the link for a grace window on every disconnect so a
      // phone (or any other central) can win the reconnect race and take the scooter.
      // "always" reconnects immediately (base auto_connect); OTA always pins the link up.
      if (this->link_mode_str_ == "auto" && !this->ota_active_ && this->get_address() != 0) {
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
      if (this->check_addr(param->ble_security.auth_cmpl.bd_addr)) {
        ESP_LOGW(TAG, "!!! PIN REQUIRED !!! Home Assistant -> Developer Tools -> Actions ->");
        ESP_LOGW(TAG, "    '<node>_passkey_reply', enter the code shown on the scooter dashboard.");
        if (this->passkey_required_ != nullptr)
          this->passkey_required_->publish_state(true);
      }
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      // Bonding concluded — on success the passkey is no longer needed.
      if (this->check_addr(param->ble_security.auth_cmpl.bd_addr) &&
          param->ble_security.auth_cmpl.success && this->passkey_required_ != nullptr)
        this->passkey_required_->publish_state(false);
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
  if (device.address_uint64() == this->get_address())
    this->last_adv_ms_ = millis();
  if (!this->enabled_)
    return false;
  return BLEClientBase::parse_device(device);
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
  if (this->sw_dbc_ != nullptr || this->update_ != nullptr)
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

void LibrescootBleClient::refresh_() { this->on_connected_(); }

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
    case CharId::POWER_STATE:
      if (this->power_state_ != nullptr)
        this->power_state_->publish_state(clean_str_(v, len));
      break;
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
      if (this->scooter_lock_ != nullptr && millis() >= this->lock_suppress_until_)
        this->scooter_lock_->publish_state(s == "ready-to-drive" ? lock::LOCK_STATE_UNLOCKED
                                                                  : lock::LOCK_STATE_LOCKED);
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
  bool want = has_mac && (this->ota_active_ || (this->link_mode_str_ != "scan" && this->link_mode_str_ != "disconnect"));
  this->set_enabled(want);
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
}

void LibrescootBleClient::set_dbc_version_(const std::string &v) {
  this->dbc_version_ = v;
  if (this->sw_dbc_ != nullptr)
    this->sw_dbc_->publish_state(v);
  this->publish_current_();
}

void LibrescootBleClient::publish_current_() {
  if (this->update_ == nullptr)
    return;
  // MDB and DBC ship together; show the common installed version (fall back to whichever we
  // have). If they somehow differ, showing MDB is close enough — the check flags "available"
  // if either component is behind.
  const std::string &v = !this->mdb_version_.empty() ? this->mdb_version_ : this->dbc_version_;
  if (!v.empty())
    this->update_->set_current(v);
}

void LibrescootBleClient::request_update_check() {
  if (this->gh_running_)
    return;
  this->gh_channel_ = this->channel_;
  this->gh_running_ = true;
  this->gh_done_ = false;
  this->gh_ok_ = false;
  ESP_LOGI(TAG, "update check: querying GitHub for channel '%s'", this->gh_channel_.c_str());
  BaseType_t ok = xTaskCreate(&LibrescootBleClient::github_task_, "librescoot_gh", 8192, this, 5, nullptr);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "update check: could not start worker task");
    this->gh_running_ = false;
  }
}

void LibrescootBleClient::github_task_(void *arg) {
  static_cast<LibrescootBleClient *>(arg)->github_fetch_();
  vTaskDelete(nullptr);
}

// Stream a GitHub API GET (TLS validated against the two embedded roots) through a per-byte
// sink, without buffering the whole payload. Returns true on HTTP 200.
static bool github_http_stream(const std::string &url, const std::function<void(char)> &sink) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.cert_pem = GITHUB_ROOTS;
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
      while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0)
        for (int i = 0; i < r; i++)
          sink(buf[i]);
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

void LibrescootBleClient::github_fetch_() {
  const bool stable = this->gh_channel_ == "stable";
  const std::string base = "https://api.github.com/repos/" + this->github_repo_ + "/releases";
  const std::string prefix = this->gh_channel_ + "-";  // e.g. "nightly-"

  // --- pass 1: newest release tag for the selected channel ---
  std::string best;
  {
    static const char NEEDLE[] = "\"tag_name\":\"";
    const size_t NLEN = sizeof(NEEDLE) - 1;
    size_t match = 0;
    bool capturing = false;
    std::string cur;
    github_http_stream(stable ? base + "/latest" : base + "?per_page=30", [&](char ch) {
      if (capturing) {
        if (ch == '"') {
          capturing = false;
          if (stable)
            best = cur;  // /releases/latest has exactly one tag
          else if (cur.rfind(prefix, 0) == 0 && cur > best)
            best = cur;  // newest by lexicographic (= chronological) tag order
        } else {
          cur += ch;
        }
        return;
      }
      if (ch == NEEDLE[match]) {
        if (++match == NLEN) {
          match = 0;
          capturing = true;
          cur.clear();
        }
      } else {
        match = (ch == NEEDLE[0]) ? 1 : 0;
      }
    });
  }

  // --- pass 2: release notes (the "body" field) for the chosen tag ---
  std::string summary;
  if (!best.empty()) {
    static const char BN[] = "\"body\":\"";
    const size_t BLEN = sizeof(BN) - 1;
    size_t match = 0;
    bool capturing = false, esc = false, done = false;
    int uskip = 0;
    github_http_stream(base + "/tags/" + best, [&](char ch) {
      if (done)
        return;
      if (capturing) {
        if (uskip > 0) {  // swallow the 4 hex digits of a \uXXXX escape
          uskip--;
          return;
        }
        if (esc) {
          esc = false;
          switch (ch) {
            case 'n': summary += '\n'; break;
            case 't': summary += '\t'; break;
            case '"': summary += '"'; break;
            case '\\': summary += '\\'; break;
            case '/': summary += '/'; break;
            case 'u': uskip = 4; break;  // non-ASCII: skip (bodies are mostly raw UTF-8)
            case 'r': break;             // drop CR
            default: summary += ch; break;
          }
          return;
        }
        if (ch == '\\') {
          esc = true;
          return;
        }
        if (ch == '"') {
          done = true;
          capturing = false;
          return;
        }
        if (summary.size() < 1400)
          summary += ch;
        return;
      }
      if (ch == BN[match]) {
        if (++match == BLEN) {
          match = 0;
          capturing = true;
          summary.clear();
        }
      } else {
        match = (ch == BN[0]) ? 1 : 0;
      }
    });
    if (summary.size() >= 1400)
      summary += "\n…";
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
  const std::string &tag = this->gh_latest_tag_;
  std::string url = "https://github.com/" + this->github_repo_ + "/releases/tag/" + tag;
  ESP_LOGI(TAG, "update check: latest '%s' for channel '%s'", tag.c_str(), this->gh_channel_.c_str());

  // One update entity for the whole scooter. Home Assistant decides "update available" by
  // string-comparing installed vs latest, so when up to date (case-insensitively equal — the
  // BLE version reports a lowercase 't' in the timestamp, the tag an uppercase 'T') publish
  // latest == installed. Available if EITHER component is behind (they normally move together).
  if (this->update_ != nullptr && (!this->mdb_version_.empty() || !this->dbc_version_.empty())) {
    bool avail = (!this->mdb_version_.empty() && !ieq(this->mdb_version_, tag)) ||
                 (!this->dbc_version_.empty() && !ieq(this->dbc_version_, tag));
    const std::string &cur = !this->mdb_version_.empty() ? this->mdb_version_ : this->dbc_version_;
    this->update_->set_latest(avail ? tag : cur, "LibreScoot (MDB + DBC)", url, this->gh_summary_);
    this->update_->set_available(avail);
    ESP_LOGD(TAG, "update: MDB '%s' DBC '%s' vs '%s' -> %s", this->mdb_version_.c_str(),
             this->dbc_version_.c_str(), tag.c_str(), avail ? "available" : "up to date");
  }
}

void LibrescootBleClient::perform_update(bool force) {
  if (this->ota_state_ != OtaState::IDLE || this->ota_resolve_running_ || !this->ota_jobs_.empty()) {
    ESP_LOGW("ota", "install already in progress");
    return;
  }
  // Target: the OTA Version text if the user set one, otherwise the latest resolved release.
  std::string tag = this->ota_target_version_.empty() ? this->gh_latest_tag_ : this->ota_target_version_;
  if (tag.empty()) {
    ESP_LOGW("ota", "no target version — enter one in OTA Version or run the update check first");
    if (this->ota_status_ != nullptr)
      this->ota_status_->publish_state("Error: no target version");
    return;
  }
  // Resolve the assets (MDB + DBC) for the target release in a worker (this queries the GitHub
  // API for that exact tag → verifies it exists), then the loop kicks off the two transfers.
  this->rs_tag_ = tag;
  this->rs_source_ = this->ota_source_url_;
  this->rs_mdb_ok_ = this->rs_dbc_ok_ = false;
  this->ota_resolve_running_ = true;
  this->ota_resolve_done_ = false;
  ESP_LOGI("ota", "install %s: resolving %s assets for '%s' (bytes from %s)",
           force ? "(forced)" : "", this->ota_method_str_.c_str(), this->rs_tag_.c_str(), this->rs_source_.c_str());
  if (xTaskCreate(&LibrescootBleClient::ota_resolve_task_, "librescoot_res", 8192, this, 5, nullptr) != pdPASS) {
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
    case BtnAction::OTA_STATUS_REQ: {
      const uint8_t d[] = {0x05};
      this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
      break;
    }
    case BtnAction::OTA_ABORT: {
      const uint8_t d[] = {0x04, 0x00};
      this->write_raw_(CharId::OTA_CONTROL, d, sizeof(d));
      break;
    }
    case BtnAction::SYSTIME_SYNC:
      if (this->time_ != nullptr && this->time_->now().is_valid())
        this->run_command_(std::string("time:set ") + to_string((long) this->time_->now().timestamp));
      break;
    case BtnAction::RESTART_ESP: App.safe_reboot(); break;
    case BtnAction::OTA_UPDATE:
      // Transfer + install the target release (OTA Version text if set, else the latest
      // resolved release), honouring the OTA method select. The target tag is verified
      // against GitHub by the resolve step before any bytes move.
      this->perform_update(true);
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
    case SelKind::LINK_MODE:
      this->link_mode_str_ = value;
      sel->publish_state(value);
      this->save_link_mode_();
      this->apply_link_state_();
      break;
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
    case TxtKind::OTA_SOURCE_URL:
      this->ota_source_url_ = value;
      if (this->ota_source_url_text_ != nullptr)
        this->ota_source_url_text_->publish_state(value);
      break;
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
void LibrescootUpdate::perform(bool force) { this->parent_->perform_update(force); }

}  // namespace librescoot_ble_client
}  // namespace esphome

#endif  // USE_ESP32
