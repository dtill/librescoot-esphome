#pragma once
#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/button/button.h"
#include "esphome/components/text/text.h"
#include "esphome/components/lock/lock.h"
#include "esphome/components/update/update_entity.h"
#include "esphome/components/time/real_time_clock.h"
#include "esp_http_client.h"  // esp_http_client_config_t, for the shared TLS-config helper
#include <map>

#include <functional>
#include <string>
#include <vector>

namespace esphome {
namespace librescoot_ble_client {

namespace espbt = esphome::esp32_ble_tracker;

enum class BtnAction : uint8_t {
  SEATBOX_OPEN, HIBERNATE, WAKEUP, REBOOT, REBOOT_HARD, REMOVE_BOND,
  OTA_STATUS_REQ, OTA_ABORT, SYSTIME_SYNC, RESTART_ESP,
  ALARM_ARM, ALARM_DISARM, ALARM_START, ALARM_STOP, NAV_CLEAR, CANCEL_HIBERNATE, REFRESH, OTA_MDB_UPDATE, OTA_DBC_UPDATE,
  PAIR,
};
enum class SelKind : uint8_t { BLINKER, USB_MODE, LINK_MODE, OTA_CHANNEL, OTA_METHOD, OTA_SOURCE };
enum class SwKind : uint8_t { ALARM_ENABLED, PM_SCHED_HIB, STAGE_ONLY, AUTO_UPDATE };
enum class TxtKind : uint8_t {
  COMMAND, NAV_DEST, CELLULAR_APN, PM_CRON, PM_DURATION, SYSTIME_ISO, OTA_SOURCE_URL, OTA_VERSION
};

// Characteristics we read from, notify on, or write to.
enum class CharId : uint8_t {
  BAT1_SOC, BAT1_CYCLES, BAT1_PRESENT, BAT2_SOC, BAT2_CYCLES, BAT2_PRESENT,
  AUX_VOLTAGE, AUX_LEVEL, CBB_LEVEL, CBB_REMAINING, CBB_FULL, CBB_CELL,
  ODOMETER, NAV_ACTIVE, UMS_STATUS, POWER_MUX, STATUS, SEATBOX, HANDLEBAR,
  POWER_STATE, BAT1_STATE, BAT2_STATE, SW_MDB, SW_NRF, AUX_CHARGE, CBB_CHARGE,
  CMD_RESPONSE, OTA_STATUS,
  // write targets (interval 0, no notify):
  CTRL_CMD, POWER_CMD, EXT_CMD, OTA_CONTROL, OTA_DATA,
};

// Assumed BLE upload throughput until a transfer has measured one. The link tops out well below
// what the MTU suggests — writes are paced one chunk per loop iteration and weak-link rewinds cost
// further — so this is a measured floor, not a theoretical rate.
static constexpr float OTA_DEFAULT_RATE_BPS = 3700.0f;

// BLE-OTA transfer state (GATT service 9a590500).
enum class OtaState : uint8_t { IDLE, STARTING, STREAMING, COMPLETING, INSTALLING, DONE, FAILED };

class LibrescootBleClient;

class LibrescootButton : public button::Button, public Parented<LibrescootBleClient> {
 public:
  void set_action(BtnAction a) { this->action_ = a; }
 protected:
  void press_action() override;
  BtnAction action_{};
};

class LibrescootSelect : public select::Select, public Parented<LibrescootBleClient> {
 public:
  void set_kind(SelKind k) { this->kind_ = k; }
  SelKind kind() const { return this->kind_; }
 protected:
  void control(const std::string &value) override;
  SelKind kind_{};
};

class LibrescootSwitch : public switch_::Switch, public Parented<LibrescootBleClient> {
 public:
  void set_kind(SwKind k) { this->kind_ = k; }
 protected:
  void write_state(bool state) override;
  SwKind kind_{};
};

class LibrescootText : public text::Text, public Parented<LibrescootBleClient> {
 public:
  void set_kind(TxtKind k) { this->kind_ = k; }
 protected:
  void control(const std::string &value) override;
  TxtKind kind_{};
};

class LibrescootLock : public lock::Lock, public Parented<LibrescootBleClient> {
 protected:
  void control(const lock::LockCall &call) override;
};

// Single GitHub-backed update entity for the whole scooter (MDB + DBC ship together as one
// release; the transfer engine still runs them as two separate background OTA processes).
class LibrescootUpdate : public update::UpdateEntity, public Parented<LibrescootBleClient> {
 public:
  void check() override;
  void perform(bool force) override;
  void set_current(const std::string &v) {
    this->update_info_.current_version = v;
    // Home Assistant derives "an update is available" from latest != installed. A version learned
    // over BLE arrives long before the first GitHub check has a target, so publishing it on its own
    // shows up as an offer with an empty target. Keep the two equal until a real target exists.
    if (this->update_info_.latest_version.empty()) {
      this->update_info_.latest_version = v;
      this->state_ = update::UPDATE_STATE_NO_UPDATE;
    }
    this->publish_state();
  }
  void set_latest(const std::string &latest, const std::string &title, const std::string &url,
                  const std::string &summary) {
    this->update_info_.latest_version = latest;
    this->update_info_.title = title;
    this->update_info_.release_url = url;
    this->update_info_.summary = summary;
  }
  void set_available(bool avail) {
    this->state_ = avail ? update::UPDATE_STATE_AVAILABLE : update::UPDATE_STATE_NO_UPDATE;
    this->publish_state();
  }
  void set_progress(float pct) {
    this->state_ = update::UPDATE_STATE_INSTALLING;
    this->update_info_.has_progress = true;
    this->update_info_.progress = pct;
    this->publish_state();
  }
  void clear_progress(bool available) {
    this->update_info_.has_progress = false;
    this->update_info_.progress = 0.0f;
    this->state_ = available ? update::UPDATE_STATE_AVAILABLE : update::UPDATE_STATE_NO_UPDATE;
    this->publish_state();
  }
};

struct CharEntry {
  CharId id;
  espbt::ESPBTUUID service;
  espbt::ESPBTUUID chr;
  uint16_t handle{0};
  esp_gatt_char_prop_t props{};
  bool notify{false};
  bool force{false};   // read on connect; stays set until a read succeeds (or we give up)
  uint8_t tries{0};    // forced-read attempts so far
  uint32_t interval_ms{0};
  uint32_t last_ms{0};
};

class LibrescootBleClient : public esp32_ble_client::BLEClientBase {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  bool gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  bool parse_device(const espbt::ESPBTDevice &device) override;

  void set_enabled(bool enabled);
  void set_time(time::RealTimeClock *t) { this->time_ = t; }

  // Control entry points (from entity trampolines / the passkey action).
  void on_button(BtnAction a);
  void on_select(LibrescootSelect *sel, const std::string &value);
  void on_switch(SwKind k, bool state);
  void on_text(TxtKind k, const std::string &value);
  void on_lock(bool lock_it);
  void passkey_reply(uint32_t passkey);

  // --- entity setters (codegen) ---
  void set_bat1_soc(sensor::Sensor *s) { bat1_soc_ = s; }
  void set_bat1_cycles(sensor::Sensor *s) { bat1_cycles_ = s; }
  void set_bat2_soc(sensor::Sensor *s) { bat2_soc_ = s; }
  void set_bat2_cycles(sensor::Sensor *s) { bat2_cycles_ = s; }
  void set_aux_voltage(sensor::Sensor *s) { aux_voltage_ = s; }
  void set_aux_level(sensor::Sensor *s) { aux_level_ = s; }
  void set_cbb_level(sensor::Sensor *s) { cbb_level_ = s; }
  void set_cbb_remaining(sensor::Sensor *s) { cbb_remaining_ = s; }
  void set_cbb_full(sensor::Sensor *s) { cbb_full_ = s; }
  void set_cbb_cell(sensor::Sensor *s) { cbb_cell_ = s; }
  void set_odometer(sensor::Sensor *s) { odometer_ = s; }
  void set_rssi(sensor::Sensor *s) { rssi_ = s; }
  void set_ota_dl_speed(sensor::Sensor *s) { ota_dl_speed_ = s; }
  void set_ota_ul_speed(sensor::Sensor *s) { ota_ul_speed_ = s; }
  void set_ota_ble_bytes_total(sensor::Sensor *s) { ota_ble_bytes_total_ = s; }
  void set_ota_target_transferred(sensor::Sensor *s) { ota_target_transferred_ = s; }
  void set_ota_selfheal_resumes(sensor::Sensor *s) { ota_selfheal_resumes_ = s; }
  void set_ota_auto_resume(bool b) { this->ota_auto_resume_ = b; }

  void set_bat1_present(binary_sensor::BinarySensor *b) { bat1_present_ = b; }
  void set_bat2_present(binary_sensor::BinarySensor *b) { bat2_present_ = b; }
  void set_nav_active(binary_sensor::BinarySensor *b) { nav_active_ = b; }
  void set_aux_charger(binary_sensor::BinarySensor *b) { aux_charger_ = b; }
  void set_ums_status(binary_sensor::BinarySensor *b) { ums_status_ = b; }
  void set_maps_available(binary_sensor::BinarySensor *b) { maps_available_ = b; }
  void set_nav_available(binary_sensor::BinarySensor *b) { nav_available_ = b; }
  void set_ble_connection(binary_sensor::BinarySensor *b) { ble_connection_ = b; }
  void set_ble_presence(binary_sensor::BinarySensor *b) { ble_presence_ = b; }
  void set_presence_timeout(uint32_t ms) { this->presence_timeout_ms_ = ms; }
  void set_passkey_required(binary_sensor::BinarySensor *b) { passkey_required_ = b; }
  void set_pairing_required_sensor(binary_sensor::BinarySensor *b) { pairing_required_sensor_ = b; }
  void set_reboot_required(binary_sensor::BinarySensor *b) { reboot_required_ = b; }

  void set_status(text_sensor::TextSensor *t) { status_ = t; }
  void set_seatbox(text_sensor::TextSensor *t) { seatbox_ = t; }
  void set_handlebar(text_sensor::TextSensor *t) { handlebar_ = t; }
  void set_power_state(text_sensor::TextSensor *t) { power_state_ = t; }
  void set_power_mux(text_sensor::TextSensor *t) { power_mux_ = t; }
  void set_bat1_state(text_sensor::TextSensor *t) { bat1_state_ = t; }
  void set_bat2_state(text_sensor::TextSensor *t) { bat2_state_ = t; }
  void set_sw_mdb(text_sensor::TextSensor *t) { sw_mdb_ = t; }
  void set_sw_nrf(text_sensor::TextSensor *t) { sw_nrf_ = t; }
  void set_sw_dbc(text_sensor::TextSensor *t) { sw_dbc_ = t; }
  void set_sw_esp(text_sensor::TextSensor *t) { sw_esp_ = t; }
  void set_aux_charge(text_sensor::TextSensor *t) { aux_charge_ = t; }
  void set_cbb_charge(text_sensor::TextSensor *t) { cbb_charge_ = t; }
  void set_keycard_count(text_sensor::TextSensor *t) { keycard_count_ = t; }
  void set_cmd_response(text_sensor::TextSensor *t) { cmd_response_ = t; }
  void set_cmd_last_response(text_sensor::TextSensor *t) { cmd_last_response_ = t; }
  void set_ota_status(text_sensor::TextSensor *t) { ota_status_ = t; }
  void set_scooter_filter(const std::string &f) { scooter_filter_ = f; }
  void set_scooter_mac_sensor(text_sensor::TextSensor *t) { scooter_mac_sensor_ = t; }
  void set_ha_integration(binary_sensor::BinarySensor *b) { ha_integration_ = b; }
  void set_ota_eta(text_sensor::TextSensor *t) { ota_eta_ = t; }

  void set_blinker(LibrescootSelect *s) { blinker_ = s; }
  void set_usb_mode(LibrescootSelect *s) { usb_mode_ = s; }
  void set_link_mode(LibrescootSelect *s) { link_mode_ = s; }
  void set_ota_channel(LibrescootSelect *s) { ota_channel_ = s; }
  void set_ota_method(LibrescootSelect *s) { ota_method_ = s; }
  void set_ota_source_select(LibrescootSelect *s) { ota_source_select_ = s; }
  // Compile-time default OTA byte source ("github" = direct GitHub, "relay" = HA integration relay).
  // Codegen picks it by chip (S3 → github, else relay) unless the YAML sets ota_source:.
  void set_ota_source_default(const std::string &m) { this->ota_source_default_ = m; }

  void set_alarm_enabled(LibrescootSwitch *s) { alarm_enabled_ = s; }
  void set_pm_sched_hib(LibrescootSwitch *s) { pm_sched_hib_ = s; }
  void set_stage_only_switch(LibrescootSwitch *s) { stage_only_sw_ = s; }
  void set_auto_update_switch(LibrescootSwitch *s) { auto_update_sw_ = s; }

  void set_command_text(text::Text *t) { command_text_ = t; }
  void set_apn_text(text::Text *t) { apn_text_ = t; }
  void set_pm_cron_text(text::Text *t) { pm_cron_text_ = t; }
  void set_pm_duration_text(text::Text *t) { pm_duration_text_ = t; }
  void set_ota_source_url_text(text::Text *t) { ota_source_url_text_ = t; }
  void set_ota_version_text(text::Text *t) { ota_version_text_ = t; }
  void set_scooter_lock(LibrescootLock *l) { scooter_lock_ = l; }
  void set_mdb_update(LibrescootUpdate *u) { mdb_update_ = u; }
  void set_dbc_update(LibrescootUpdate *u) { dbc_update_ = u; }

  // Update checking (called from the update entity / periodic timer).
  void request_update_check();
  void perform_update(uint8_t component, bool force);  // 0 = MDB, 1 = DBC (one at a time)
  void update_perform(LibrescootUpdate *u, bool force);  // route an entity's Install to a component

  // BLE-OTA transfer engine.
  void set_stage_only(bool s) { this->stage_only_ = s; }
  void set_github_repo(const std::string &r) {
    this->github_repo_ = r;
    this->ota_source_url_ = "https://github.com/" + r + "/releases/download";
  }
  // Compile-time default for the OTA byte source (overrides the github_repo-derived default);
  // e.g. a local mirror. The runtime "OTA Source URL" text entity still overrides at runtime.
  void set_firmware_source(const std::string &url) {
    if (!url.empty())
      this->ota_source_url_ = url;
  }
  // TLS trust for the GitHub HTTPS requests, supplied from YAML instead of baked in:
  //   use_cert_bundle → the ESP-IDF Mozilla bundle (needs CONFIG_MBEDTLS_CERTIFICATE_BUNDLE);
  //   ca_certificate  → an explicit PEM root/chain; else the built-in GitHub roots are used.
  void set_ca_certificate(const std::string &pem) { this->ca_cert_ = pem; }
  void set_use_cert_bundle(bool b) { this->use_cert_bundle_ = b; }
  void set_update_check_interval(uint32_t ms) { this->update_check_interval_ = ms; }
  void set_link_interval_ms(uint32_t ms) { this->link_interval_ms_ = ms; }
  void set_auto_hold_ms(uint32_t ms) { this->auto_hold_ms_ = ms; }
  void ota_start(const std::string &url, const std::string &sha256_hex, uint32_t size,
                 const std::string &bundle_id, uint8_t component);
  void ota_user_abort();

 protected:
  // --- BLE plumbing ---
  time::RealTimeClock *time_{nullptr};
  bool enabled_{true};
  std::vector<CharEntry> chars_;
  bool ota_present_{false};
  bool read_in_flight_{false};
  uint32_t read_issued_ms_{0};
  uint32_t last_read_issue_ms_{0};
  uint32_t last_pres_pub_ms_{0};
  uint32_t last_adv_ms_{0};
  uint32_t last_rssi_pub_ms_{0};  // throttle advert-RSSI publishing for the configured scooter
  uint32_t last_rssi_ms_{0};

  void build_char_table_();
  CharEntry *find_char_(CharId id);
  void handle_char_value_(CharId id, uint8_t *data, uint16_t len);
  bool write_raw_(CharId id, const uint8_t *data, size_t len);  // gate: writes now, or defers via connect-on-demand
  bool write_now_(CharId id, const uint8_t *data, size_t len);  // actual GATT write (requires a live link)
  void queue_ondemand_(std::function<void()> action);
  void end_ondemand_();
  void write_str_(CharId id, const std::string &s);       // no terminator (control chars)
  void send_ext_query_(const std::string &q);             // ext cmd, null-terminated
  void run_command_(const std::string &cmd);              // ext cmd + start collecting
  void parse_cmd_response_(const std::string &line);
  void parse_ota_status_(uint8_t *data, uint16_t len);
  void mark_unknown_();
  void on_connected_();
  void refresh_();
  void apply_link_state_();
  void save_link_mode_();
  static std::string clean_str_(const uint8_t *data, uint16_t len);

  // --- GitHub update check ---
  void reflect_channel_(const std::string &mdb_version);  // OTA channel from the version prefix
  void set_mdb_version_(const std::string &v);
  void set_dbc_version_(const std::string &v);
  void publish_current_();  // combined installed version for the single update entity
  void github_fetch_();                    // runs in its own task (blocking HTTPS)
  static void github_task_(void *arg);
  // Stream a GitHub API GET through a per-byte sink (member so it applies the configured TLS).
  bool github_http_stream_(const std::string &url, const std::function<void(char)> &sink);
  // Apply the YAML-configured TLS trust (bundle / ca_certificate / built-in roots) to a config.
  void ota_http_tls_(esp_http_client_config_t *cfg);
  void apply_check_result_();              // main-loop: push task result to the entities
  void refresh_update_availability_();     // recompute MDB/DBC available from known versions
  void squelch_updates_();                 // park both update entities at "no update"
  void ota_auto_update_tick_();            // OTA Auto Update: install the next offered update / self-off
  std::string gh_successor_(const std::string &from) const;  // adjacent next release from gh_tags_
  bool ota_offers_allowed_() const;        // scooter in range AND a link mode that will connect
  void ota_prefill_version_text_(const std::string &tag);  // show the offered target in OTA Version

  // --- OTA transfer engine ---
  void ota_step_();                        // BLE consumer, driven from loop()
  void ota_publish_rates_();               // 10 s throughput/byte sensors, only while streaming
  void ota_park_rates_();                  // flush the last window and park the speed sensors at 0
  void ota_send_data_();                   // send windowed DATA chunks from the ring buffer
  void ota_handle_status_(uint8_t *data, uint16_t len);  // OTA_STATUS dispatch into the FSM
  bool ota_write_data_(uint32_t offset, const uint8_t *data, uint16_t len);
  void ota_send_start_();
  void ota_set_state_(OtaState s);
  bool ota_report_due_();  // true when the size-scaled status/log interval has elapsed
  void ota_fail_(const char *why);
  void ota_finish_(bool ok);               // free the buffer/task, reset the entity
  void ota_progress_();                    // publish transfer progress to the update entity
  static void ota_producer_task_(void *arg);
  void ota_producer_();                    // HTTP producer, runs in its own task
  // Install path: resolve the delta assets for the latest release, then transfer MDB + DBC.
  void ota_resolve_();                     // runs in its own task (GitHub asset metadata)
  // The release a .delta patches against; empty when it could not be determined. Blocking — worker
  // task only. Dispatches to whichever route the byte source offers: the Home Assistant relay reads
  // the archive itself (no cost here, works on any board), a direct GitHub source is read on-chip.
  std::string ota_delta_base_(const std::string &url);
  std::string ota_delta_base_relay_(const std::string &url);
  std::string ota_delta_base_onchip_(const std::string &url);
  static void ota_resolve_task_(void *arg);
  void ota_kick_next_job_();               // start the next queued transfer when idle
  void ota_begin_await_version_();         // after install: wait for the reboot + new version
  void ota_install_progress_();            // map the on-scooter install onto the progress bar
  void ota_handle_disconnect_();           // resume transfer / keep installing across a drop
  void ota_settle_update_entity_();        // re-evaluate available/not from known versions

  // --- extended-command response state ---
  std::string cmd_log_;
  bool cmd_collecting_{false};
  bool cmd_done_{false};
  int cmd_expected_lines_{0};
  int cmd_lines_seen_{0};
  uint32_t cmd_started_ms_{0};

  // --- on-connect query queue ---
  std::vector<std::string> pending_queries_;
  uint32_t next_query_ms_{0};

  // --- misc state ---
  bool present1_{false};
  bool present2_{false};
  bool ota_active_{false};
  uint32_t lock_suppress_until_{0};
  std::string link_mode_str_{"auto"};
  uint32_t yield_until_ms_{0};  // in "auto": hold the link released until this time (phone handoff)
  // "auto" proactive yield: hold a healthy link this long, then drop it briefly (the same
  // LINK_YIELD_MS window) so a phone / other central gets a turn. 0 = never yield proactively.
  uint32_t auto_hold_ms_{180000};
  uint32_t auto_hold_until_ms_{0};  // when the current auto hold expires and we proactively release
  ESPPreferenceObject link_pref_;
  // "interval" link mode: stay disconnected, but every link_interval_ms_ connect, refresh every
  // sensor once (on_connected_ forces a full read) for a short dwell, then release the link again.
  uint32_t link_interval_ms_{300000};  // default 5 min (YAML-configurable)
  bool interval_refreshing_{false};    // true during the connect+read dwell of an interval cycle
  uint32_t interval_next_ms_{0};       // when the next interval cycle may start
  uint32_t interval_release_at_ms_{0}; // when the current dwell ends and the link is released
  void service_link_interval_();       // scheduler tick that drives the interval cycle
  // OTA source URL persisted across reboots so the download source + integration-reachability check
  // survive an ESP-only restart (HA doesn't necessarily restart to re-push it).
  struct UrlPref {
    char url[192];
  };
  ESPPreferenceObject url_pref_;

  // --- connect-on-demand: run an action while normally disconnected ---
  std::vector<std::function<void()>> ondemand_actions_;
  bool ondemand_active_{false};
  bool ondemand_ran_{false};
  uint32_t ondemand_run_at_ms_{0};
  uint32_t ondemand_disc_at_ms_{0};
  uint32_t ondemand_deadline_ms_{0};

  // --- entity pointers ---
  sensor::Sensor *bat1_soc_{nullptr}, *bat1_cycles_{nullptr}, *bat2_soc_{nullptr}, *bat2_cycles_{nullptr};
  sensor::Sensor *aux_voltage_{nullptr}, *aux_level_{nullptr}, *cbb_level_{nullptr};
  sensor::Sensor *cbb_remaining_{nullptr}, *cbb_full_{nullptr}, *cbb_cell_{nullptr};
  sensor::Sensor *odometer_{nullptr}, *rssi_{nullptr};
  sensor::Sensor *ota_dl_speed_{nullptr}, *ota_ul_speed_{nullptr};
  sensor::Sensor *ota_ble_bytes_total_{nullptr}, *ota_target_transferred_{nullptr};
  sensor::Sensor *ota_selfheal_resumes_{nullptr};
  // Self-heal: auto-resume a transfer-phase failure from the staged offset instead of aborting.
  bool ota_auto_resume_{true};        // YAML ota_auto_resume (default on)
  uint16_t ota_selfheal_count_{0};    // consecutive auto-resumes (reset on real progress); bounded
  uint32_t ota_selfheal_at_ms_{0};    // earliest millis() to re-kick the job (backoff; 0 = none)
  uint32_t ota_selfheal_anchor_{0};   // ota_acked_ at the last streak reset (progress detector)
  double ota_selfheal_total_{0};      // lifetime auto-resumes (HA total_increasing)
  // Rolling-rate state for the OTA throughput sensors (10 s cadence, only while transferring).
  uint32_t ota_rate_ms_{0};         // last sample time; 0 = re-baseline on next tick (start/resume)
  uint32_t ota_rate_last_prod_{0};  // ota_produced_ snapshot at last sample
  uint32_t ota_rate_last_ack_{0};   // ota_acked_ snapshot at last sample
  double ota_ble_bytes_total_v_{0}; // lifetime acked bytes (monotonic; HA total_increasing)
  float ota_ul_ema_bpms_{0};        // EMA of the BLE upload rate (bytes/ms), drives OTA Upload ETA
  uint32_t ota_log_ms_{0};          // 1 s ESP-log cadence for live speeds (separate from the 10 s sensors)
  uint32_t ota_log_last_prod_{0}, ota_log_last_ack_{0};

  binary_sensor::BinarySensor *bat1_present_{nullptr}, *bat2_present_{nullptr};
  binary_sensor::BinarySensor *nav_active_{nullptr}, *ums_status_{nullptr}, *aux_charger_{nullptr};
  binary_sensor::BinarySensor *maps_available_{nullptr}, *nav_available_{nullptr};
  binary_sensor::BinarySensor *ble_connection_{nullptr}, *ble_presence_{nullptr};
  uint32_t presence_timeout_ms_{60000};  // "Home" if an advert was seen within this window
  binary_sensor::BinarySensor *passkey_required_{nullptr};
  binary_sensor::BinarySensor *pairing_required_sensor_{nullptr};
  // Pairing gate: the ESP NEVER completes a fresh pairing on its own. A PASSKEY_REQ (= the scooter
  // wants to bond, because there's no valid bond) is only honoured while pairing is *armed* — armed
  // deliberately via the Pair button. Otherwise it's refused and the link is released
  // (pairing_blocked_) so the scooter isn't spammed with pairing requests. Arming is a time WINDOW,
  // not a one-shot flag: it must survive reconnects/timeouts while the user reads the code off the
  // dashboard and enters it (a bool cleared on the first failed attempt broke exactly this). Each
  // PASSKEY_REQ during an armed window extends it; a successful bond or the window expiring ends it.
  uint32_t pair_arm_until_ms_{0};
  bool pairing_armed_() const { return this->pair_arm_until_ms_ != 0 && millis() < this->pair_arm_until_ms_; }
  bool pairing_blocked_{false};
  binary_sensor::BinarySensor *reboot_required_{nullptr};

  text_sensor::TextSensor *status_{nullptr}, *seatbox_{nullptr}, *handlebar_{nullptr}, *power_state_{nullptr};
  text_sensor::TextSensor *power_mux_{nullptr}, *bat1_state_{nullptr}, *bat2_state_{nullptr};
  text_sensor::TextSensor *sw_mdb_{nullptr}, *sw_nrf_{nullptr}, *sw_dbc_{nullptr}, *sw_esp_{nullptr};
  text_sensor::TextSensor *aux_charge_{nullptr}, *cbb_charge_{nullptr}, *keycard_count_{nullptr};
  text_sensor::TextSensor *cmd_response_{nullptr}, *cmd_last_response_{nullptr}, *ota_status_{nullptr};
  text_sensor::TextSensor *ota_eta_{nullptr};
  // Scooter discovery: nearby BLE adverts whose name matches scooter_filter_, published as
  // "MAC@rssi,MAC@rssi,…" for the HA integration's new-scooter picker. Cleared on entering scan.
  text_sensor::TextSensor *scooter_mac_sensor_{nullptr};  // the configured scooter MAC, always shown
  std::string scooter_filter_{"scooter"};
  std::map<uint64_t, int> discovered_;  // MAC → best RSSI
  uint32_t discovered_pub_ms_{0};
  // Scan results are POSTed to the HA integration's HTTP listener — NO HA entity for the raw list.
  //   POST http://<host>:<port>/scan/<secret>   (derived from ota_source_url_ = .../ota/<secret>)
  std::string disc_body_;
  volatile bool disc_post_running_{false};
  void publish_discovered_();          // build disc_body_, then POST it (throttled, scan mode only)
  void post_discovered_();
  static void post_discovered_task_(void *arg);
  // On = the Home Assistant "Librescoot-BLE-Client" integration is active — detected by pinging
  // its plain-HTTP OTA relay (the http:// OTA Source URL it configures). Drives the OTA-source
  // capability: without the relay, this board must download from GitHub itself.
  binary_sensor::BinarySensor *ha_integration_{nullptr};
  bool hi_last_{false};
  uint32_t hi_next_check_ms_{0};
  uint32_t ota_hint_last_ms_{0};        // throttle the "no OTA source" log hint
  volatile bool integration_reachable_{false};
  volatile bool hi_running_{false};
  volatile bool hi_done_{false};
  void check_integration_();            // blocking HTTP HEAD on the relay (own task)
  static void check_integration_task_(void *arg);
  void request_integration_check_();
  // Scheduler-driven (set_interval) so it runs even while BLEClientBase has disabled our loop()
  // during an idle/disconnected BLE link: consumes a finished probe + schedules the next one.
  void service_integration_check_();
  // Scheduler-driven too: raise "pairing required" only while the CONFIGURED scooter is actually
  // in range (advertising) but we could not bond it (pairing refused/failed → pairing_blocked_).
  void service_pairing_watch_();
  // Can this board actually fetch the firmware bytes? A plain-HTTP source (HA relay / mirror)
  // needs that server reachable; an https source (GitHub) needs a working TLS path (cert bundle,
  // i.e. a PSRAM board — the classic can't do the RSA-4096 CDN handshake).
  bool ota_download_capable_() {
    if (this->ota_source_url_.rfind("http://", 0) == 0)
      return this->integration_reachable_;
    return this->use_cert_bundle_ || !this->ca_cert_.empty();
  }

  LibrescootSelect *blinker_{nullptr}, *usb_mode_{nullptr}, *link_mode_{nullptr}, *ota_channel_{nullptr};
  LibrescootSelect *ota_method_{nullptr};
  LibrescootSelect *ota_source_select_{nullptr};
  // OTA byte source: "github" (direct) or "relay" (HA integration). Persisted in NVS; the select,
  // the YAML default and the HA config flow all set it. github mode owns ota_source_url_ (= the
  // github base); relay mode expects the HA integration to set the URL.
  std::string ota_source_mode_{"relay"};
  std::string ota_source_default_{"relay"};
  ESPPreferenceObject ota_source_pref_;
  // Measured BLE upload throughput, used to estimate the transfer time BEFORE one starts. Seeded
  // with a conservative default and refined from every transfer that ran long enough to be
  // representative, so the estimate adapts to this board's range and byte source. NVS-persisted.
  float ota_rate_bps_{OTA_DEFAULT_RATE_BPS};
  ESPPreferenceObject ota_rate_pref_;
  void ota_learn_rate_(uint32_t moved, uint32_t dt_ms);
  std::string github_base_() const { return "https://github.com/" + this->github_repo_ + "/releases/download"; }
  void apply_ota_source_(const std::string &mode, bool persist);  // set mode → url, publish, persist
  std::string ota_method_str_{"delta"};  // delta (default) or full (.mender image)
  LibrescootSwitch *alarm_enabled_{nullptr}, *pm_sched_hib_{nullptr};
  LibrescootSwitch *stage_only_sw_{nullptr}, *auto_update_sw_{nullptr};
  text::Text *command_text_{nullptr}, *apn_text_{nullptr}, *pm_cron_text_{nullptr}, *pm_duration_text_{nullptr};
  text::Text *ota_source_url_text_{nullptr};
  text::Text *ota_version_text_{nullptr};  // prefilled with the offered target while none is pinned
  LibrescootLock *scooter_lock_{nullptr};
  LibrescootUpdate *mdb_update_{nullptr}, *dbc_update_{nullptr};
  LibrescootUpdate *ota_active_update_{nullptr};  // the component's entity currently installing
  uint8_t ota_install_component_{0};              // 0 = MDB, 1 = DBC for the running install

  // --- update-check state ---
  std::string mdb_version_;
  std::string dbc_version_;
  std::string channel_{"nightly"};        // reflected from the running MDB version
  std::string gh_channel_;                // snapshot for the worker task
  std::string gh_current_tag_;            // installed MDB version (tag form) snapshot, for the range
  std::string gh_current_dbc_tag_;        // installed DBC version (tag form) snapshot, for the range
  std::string gh_method_;                 // "delta"/"full" snapshot for the worker
  std::string gh_latest_tag_;             // channel latest (result written by the task)
  std::string gh_summary_;                // legacy aggregate (installed MDB → latest); kept for compat
  // Per-component target + changelog computed by the worker: delta → the NEXT release (successor of
  // that component's installed version) with its single changelog; full → the channel latest with the
  // aggregated (all-in-between) changelog. Empty target = nothing newer for that component.
  std::string gh_mdb_target_, gh_dbc_target_;
  std::string gh_mdb_summary_, gh_dbc_summary_;
  // The tag each worker-fetched changelog + size actually belongs to. refresh only shows the
  // changelog/size when this matches the (live, current-method) target — otherwise the body could be
  // for a different release (e.g. the worker ran while the DBC read "unknown" and fetched the latest).
  std::string gh_mdb_summary_tag_, gh_dbc_summary_tag_;
  // Channel tags from the last check (any order). Retained so refresh_update_availability_ computes
  // the per-component TARGET for the CURRENT method immediately, independent of the worker's method
  // snapshot (delta → adjacent successor, full → latest). Same worker→main handoff as gh_summary_.
  std::vector<std::string> gh_tags_;
  uint32_t gh_mdb_size_{0}, gh_dbc_size_{0};  // asset sizes for each component's target (summary line)
  // refresh_update_availability_ decision, cached for the auto-update tick (single source of truth).
  bool ota_mdb_avail_{false}, ota_dbc_avail_{false};
  // OTA Auto Update: install every offered update unattended, then switch itself off when up to date.
  bool ota_auto_update_{false};
  bool ota_auto_check_pending_{false};    // an auto-install fired; wait for a fresh check before "done"
  uint32_t ota_auto_next_ms_{0};          // periodic auto-update re-evaluation while the switch is on
  uint32_t next_avail_refresh_ms_{0};     // periodic refresh so a stale changelog/target self-corrects
  uint32_t sum_recheck_after_ms_{0};      // rate limit for changelog-mismatch rechecks (GitHub 60/h)
  volatile bool gh_running_{false};
  volatile bool gh_done_{false};
  volatile bool gh_ok_{false};
  uint32_t next_check_ms_{0};

  // --- OTA transfer state ---
  bool stage_only_{false};  // build-time test flag: when true, transfers stop before COMPLETE
  uint16_t att_mtu_{23};
  OtaState ota_state_{OtaState::IDLE};
  std::string ota_url_;        // original URL (re-resolved on resume; signed redirects expire)
  std::string ota_bundle_id_;
  uint8_t ota_component_{0};
  uint8_t ota_sha_[32]{};
  uint32_t ota_total_{0};
  uint16_t ota_chunk_{240};
  uint16_t ota_window_chunks_{64};
  uint8_t ota_ack_every_{16};
  uint8_t *ota_buf_{nullptr};
  uint32_t ota_cap_{0};
  volatile uint32_t ota_produced_{0};   // bytes fetched into the ring (producer writes)
  volatile uint32_t ota_acked_{0};      // cumulative acked bytes (consumer writes; ring floor)
  uint32_t ota_sent_{0};                // bytes handed to OTA_DATA
  uint32_t ota_resume_{0};
  volatile bool ota_producer_run_{false};
  volatile bool ota_producer_done_{false};
  volatile bool ota_http_ok_{false};
  bool ota_congested_{false};
  int ota_rewinds_{0};
  uint32_t ota_state_ms_{0};             // for phase timeouts
  uint32_t ota_last_ack_ms_{0};
  uint32_t ota_start_ms_{0};             // throughput
  uint32_t ota_last_report_ms_{0};       // status/log throttle (interval scales with file size)
  bool ota_report_now_{true};            // decided per OTA_STATUS notification
  uint32_t ota_last_send_ms_{0};         // pacing: Bluedroid gives no write-no-rsp backpressure
  uint16_t ota_send_gap_ms_{6};          // min ms between DATA chunks (adaptive on rewinds)

  // --- install path (perform): resolve delta assets, then transfer MDB then DBC ---
  // Byte source base; metadata (size/sha) always comes from the GitHub API. Point this at a
  // local HTTP mirror to drop TLS from the bulk transfer. Layout: <base>/<tag>/<asset_name>.
  std::string github_repo_{"librescoot/librescoot"};   // owner/name, configurable in YAML
  std::string ota_source_url_{"https://github.com/librescoot/librescoot/releases/download"};
  std::string ca_cert_;                   // YAML PEM root(s) for HTTPS; empty = built-in GitHub roots
  bool use_cert_bundle_{false};           // use the ESP-IDF Mozilla cert bundle instead of a pinned cert
  std::string ota_target_version_;        // OTA Version text: empty = latest, else a specific tag
  uint32_t update_check_interval_{6UL * 3600 * 1000};  // how often to poll GitHub for a release
  struct OtaJob {
    std::string url, sha, bundle_id;
    uint32_t size;
    uint8_t component;
  };
  std::vector<OtaJob> ota_jobs_;
  std::string rs_tag_, rs_source_;       // snapshot for the resolve task
  volatile bool ota_resolve_running_{false};
  volatile bool ota_resolve_done_{false};
  volatile bool ota_cancel_{false};      // abort requested while a resolve is in flight
  bool rs_mdb_ok_{false}, rs_dbc_ok_{false};
  // The release a resolved .delta patches against, read from the archive before transferring
  // it. Empty when the target is a full image or the base could not be read.
  std::string rs_delta_base_;
  // After the last component's install is queued, stay "installing" until the scooter reboots
  // and BLE reports the new version — or the user aborts the wait.
  bool ota_awaiting_version_{false};
  std::string ota_await_tag_;
  uint32_t ota_await_until_ms_{0};
  // Last INSTALL_PROGRESS phase the scooter reported (0xFF = not asked yet). Updates are only
  // offered when this is 0x06 (idle); any other value — including unknown — suppresses them and
  // shows the ongoing status instead. Requested via STATUS_REQ on every connect.
  uint8_t ota_scooter_phase_{0xFF};
  // Stuck-pending-reboot watchdog: when the phase has been pending-reboot (0x02) continuously for
  // >20 min, raise reboot_required_ so the HA integration can offer a manual restart. Re-requests
  // STATUS_REQ every 30 s while pending so a clear (or a genuinely-needed reboot) is caught.
  uint32_t ota_pending_since_ms_{0};
  uint32_t ota_pending_last_req_ms_{0};
  bool reboot_problem_{false};
  // Progress bar spans transfer + the (~10 min, two sub-phase) on-scooter install.
  float ota_transfer_share_{0.5f};   // fraction of the bar the upload gets (by size vs install)
  uint8_t ota_install_pct_{0};       // last INSTALL_PROGRESS percent (resets between sub-phases)
  uint8_t ota_install_sub_{0};       // 0 = verify/prepare, 1 = install (second 0-100% pass)
  OtaJob ota_current_job_{};         // the running job, re-queued to resume across a disconnect
  bool ota_have_current_job_{false};
  int ota_resume_count_{0};          // capped so a truly dead link can't reconnect-loop forever
  uint32_t boot_reread_at_ms_{0};    // re-read all sensors 30 s after "booting" is seen
  std::string rs_mdb_name_, rs_mdb_sha_, rs_dbc_name_, rs_dbc_sha_;
  uint32_t rs_mdb_size_{0}, rs_dbc_size_{0};
};

template<typename... Ts> class PasskeyReplyAction : public Action<Ts...>, public Parented<LibrescootBleClient> {
 public:
  TEMPLATABLE_VALUE(uint32_t, passkey)
  void play(const Ts &...x) override { this->parent_->passkey_reply(this->passkey_.value(x...)); }
};

// Test/engine entry point: transfer an arbitrary URL to the scooter over BLE-OTA. Lets the
// state machine be exercised against a local `python -m http.server` before GitHub/TLS.
template<typename... Ts> class OtaTransferAction : public Action<Ts...>, public Parented<LibrescootBleClient> {
 public:
  TEMPLATABLE_VALUE(std::string, url)
  TEMPLATABLE_VALUE(std::string, sha256)
  TEMPLATABLE_VALUE(uint32_t, size)
  TEMPLATABLE_VALUE(std::string, bundle_id)
  TEMPLATABLE_VALUE(uint8_t, component)
  void play(const Ts &...x) override {
    this->parent_->ota_start(this->url_.value(x...), this->sha256_.value(x...), this->size_.value(x...),
                             this->bundle_id_.value(x...), this->component_.value(x...));
  }
};

template<typename... Ts> class OtaAbortAction : public Action<Ts...>, public Parented<LibrescootBleClient> {
 public:
  void play(const Ts &...x) override { this->parent_->ota_user_abort(); }
};

}  // namespace librescoot_ble_client
}  // namespace esphome

#endif  // USE_ESP32
