#include "lsc_battery_nfc.h"

#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace lsc_battery_nfc {

static const char *const TAG      = "lsc_battery_nfc";
static const char *const TAG_DUMP = "lsc_battery_nfc.full_dump";

// ---------------------------------------------------------------------------
// "Rob" idiom — capture pointers-to-member of protected pn532::PN532 methods
// without modifying the upstream class. Standard C++ trick: explicit template
// instantiation does not enforce access checks on the template arguments.
// ---------------------------------------------------------------------------

namespace rob {

template<typename Tag, typename Tag::type M>
struct Accessor {
  friend typename Tag::type lsc_rob_ptr(Tag) { return M; }
};

struct TWrite { using type = bool (pn532::PN532::*)(const std::vector<uint8_t> &); friend type lsc_rob_ptr(TWrite); };
template struct Accessor<TWrite, &pn532::PN532::write_command_>;

struct TResp  { using type = bool (pn532::PN532::*)(uint8_t, std::vector<uint8_t> &); friend type lsc_rob_ptr(TResp); };
template struct Accessor<TResp,  &pn532::PN532::read_response>;

struct TRdy   { using type = bool (pn532::PN532::*)(); friend type lsc_rob_ptr(TRdy); };
template struct Accessor<TRdy,   &pn532::PN532::is_read_ready>;

struct TAck   { using type = void (pn532::PN532::*)(); friend type lsc_rob_ptr(TAck); };
template struct Accessor<TAck,   &pn532::PN532::send_ack_>;

struct TReadU { using type = bool (pn532::PN532::*)(uint8_t, uint16_t, std::vector<uint8_t> &); friend type lsc_rob_ptr(TReadU); };
template struct Accessor<TReadU, &pn532::PN532::read_mifare_ultralight_bytes_>;

struct TWriteU { using type = bool (pn532::PN532::*)(uint8_t, const uint8_t *, size_t); friend type lsc_rob_ptr(TWriteU); };
template struct Accessor<TWriteU, &pn532::PN532::write_mifare_ultralight_page_>;

struct TUid   { using type = nfc::NfcTagUid pn532::PN532::*; friend type lsc_rob_ptr(TUid); };
template struct Accessor<TUid,   &pn532::PN532::current_uid_>;

}  // namespace rob

// ---------------------------------------------------------------------------
// Fault-code description (mirrors battery-service/battery/fault.go strings)
// ---------------------------------------------------------------------------

const char *fault_description(uint16_t code) {
  switch (code) {
    case 0:  return "OK";
    case 1:  return "High temperature during charging";
    case 2:  return "Low temperature during charging";
    case 3:  return "High temperature during discharge";
    case 4:  return "Low temperature during discharge";
    case 5:  return "Signal wire disconnected";
    case 6:  return "Critical temperature level";
    case 7:  return "Battery pack overvoltage";
    case 8:  return "Power transistor overheating";
    case 9:  return "Cell overvoltage";
    case 10: return "Battery pack undervoltage";
    case 11: return "Cell undervoltage";
    case 12: return "Charging overcurrent";
    case 13: return "Discharge overcurrent";
    case 14: return "Short circuit detected";
    case 15: return "Reserved fault 1";
    case 16: return "Reserved fault 2";
    case 32: return "Battery not responding to commands";
    case 33: return "Battery data unavailable";
    case 34: return "Battery communication failed";
    case 35: return "NFC reader malfunction";
    default: return "Unknown fault";
  }
}

const char *state_to_string(State s) {
  switch (s) {
    case State::INIT:           return "INIT";
    case State::DISCOVERING:    return "DISCOVERING";
    case State::TAG_VALIDATING: return "TAG_VALIDATING";
    case State::TAG_FOREIGN:    return "TAG_FOREIGN";
    case State::TAG_BATTERY:    return "TAG_BATTERY";
    case State::WAKEUP:         return "WAKEUP";
    case State::DUMP_RUNNING:   return "DUMP_RUNNING";
  }
  return "?";
}

static const char *bms_state_name(uint32_t s) {
  switch (s) {
    case BMS_STATE_ASLEEP: return "asleep";
    case BMS_STATE_IDLE:   return "idle";
    case BMS_STATE_ACTIVE: return "active";
    default:               return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void LSCBatteryNFC::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LSC Battery NFC...");
  if (this->pn532_ == nullptr) {
    ESP_LOGE(TAG, "PN532 reference is null — check pn532_id in YAML");
    this->mark_failed();
    return;
  }
  // Initialize tag-present / battery-detected to OFF until first poll.
  if (this->tag_present_ != nullptr) this->tag_present_->publish_state(false);
  if (this->battery_detected_ != nullptr) this->battery_detected_->publish_state(false);

  // Capture the YAML-configured update interval so we can restore it after a
  // seatbox-open loop ends (we drop to ~800 ms during the loop to match the
  // Go FSM's faster command cadence, which keeps the battery's light bar lit
  // continuously instead of dimming every command swap).
  this->base_update_interval_ms_ = this->get_update_interval();

  // Sync internal FSM flags FROM the switch state — the switch is the user-
  // facing source of truth and is restored from flash (or the YAML-default
  // restore mode) before our setup runs (we use setup_priority::LATE).
  // Without this, internal defaults could disagree with the UI: e.g. heart-
  // beat says "seatbox=closed" while the switch displays OFF.
  if (this->seatbox_switch_ != nullptr) {
    this->seatbox_closed_ = this->seatbox_switch_->state;
    ESP_LOGCONFIG(TAG, "  Seatbox initial state: %s",
                  this->seatbox_closed_ ? "CLOSED" : "OPEN");
  }
  if (this->high_current_switch_ != nullptr) {
    this->enabled_ = this->high_current_switch_->state;
    ESP_LOGCONFIG(TAG, "  High-current initial state: %s",
                  this->enabled_ ? "ON" : "OFF");
  }

  // If the restored switch state says seatbox is open, prime the seatbox-
  // open maintenance loop so we don't enter heartbeat with a stale state.
  // Also switch to the faster polling rate so the OPENED/INSERTED loop
  // runs at Go-FSM-like cadence (light bar stays lit instead of dimming).
  if (!this->seatbox_closed_ && !this->keep_active_on_seatbox_open_) {
    this->seatbox_open_step_ = SeatboxOpenStep::DEACTIVATE;
    this->seatbox_open_step_at_ms_ = millis();
    this->set_polling_rate_(800);
  }

  this->state_ = State::DISCOVERING;
}

void LSCBatteryNFC::dump_config() {
  ESP_LOGCONFIG(TAG, "LSC Battery NFC:");
  ESP_LOGCONFIG(TAG, "  Heartbeat interval: %u ms", this->heartbeat_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Keep active on seatbox open: %s",
                this->keep_active_on_seatbox_open_ ? "YES" : "NO");
  LOG_UPDATE_INTERVAL(this);
}

void LSCBatteryNFC::update() {
  if (this->is_failed()) return;
  if (this->pn532_ == nullptr) return;

  // Diagnostic dump runs in its own state and consumes the whole update
  // cycle (no status reads, no heartbeats during dump).
  if (this->state_ == State::DUMP_RUNNING) {
    this->run_dump_batch_();
    return;
  }

  // Poll PN532 for a tag.
  std::vector<uint8_t> poll_resp;
  if (!this->poll_for_tag_(500, poll_resp)) {
    this->fsm_handle_no_tag_();
    this->rf_off_();
    return;
  }

  std::string uid = this->uid_to_string_(poll_resp);
  this->fsm_handle_tag_(uid);
  this->rf_off_();
}

// ---------------------------------------------------------------------------
// PN532 primitives (using rob:: accessors)
// ---------------------------------------------------------------------------

bool LSCBatteryNFC::poll_for_tag_(uint32_t timeout_ms, std::vector<uint8_t> &resp) {
  auto write_fn = lsc_rob_ptr(rob::TWrite{});
  auto read_fn  = lsc_rob_ptr(rob::TResp{});
  auto rdy_fn   = lsc_rob_ptr(rob::TRdy{});

  if (!(this->pn532_->*write_fn)({0x4A, 0x01, 0x00})) return false;

  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    if ((this->pn532_->*rdy_fn)()) {
      resp.clear();
      if (!(this->pn532_->*read_fn)(0x4A, resp)) return false;
      return !resp.empty() && resp[0] >= 1;
    }
    delay(5);
  }
  this->send_abort_();
  return false;
}

bool LSCBatteryNFC::ntag_read_block_(uint8_t page, std::vector<uint8_t> &out) {
  auto fn = lsc_rob_ptr(rob::TReadU{});
  for (int attempt = 0; attempt < 2; attempt++) {
    out.clear();
    if ((this->pn532_->*fn)(page, 16, out) && out.size() >= 16) return true;
    delay(3);
  }
  return false;
}

bool LSCBatteryNFC::ntag_write_page_(uint8_t page, const uint8_t *data4) {
  auto fn = lsc_rob_ptr(rob::TWriteU{});
  return (this->pn532_->*fn)(page, data4, 4);
}

bool LSCBatteryNFC::write_command_raw_(uint32_t cmd) {
  uint8_t b[4] = {
      uint8_t(cmd & 0xFFu),
      uint8_t((cmd >> 8) & 0xFFu),
      uint8_t((cmd >> 16) & 0xFFu),
      uint8_t((cmd >> 24) & 0xFFu),
  };
  return this->ntag_write_page_(PAGE_COMMAND, b);
}

void LSCBatteryNFC::rf_off_() {
  auto write_fn = lsc_rob_ptr(rob::TWrite{});
  auto read_fn  = lsc_rob_ptr(rob::TResp{});
  if (!(this->pn532_->*write_fn)({0x32, 0x01, 0x00})) return;
  delay(2);
  std::vector<uint8_t> resp;
  (this->pn532_->*read_fn)(0x32, resp);
}

void LSCBatteryNFC::reset_current_uid_() {
  auto field = lsc_rob_ptr(rob::TUid{});
  (this->pn532_->*field).clear();
}

void LSCBatteryNFC::send_abort_() {
  auto fn = lsc_rob_ptr(rob::TAck{});
  (this->pn532_->*fn)();
}

// ---------------------------------------------------------------------------
// Battery-level helpers
// ---------------------------------------------------------------------------

bool LSCBatteryNFC::read_all_status_(BatteryStatus &out) {
  std::vector<uint8_t> s0, s1, s2;
  if (!this->ntag_read_block_(PAGE_STATUS0, s0)) return false;
  if (!this->ntag_read_block_(PAGE_STATUS1, s1)) return false;
  if (!this->ntag_read_block_(PAGE_STATUS2, s2)) return false;

  // STATUS0 layout per battery-service/battery/battery_status.go:39-65
  out.voltage_mv         = uint16_t(s0[0]) | (uint16_t(s0[1]) << 8);
  out.current_ma         = int16_t(uint16_t(s0[2]) | (uint16_t(s0[3]) << 8));
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u", unsigned(s0[4]), unsigned(s0[5]));
    out.fw_version = buf;
  }
  out.remaining_capacity = uint16_t(s0[6]) | (uint16_t(s0[7]) << 8);
  out.full_capacity      = uint16_t(s0[8]) | (uint16_t(s0[9]) << 8);
  if (out.full_capacity == 0) {
    out.charge_pct = 0;
  } else {
    uint32_t pct = (uint32_t(out.remaining_capacity) * 100u + out.full_capacity / 2u)
                   / out.full_capacity;
    out.charge_pct = uint8_t(pct > 100u ? 100u : pct);
  }
  out.fault_code      = uint16_t(s0[10]) | (uint16_t(s0[11]) << 8);
  out.temp0           = int8_t(s0[12]);
  out.temp1           = int8_t(s0[13]);
  out.state_of_health = s0[14];
  out.low_soc         = s0[15] != 0;

  // STATUS1
  out.state_raw  = uint32_t(s1[0]) | (uint32_t(s1[1]) << 8)
                 | (uint32_t(s1[2]) << 16) | (uint32_t(s1[3]) << 24);
  out.state_name = bms_state_name(out.state_raw);
  out.serial_number.assign(reinterpret_cast<const char *>(&s1[4]), 12);

  // STATUS2
  out.serial_number.append(reinterpret_cast<const char *>(&s2[0]), 4);
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%c%c%c%c-%c%c-%c%c",
             s2[4], s2[5], s2[6], s2[7], s2[8], s2[9], s2[10], s2[11]);
    out.manufacturing_date = buf;
  }
  out.cycle_count = uint16_t(s2[12]) | (uint16_t(s2[13]) << 8);
  out.temp2       = int8_t(s2[14]);
  out.temp3       = int8_t(s2[15]);

  out.valid = true;
  return true;
}

void LSCBatteryNFC::publish_status_(const BatteryStatus &s) {
  // Dedup helpers — only call publish_state when the value actually changed.
  // ESPHome's API state-push log fires on every publish_state regardless of
  // change, so naive publishing produces a wall of [S] lines per poll.
#define PUB_NUM(SENS, CACHE, VAL) do { \
  if (this->SENS != nullptr) { \
    auto v_ = (VAL); \
    if (v_ != this->CACHE) { this->SENS->publish_state(v_); this->CACHE = v_; } \
  } } while (0)
#define PUB_STR(SENS, CACHE, VAL) do { \
  if (this->SENS != nullptr) { \
    const std::string &v_ = (VAL); \
    if (v_ != this->CACHE) { this->SENS->publish_state(v_); this->CACHE = v_; } \
  } } while (0)

  PUB_NUM(voltage_,            L_voltage_,    s.voltage_mv);
  PUB_NUM(current_,            L_current_,    s.current_ma);
  PUB_NUM(level_,              L_level_,      s.charge_pct);
  PUB_NUM(health_,             L_health_,     s.state_of_health);
  PUB_NUM(cycle_count_,        L_cycle_count_, s.cycle_count);
  PUB_NUM(remaining_capacity_, L_remcap_,     s.remaining_capacity);
  PUB_NUM(full_capacity_,      L_fullcap_,    s.full_capacity);
  PUB_NUM(fault_code_,         L_fault_,      s.fault_code);
  PUB_NUM(temp_0_,             L_t0_,         s.temp0);
  PUB_NUM(temp_1_,             L_t1_,         s.temp1);
  PUB_NUM(temp_2_,             L_t2_,         s.temp2);
  PUB_NUM(temp_3_,             L_t3_,         s.temp3);

  if (this->low_soc_ != nullptr) {
    int v = s.low_soc ? 1 : 0;
    if (v != this->L_lowsoc_) {
      this->low_soc_->publish_state(s.low_soc);
      this->L_lowsoc_ = v;
    }
  }

  std::string state_str = s.state_name;
  PUB_STR(state_text_,         L_state_,      state_str);
  PUB_STR(serial_,             L_serial_,     s.serial_number);
  PUB_STR(firmware_,           L_fw_,         s.fw_version);
  PUB_STR(manufacturing_date_, L_mfg_,        s.manufacturing_date);
  std::string ft = fault_description(s.fault_code);
  PUB_STR(fault_description_,  L_faulttext_,  ft);

#undef PUB_NUM
#undef PUB_STR

  ESP_LOGD(TAG,
           "BATTERY STATE=%s V=%.3fV I=%dmA LVL=%u%% RCAP=%umAh SOH=%u%% CYCLES=%u SN=%s FAULT=%s(%u)",
           s.state_name,
           s.voltage_mv / 1000.0f,
           s.current_ma,
           s.charge_pct,
           s.remaining_capacity,
           s.state_of_health,
           s.cycle_count,
           s.serial_number.c_str(),
           fault_description(s.fault_code),
           s.fault_code);
}

void LSCBatteryNFC::read_manufacturer_once_(const std::string &uid) {
  if (this->manufacturer_ == nullptr) return;
  if (uid == this->cached_mfr_uid_) return;
  std::vector<uint8_t> mfr;
  if (!this->ntag_read_block_(PAGE_MFR, mfr)) return;
  std::string s(reinterpret_cast<const char *>(mfr.data()), 16);
  auto last = s.find_last_not_of(' ');
  if (last == std::string::npos) s.clear(); else s.erase(last + 1);
  this->manufacturer_->publish_state(s);
  this->cached_mfr_uid_ = uid;
}

// ---------------------------------------------------------------------------
// FSM
// ---------------------------------------------------------------------------

void LSCBatteryNFC::fsm_handle_no_tag_() {
  if (this->tag_present_ != nullptr && this->tag_present_->state) {
    this->presence_fail_count_++;
    if (this->presence_fail_count_ >= 3) {
      ESP_LOGI(TAG, "tag departed");
      if (this->tag_present_ != nullptr) this->tag_present_->publish_state(false);
      if (this->battery_detected_ != nullptr) this->battery_detected_->publish_state(false);
      this->state_ = State::DISCOVERING;
      this->current_uid_.clear();
      this->cached_mfr_uid_.clear();
      this->just_inserted_ = false;
    }
  } else {
    this->state_ = State::DISCOVERING;
  }
}

void LSCBatteryNFC::fsm_handle_tag_(const std::string &uid) {
  this->presence_fail_count_ = 0;
  if (this->tag_present_ != nullptr) this->tag_present_->publish_state(true);

  // New UID detection — anything other than the same UID we last validated
  // resets validation state and sets the just-inserted flag.
  if (uid != this->current_uid_) {
    ESP_LOGI(TAG, "new tag UID=%s", uid.c_str());
    this->current_uid_ = uid;
    this->just_inserted_ = true;
    this->state_ = State::TAG_VALIDATING;
  }

  // Always try a full status read — succeeds only on real batteries.
  BatteryStatus s;
  if (!this->read_all_status_(s)) {
    this->status_fail_count_++;
    if (this->status_fail_count_ >= 3) {
      ESP_LOGW(TAG, "3 consecutive status read failures");
      // Treat tag as foreign / unreadable
      this->state_ = State::TAG_FOREIGN;
      if (this->battery_detected_ != nullptr) this->battery_detected_->publish_state(false);
    }
    return;
  }
  this->status_fail_count_ = 0;

  if (!this->is_battery_state_(s.state_raw)) {
    if (this->state_ != State::TAG_FOREIGN) {
      ESP_LOGW(TAG, "Tag UID=%s is not an LSC battery — writes inhibited", uid.c_str());
    }
    this->state_ = State::TAG_FOREIGN;
    if (this->battery_detected_ != nullptr) this->battery_detected_->publish_state(false);
    return;
  }

  // Battery confirmed.
  if (this->battery_detected_ != nullptr) this->battery_detected_->publish_state(true);
  this->publish_status_(s);
  this->read_manufacturer_once_(uid);

  // Dispatch heartbeat or wake-up.
  const bool battery_active = (s.state_raw == BMS_STATE_ACTIVE);
  const bool seatbox_open_path = !this->seatbox_closed_ && !this->keep_active_on_seatbox_open_;

  if (this->just_inserted_ && seatbox_open_path && !battery_active) {
    this->state_ = State::WAKEUP;
    this->run_wakeup_cycle_(s);
    this->just_inserted_ = false;
  } else if (this->seatbox_open_step_ != SeatboxOpenStep::NONE) {
    // Seatbox-open maintenance loop is active — runs INSTEAD of heartbeat
    // (matches Go FSM: while in the seatbox-open branch, the heartbeat
    // path is not entered). When the user closes the seatbox again,
    // on_seatbox_command resets the step to NONE and heartbeat resumes.
    this->state_ = State::TAG_BATTERY;
    this->run_seatbox_open_loop_(s);
    this->just_inserted_ = false;
  } else {
    this->state_ = State::TAG_BATTERY;
    this->run_heartbeat_cycle_(s);
    // Clear the just-inserted latch once the tag has cleanly handed off to
    // the heartbeat path. Otherwise a later seatbox-open toggle would re-
    // trigger run_wakeup_cycle_ even though the battery is long-stabilised.
    this->just_inserted_ = false;
  }
}

void LSCBatteryNFC::run_heartbeat_cycle_(const BatteryStatus &s) {
  const uint32_t now = millis();
  if (this->last_heartbeat_ms_ != 0 &&
      (now - this->last_heartbeat_ms_) < this->heartbeat_interval_ms_) {
    return;  // not yet time
  }

  // Mirrors Go FSM StateHeartbeatActions: SEND_CLOSED → SEND_ON_OFF →
  // CheckStateCorrect → optional SEND_INSERTED_CLOSED on mismatch. We
  // run all sub-steps in a single update() so the chip stays selected.
  ESP_LOGD(TAG, "heartbeat cycle (seatbox=%s, enabled=%s)",
           this->seatbox_closed_ ? "closed" : "open",
           this->enabled_ ? "on" : "off");

  // Step 1: seatbox state command (CLOSED or OPENED — drives BMS view).
  uint32_t cmd_seatbox = this->seatbox_closed_ ? CMD_SEATBOX_CLOSED : CMD_SEATBOX_OPENED;
  bool ok = this->write_command_raw_(cmd_seatbox);
  ESP_LOGD(TAG, "  cmd 0x%08X (%s) -> %s",
           cmd_seatbox,
           this->seatbox_closed_ ? "SEATBOX_CLOSED" : "SEATBOX_OPENED",
           ok ? "OK" : "FAIL");
  delay(T_CMD_MS);

  // Step 2: ON or OFF — effective intent. Seatbox-open is a safety override
  // per Go FSM (state_machine.go:540-557, transition guard at :663-668):
  // opening the seatbox while the battery is active forces CMD_OFF unless
  // keep_active_on_seatbox_open is set. The user's `enabled_` flag is
  // preserved (UI switch unchanged) so the battery resumes automatically
  // when the seatbox closes again.
  bool effective_on = this->enabled_;
  if (!this->seatbox_closed_ && !this->keep_active_on_seatbox_open_) {
    effective_on = false;
    if (this->enabled_) {
      ESP_LOGD(TAG, "  seatbox-open safety override — forcing CMD_OFF (UI intent stays ON)");
    }
  }

  uint32_t cmd_onoff = effective_on ? CMD_ON : CMD_OFF;
  ok = this->write_command_raw_(cmd_onoff);
  ESP_LOGI(TAG, "  cmd 0x%08X (%s) -> %s",
           cmd_onoff,
           effective_on ? "ON" : "OFF",
           ok ? "OK" : "FAIL");

  // Step 3: HEARTBEAT_SCOOTER keep-alive, but ONLY when effectively ON.
  // Without this, BMS_STATE_ACTIVE decays back to idle ~5 s after CMD_ON,
  // leaving half of every 10 s heartbeat cycle in idle state. When OFF
  // we must NOT send it — empirically HEARTBEAT_SCOOTER keeps the battery
  // in its current state, so sending it after CMD_OFF would lock the BMS
  // into active forever, and the CheckStateCorrect recovery (INSERTED)
  // can never bring it to idle.
  if (effective_on) {
    delay(T_CMD_MS);
    ok = this->write_command_raw_(CMD_HEARTBEAT_SCOOTER);
    ESP_LOGD(TAG, "  cmd 0x%08X (HEARTBEAT_SCOOTER) -> %s",
             CMD_HEARTBEAT_SCOOTER, ok ? "OK" : "FAIL");
  }

  this->last_heartbeat_ms_ = now;

  // Step 4: CheckStateCorrect. The state in `s` was read BEFORE this
  // heartbeat's writes — it's authoritative for whether the previous
  // heartbeat held. We only act on persistent mismatch (≥ 2 consecutive)
  // because one cycle of disagreement is normal right after a user toggle:
  // the read on the cycle that observes the new effective state reflects
  // the pre-toggle state. Compare against `effective_on`, not `enabled_`,
  // so we correctly expect idle while seatbox is open.
  bool expect_active = effective_on;
  bool currently_active = (s.state_raw == BMS_STATE_ACTIVE);
  if (expect_active != currently_active) {
    this->state_mismatch_count_++;
    if (this->state_mismatch_count_ >= 2) {
      ESP_LOGW(TAG,
               "Persistent state mismatch (expect=%s, actual=%s, %u cycles) — re-poking with INSERTED",
               expect_active ? "active" : "idle",
               s.state_name,
               this->state_mismatch_count_);
      delay(T_CMD_MS);
      this->write_command_raw_(CMD_INSERTED);
    }
  } else {
    this->state_mismatch_count_ = 0;
  }
}

void LSCBatteryNFC::run_wakeup_cycle_(const BatteryStatus &s) {
  ESP_LOGI(TAG, "wake-up cycle (just inserted, seatbox open, battery state=%s)",
           s.state_name);
  // Mirrors Go FSM StateSendOpened → StateSendInsertedOpen.
  uint32_t opened_delay = (s.state_raw == BMS_STATE_ASLEEP)
                              ? T_CMD_FIRST_OPEN_ASLEEP_MS
                              : T_CMD_FIRST_OPEN_AWAKE_MS;

  bool ok = this->write_command_raw_(CMD_SEATBOX_OPENED);
  ESP_LOGI(TAG, "  cmd 0x%08X (SEATBOX_OPENED) -> %s",
           CMD_SEATBOX_OPENED, ok ? "OK" : "FAIL");
  delay(opened_delay);

  ok = this->write_command_raw_(CMD_INSERTED);
  ESP_LOGI(TAG, "  cmd 0x%08X (INSERTED) -> %s",
           CMD_INSERTED, ok ? "OK" : "FAIL");

  // Reset heartbeat timer so heartbeat doesn't fire immediately after.
  this->last_heartbeat_ms_ = millis();
}

// ---------------------------------------------------------------------------
// Seatbox-open maintenance loop
// ---------------------------------------------------------------------------
//
// Mirrors Go FSM state_machine.go branch:
//   StateCondJustInserted -> StateCondOff -> StateSendOff (if active)
//                                         -> StateSendOpened (with justOpened
//                                            timer = 2 s) -> StateSendInsertedOpen
//                                         -> loop back to StateSendOpened
//
// Each step runs as a single command per update() tick (2 s interval), so
// the canonical OPENED ↔ INSERTED alternation that triggers the BMS
// seatbox-open behaviour (e.g. the battery's light bar) still reaches it,
// just at a slower cadence than the Go FSM's ~400 ms timers.

void LSCBatteryNFC::run_seatbox_open_loop_(const BatteryStatus &s) {
  const bool battery_active = (s.state_raw == BMS_STATE_ACTIVE);

  switch (this->seatbox_open_step_) {
    case SeatboxOpenStep::DEACTIVATE: {
      // Safety-critical: re-send CMD_OFF every tick UNTIL the BMS read
      // confirms idle. Mirrors Go FSM StateCondOff (state_machine.go:541-557):
      // if IsInactive() returns false it loops back to StateSendOff, only
      // advancing to StateSendOpened once the battery is actually off.
      //
      // Without this loop the previous implementation sent CMD_OFF once
      // then immediately started spamming OPENED/INSERTED, which on this
      // battery kept the BMS active for ~12 s (vs the bare-CMD_OFF latency
      // of ~2 s). With the loop, OPENED/INSERTED only start after we have
      // observed idle.
      if (battery_active) {
        bool ok = this->write_command_raw_(CMD_OFF);
        ESP_LOGI(TAG, "seatbox-open init: cmd 0x%08X (OFF) -> %s (waiting for idle)",
                 CMD_OFF, ok ? "OK" : "FAIL");
        // Stay in DEACTIVATE for the next tick.
      } else {
        ESP_LOGI(TAG, "seatbox-open: battery confirmed idle, advancing to OPENED/INSERTED loop");
        this->seatbox_open_step_ = SeatboxOpenStep::FIRST_OPENED;
        this->seatbox_open_step_at_ms_ = millis();
      }
      break;
    }

    case SeatboxOpenStep::FIRST_OPENED: {
      // First CMD_SEATBOX_OPENED. Go FSM uses a 2 s timer here when justOpened
      // is true (BMSTimeCmdFirstOpenedAwake); our 2 s update interval gives
      // us roughly that delay before the next step.
      bool ok = this->write_command_raw_(CMD_SEATBOX_OPENED);
      ESP_LOGI(TAG, "seatbox-open: cmd 0x%08X (SEATBOX_OPENED, first) -> %s",
               CMD_SEATBOX_OPENED, ok ? "OK" : "FAIL");
      this->seatbox_open_step_ = SeatboxOpenStep::LOOP_INSERTED;
      this->seatbox_open_step_at_ms_ = millis();
      break;
    }

    case SeatboxOpenStep::LOOP_INSERTED: {
      bool ok = this->write_command_raw_(CMD_INSERTED);
      ESP_LOGD(TAG, "seatbox-open loop: cmd 0x%08X (INSERTED) -> %s",
               CMD_INSERTED, ok ? "OK" : "FAIL");
      this->seatbox_open_step_ = SeatboxOpenStep::LOOP_OPENED;
      this->seatbox_open_step_at_ms_ = millis();
      break;
    }

    case SeatboxOpenStep::LOOP_OPENED: {
      bool ok = this->write_command_raw_(CMD_SEATBOX_OPENED);
      ESP_LOGD(TAG, "seatbox-open loop: cmd 0x%08X (SEATBOX_OPENED) -> %s",
               CMD_SEATBOX_OPENED, ok ? "OK" : "FAIL");
      this->seatbox_open_step_ = SeatboxOpenStep::LOOP_INSERTED;
      this->seatbox_open_step_at_ms_ = millis();
      break;
    }

    case SeatboxOpenStep::NONE:
      // Dispatched in error; nothing to do.
      break;
  }
}

// ---------------------------------------------------------------------------
// Full dump (batched, fresh-inlist retry per page)
// ---------------------------------------------------------------------------

void LSCBatteryNFC::run_dump_batch_() {
  // Re-inlist the tag (we own polling here).
  std::vector<uint8_t> poll_resp;
  if (!this->poll_for_tag_(500, poll_resp)) {
    ESP_LOGW(TAG_DUMP, "no tag — cancelling dump");
    this->rf_off_();
    this->dump_next_page_ = 0;
    this->dump_attempts_ = 0;
    this->dump_started_ = false;
    this->state_ = State::DISCOVERING;
    return;
  }

  if (!this->dump_started_) {
    std::string uid = this->uid_to_string_(poll_resp);
    ESP_LOGI(TAG_DUMP, "heartbeat + status reads suppressed during dump");
    ESP_LOGI(TAG_DUMP, "=== NFC dump  UID=%s  pages 0x04..0xE0 ===", uid.c_str());
    this->dump_started_ = true;
  }

  uint8_t start_page = this->dump_next_page_;
  bool batch_complete = true;
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t page = start_page + i * 4;
    if (page > 0xE0) break;

    std::vector<uint8_t> blk;
    if (!this->ntag_read_block_(page, blk)) {
      // Bail this batch; next update() gets fresh inlist.
      this->dump_attempts_++;
      if (this->dump_attempts_ >= 5) {
        ESP_LOGW(TAG_DUMP, "[0x%02X..0x%02X]  READ FAILED (5 fresh-inlist retries, skipping)",
                 page, page + 3);
        this->dump_next_page_ = page + 4;
        this->dump_attempts_ = 0;
      } else {
        this->dump_next_page_ = page;
      }
      batch_complete = false;
      break;
    }

    char asc[17];
    for (int j = 0; j < 16; j++) {
      asc[j] = (blk[j] >= 0x20 && blk[j] < 0x7F) ? char(blk[j]) : '.';
    }
    asc[16] = 0;
    ESP_LOGI(TAG_DUMP,
             "[0x%02X] %02X %02X %02X %02X  %02X %02X %02X %02X  "
             "%02X %02X %02X %02X  %02X %02X %02X %02X  |%s|",
             page,
             blk[0],  blk[1],  blk[2],  blk[3],
             blk[4],  blk[5],  blk[6],  blk[7],
             blk[8],  blk[9],  blk[10], blk[11],
             blk[12], blk[13], blk[14], blk[15], asc);
  }

  if (batch_complete) {
    this->dump_next_page_ = start_page + 12;
  }

  if (this->dump_next_page_ > 0xE0) {
    ESP_LOGI(TAG_DUMP, "=== END DUMP ===");
    this->dump_next_page_ = 0;
    this->dump_attempts_ = 0;
    this->dump_started_ = false;
    this->state_ = State::DISCOVERING;  // resume normal polling next update()
  }

  this->rf_off_();
}

// ---------------------------------------------------------------------------
// Sub-entity callbacks
// ---------------------------------------------------------------------------

void LSCBatteryNFC::on_high_current_command(bool on) {
  this->enabled_ = on;
  ESP_LOGI(TAG, "user request: high-current %s", on ? "ON" : "OFF");
  // Force a heartbeat cycle on the next update by resetting the timer.
  this->last_heartbeat_ms_ = 0;
}

void LSCBatteryNFC::on_seatbox_command(bool closed) {
  if (this->seatbox_closed_ == closed) return;
  this->seatbox_closed_ = closed;
  ESP_LOGI(TAG, "user request: seatbox %s", closed ? "CLOSED" : "OPEN");

  if (!closed && !this->keep_active_on_seatbox_open_) {
    // Enter the seatbox-open maintenance loop on the next update tick.
    // First step (DEACTIVATE) sends CMD_OFF only if the battery is active.
    this->seatbox_open_step_ = SeatboxOpenStep::DEACTIVATE;
    this->seatbox_open_step_at_ms_ = millis();
    ESP_LOGI(TAG, "  entering seatbox-open maintenance loop");
    // Speed up polling so the OPENED/INSERTED alternation is fast enough
    // to keep the battery's light bar lit continuously (Go FSM ≈ 400 ms
    // per command; we use 800 ms as a balance with PN532 I2C load).
    this->set_polling_rate_(800);
  } else if (closed) {
    // Exit the loop, resume normal heartbeat. Reset the heartbeat timer so
    // the next update fires CLOSED + ON immediately.
    if (this->seatbox_open_step_ != SeatboxOpenStep::NONE) {
      ESP_LOGI(TAG, "  exiting seatbox-open loop, resuming heartbeat");
      this->set_polling_rate_(this->base_update_interval_ms_);
    }
    this->seatbox_open_step_ = SeatboxOpenStep::NONE;
  }
  // keep_active_on_seatbox_open=true falls through to the existing safety-
  // override path in run_heartbeat_cycle_ (no loop, normal heartbeat with
  // OPENED command flavour).

  this->last_heartbeat_ms_ = 0;
}

void LSCBatteryNFC::set_polling_rate_(uint32_t interval_ms) {
  if (this->get_update_interval() == interval_ms) return;
  ESP_LOGD(TAG, "  polling rate: %u ms -> %u ms",
           this->get_update_interval(), interval_ms);
  // stop_poller + start_poller (both protected on PollingComponent) is the
  // documented way to re-register the scheduler interval; simply calling
  // set_update_interval() does not affect the already-armed timer.
  this->stop_poller();
  this->set_update_interval(interval_ms);
  this->start_poller();
}

void LSCBatteryNFC::on_full_dump_pressed() {
  if (this->state_ == State::DUMP_RUNNING) {
    ESP_LOGW(TAG, "dump already in progress");
    return;
  }
  ESP_LOGI(TAG, "full dump queued — pages 0x04..0xE0");
  this->dump_next_page_ = 0x04;
  this->dump_attempts_ = 0;
  this->dump_started_ = false;
  this->state_ = State::DUMP_RUNNING;
}

void LSCBatteryNFC::on_manual_refresh_pressed() {
  ESP_LOGI(TAG, "manual refresh requested");
  this->current_uid_.clear();   // force re-validation
  this->last_heartbeat_ms_ = 0;  // force heartbeat next update
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

std::string LSCBatteryNFC::uid_to_string_(const std::vector<uint8_t> &poll_resp) const {
  // PN532 InListPassiveTarget response layout:
  //   [0] num_targets, [1] target_no, [2-3] SENS_RES, [4] SEL_RES, [5] uid_len, [6..] UID
  if (poll_resp.size() < 7) return "";
  uint8_t uid_len = poll_resp[5];
  if (poll_resp.size() < uint16_t(6 + uid_len)) return "";
  char buf[64];
  int p = 0;
  for (uint8_t i = 0; i < uid_len && p < 60; i++) {
    if (i > 0) buf[p++] = '-';
    p += snprintf(buf + p, sizeof(buf) - p, "%02X", poll_resp[6 + i]);
  }
  buf[p] = 0;
  return std::string(buf);
}

}  // namespace lsc_battery_nfc
}  // namespace esphome
