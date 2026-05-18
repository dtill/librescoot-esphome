// LSC (Librescoot) Battery NFC interface — encapsulated ESPHome component.
//
// Mirrors the battery-service Go FSM
// (https://github.com/librescoot/battery-service/blob/main/battery/fsm/state_machine.go)
// at a useful fidelity: heartbeat cycle = CMD_SEATBOX_CLOSED + CMD_ON/OFF
// every 10 s, seatbox state drives the heartbeat command flavour,
// just-inserted wake-up sequence (OPENED + INSERTED with longer delays),
// CheckStateCorrect mechanism (re-send CMD_INSERTED when BMS state doesn't
// match the user's enabled flag).
//
// PN532 access — the upstream esphome::pn532::PN532 component keeps the
// NTAG read/write primitives we need behind `protected`. The .cpp uses the
// standard C++ "Rob" idiom to capture pointers-to-member of those methods
// without modifying the upstream class.
#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/pn532/pn532.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace lsc_battery_nfc {

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

// NTAG page addresses on the NT3H2111 surface that the BMS exposes.
static constexpr uint8_t PAGE_MFR     = 0x04;  // manufacturer / factory metadata
static constexpr uint8_t PAGE_STATUS0 = 0xC0;
static constexpr uint8_t PAGE_STATUS1 = 0xC4;
static constexpr uint8_t PAGE_STATUS2 = 0xC8;
static constexpr uint8_t PAGE_COMMAND = 0xCC;

// Known BMS state words at STATUS1 bytes[0..3] (little-endian u32).
static constexpr uint32_t BMS_STATE_ASLEEP = 0xA4983474u;
static constexpr uint32_t BMS_STATE_IDLE   = 0xB9164828u;
static constexpr uint32_t BMS_STATE_ACTIVE = 0xC6583518u;

// 4-byte commands written to PAGE_COMMAND (little-endian on the wire).
static constexpr uint32_t CMD_ON                = 0x50505050u;
static constexpr uint32_t CMD_OFF               = 0xCAFEF00Du;
static constexpr uint32_t CMD_INSERTED          = 0x44414E41u;
static constexpr uint32_t CMD_SEATBOX_OPENED    = 0x48525259u;
static constexpr uint32_t CMD_SEATBOX_CLOSED    = 0x4D4B4D4Bu;
static constexpr uint32_t CMD_HEARTBEAT_SCOOTER = 0x534E4A41u;
// CMD_HEARTBEAT_SCOOTER is empirically required to keep this battery in
// BMS_STATE_ACTIVE between heartbeat cycles. Without it the BMS falls back
// to idle ~5 s after CMD_ON, and a 10 s heartbeat interval leaves 5 s of
// idle each cycle. The Go FSM doesn't appear to use it, but a real scooter
// may be drawing current (which itself extends the active window) or the
// Go FSM heartbeat fires faster than we modelled.

// Wake-up timing per Go battery-service types.go (BMSTimeCmd*).
static constexpr uint32_t T_CMD_MS                 = 400;
static constexpr uint32_t T_CMD_FIRST_OPEN_AWAKE_MS  = 2000;
static constexpr uint32_t T_CMD_FIRST_OPEN_ASLEEP_MS = 3000;

// ---------------------------------------------------------------------------
// Battery telemetry snapshot (parsed STATUS0/1/2)
// ---------------------------------------------------------------------------

struct BatteryStatus {
  bool valid{false};
  uint32_t state_raw{0};
  const char *state_name{"unknown"};

  // STATUS0
  uint16_t voltage_mv{0};
  int16_t  current_ma{0};
  std::string fw_version;
  uint16_t remaining_capacity{0};
  uint16_t full_capacity{0};
  uint8_t  charge_pct{0};
  uint16_t fault_code{0};
  int8_t   temp0{0};
  int8_t   temp1{0};
  uint8_t  state_of_health{0};
  bool     low_soc{false};

  // STATUS1 + STATUS2 (serial spans both)
  std::string serial_number;

  // STATUS2
  std::string manufacturing_date;
  uint16_t cycle_count{0};
  int8_t   temp2{0};
  int8_t   temp3{0};
};

const char *fault_description(uint16_t code);

// ---------------------------------------------------------------------------
// FSM
// ---------------------------------------------------------------------------

enum class State : uint8_t {
  INIT,            // Component starting up, waiting for first poll
  DISCOVERING,     // No tag in field (or last poll didn't find one)
  TAG_VALIDATING,  // Tag just arrived, doing battery validation read
  TAG_FOREIGN,     // Confirmed: tag in field but not an LSC battery
  TAG_BATTERY,     // Confirmed battery — normal heartbeat / status loop
  WAKEUP,          // Active battery, just inserted with seatbox open
                   //   — runs SEATBOX_OPENED + INSERTED with longer delays
  DUMP_RUNNING,    // Diagnostic full-memory dump in progress
};

// Sub-state for the seatbox-open maintenance loop. Mirrors the Go FSM
// branch StateCondOff → StateSendOff → StateSendOpened → StateSendInsertedOpen
// (loops back to StateSendOpened). Each step is a single PN532 write per
// update() tick — 2 s ticks vs the Go FSM's ~400 ms timers, but the BMS
// still sees the canonical OPENED/INSERTED sequence that triggers its
// seatbox-open behaviour (e.g. the light bar on the battery).
enum class SeatboxOpenStep : uint8_t {
  NONE,           // Seatbox closed → normal heartbeat path is active.
  DEACTIVATE,     // First tick after open: send CMD_OFF if battery is active.
  FIRST_OPENED,   // Send CMD_SEATBOX_OPENED — the BMS's "the seat just opened" trigger.
  LOOP_INSERTED,  // Maintenance loop: send CMD_INSERTED.
  LOOP_OPENED,    // Maintenance loop: send CMD_SEATBOX_OPENED.
};

const char *state_to_string(State s);

// ---------------------------------------------------------------------------
// Main component
// ---------------------------------------------------------------------------

class LSCBatteryNFC : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ----- Configuration setters (called from generated code) ---------------
  void set_pn532(pn532::PN532 *p) { this->pn532_ = p; }
  void set_heartbeat_interval(uint32_t ms) { this->heartbeat_interval_ms_ = ms; }
  void set_keep_active_on_seatbox_open(bool k) { this->keep_active_on_seatbox_open_ = k; }

  // ----- Sensor setters ----------------------------------------------------
  void set_voltage_sensor(sensor::Sensor *s) { this->voltage_ = s; }
  void set_current_sensor(sensor::Sensor *s) { this->current_ = s; }
  void set_level_sensor(sensor::Sensor *s) { this->level_ = s; }
  void set_health_sensor(sensor::Sensor *s) { this->health_ = s; }
  void set_cycle_count_sensor(sensor::Sensor *s) { this->cycle_count_ = s; }
  void set_remaining_capacity_sensor(sensor::Sensor *s) { this->remaining_capacity_ = s; }
  void set_full_capacity_sensor(sensor::Sensor *s) { this->full_capacity_ = s; }
  void set_fault_code_sensor(sensor::Sensor *s) { this->fault_code_ = s; }
  void set_temp_0_sensor(sensor::Sensor *s) { this->temp_0_ = s; }
  void set_temp_1_sensor(sensor::Sensor *s) { this->temp_1_ = s; }
  void set_temp_2_sensor(sensor::Sensor *s) { this->temp_2_ = s; }
  void set_temp_3_sensor(sensor::Sensor *s) { this->temp_3_ = s; }

  void set_state_text_sensor(text_sensor::TextSensor *t) { this->state_text_ = t; }
  void set_fault_description_text_sensor(text_sensor::TextSensor *t) { this->fault_description_ = t; }
  void set_serial_text_sensor(text_sensor::TextSensor *t) { this->serial_ = t; }
  void set_firmware_text_sensor(text_sensor::TextSensor *t) { this->firmware_ = t; }
  void set_manufacturing_date_text_sensor(text_sensor::TextSensor *t) { this->manufacturing_date_ = t; }
  void set_manufacturer_text_sensor(text_sensor::TextSensor *t) { this->manufacturer_ = t; }

  void set_tag_present_binary_sensor(binary_sensor::BinarySensor *b) { this->tag_present_ = b; }
  void set_battery_detected_binary_sensor(binary_sensor::BinarySensor *b) { this->battery_detected_ = b; }
  void set_low_soc_binary_sensor(binary_sensor::BinarySensor *b) { this->low_soc_ = b; }

  void set_high_current_switch(switch_::Switch *s) { this->high_current_switch_ = s; }
  void set_seatbox_switch(switch_::Switch *s) { this->seatbox_switch_ = s; }
  void set_full_dump_button(button::Button *b) { this->full_dump_button_ = b; }
  void set_manual_refresh_button(button::Button *b) { this->manual_refresh_button_ = b; }

  // ----- Callbacks from sub-entities ---------------------------------------
  void on_high_current_command(bool on);   // user toggled high-current switch
  void on_seatbox_command(bool closed);    // user toggled seatbox switch
  void on_full_dump_pressed();
  void on_manual_refresh_pressed();

 protected:
  // ----- External dependencies ---------------------------------------------
  pn532::PN532 *pn532_{nullptr};

  // ----- Config ------------------------------------------------------------
  uint32_t heartbeat_interval_ms_{10000};
  bool keep_active_on_seatbox_open_{false};

  // ----- Sub-entity pointers (all optional) --------------------------------
  sensor::Sensor *voltage_{nullptr};
  sensor::Sensor *current_{nullptr};
  sensor::Sensor *level_{nullptr};
  sensor::Sensor *health_{nullptr};
  sensor::Sensor *cycle_count_{nullptr};
  sensor::Sensor *remaining_capacity_{nullptr};
  sensor::Sensor *full_capacity_{nullptr};
  sensor::Sensor *fault_code_{nullptr};
  sensor::Sensor *temp_0_{nullptr};
  sensor::Sensor *temp_1_{nullptr};
  sensor::Sensor *temp_2_{nullptr};
  sensor::Sensor *temp_3_{nullptr};

  text_sensor::TextSensor *state_text_{nullptr};
  text_sensor::TextSensor *fault_description_{nullptr};
  text_sensor::TextSensor *serial_{nullptr};
  text_sensor::TextSensor *firmware_{nullptr};
  text_sensor::TextSensor *manufacturing_date_{nullptr};
  text_sensor::TextSensor *manufacturer_{nullptr};

  binary_sensor::BinarySensor *tag_present_{nullptr};
  binary_sensor::BinarySensor *battery_detected_{nullptr};
  binary_sensor::BinarySensor *low_soc_{nullptr};

  switch_::Switch *high_current_switch_{nullptr};
  switch_::Switch *seatbox_switch_{nullptr};
  button::Button  *full_dump_button_{nullptr};
  button::Button  *manual_refresh_button_{nullptr};

  // ----- FSM state ---------------------------------------------------------
  State state_{State::INIT};
  bool enabled_{false};         // user wants high-current ON
  bool seatbox_closed_{true};   // current seatbox lock state
  bool just_inserted_{false};   // tag-arrival latch, cleared after wake-up
  std::string current_uid_;     // UID we last successfully validated
  std::string cached_mfr_uid_;  // UID whose manufacturer string we cached
  uint32_t last_heartbeat_ms_{0};
  uint32_t presence_fail_count_{0};
  uint32_t status_fail_count_{0};
  uint32_t state_mismatch_count_{0};  // consecutive heartbeats where actual != enabled
  SeatboxOpenStep seatbox_open_step_{SeatboxOpenStep::NONE};
  uint32_t seatbox_open_step_at_ms_{0};
  uint32_t base_update_interval_ms_{2000};  // captured in setup() from YAML; restored on heartbeat resume

  // Dedup caches — only publish_state() when value actually changed.
  uint16_t L_voltage_{0xFFFF};
  int16_t  L_current_{0x7FFF};
  uint8_t  L_level_{0xFF};
  uint8_t  L_health_{0xFF};
  uint16_t L_cycle_count_{0xFFFF};
  uint16_t L_remcap_{0xFFFF};
  uint16_t L_fullcap_{0xFFFF};
  uint16_t L_fault_{0xFFFF};
  int8_t   L_t0_{0x7F}, L_t1_{0x7F}, L_t2_{0x7F}, L_t3_{0x7F};
  int      L_lowsoc_{-1};
  std::string L_state_, L_serial_, L_mfg_, L_fw_, L_faulttext_;

  // Dump progress
  uint8_t  dump_next_page_{0};   // 0 = idle
  uint8_t  dump_attempts_{0};
  bool     dump_started_{false};

  // ----- Internal helpers --------------------------------------------------

  // PN532-level (defined in .cpp using Rob accessors).
  bool poll_for_tag_(uint32_t timeout_ms, std::vector<uint8_t> &resp);
  bool ntag_read_block_(uint8_t page, std::vector<uint8_t> &out);
  bool ntag_write_page_(uint8_t page, const uint8_t *data4);
  bool write_command_raw_(uint32_t cmd);
  void rf_off_();
  void reset_current_uid_();
  void send_abort_();

  // Battery-level.
  bool read_all_status_(BatteryStatus &out);
  bool is_battery_state_(uint32_t s) const {
    return s == BMS_STATE_ASLEEP || s == BMS_STATE_IDLE || s == BMS_STATE_ACTIVE;
  }
  void publish_status_(const BatteryStatus &s);
  void read_manufacturer_once_(const std::string &uid);

  // FSM step entry points.
  void fsm_handle_no_tag_();
  void fsm_handle_tag_(const std::string &uid);
  void run_heartbeat_cycle_(const BatteryStatus &s);     // CLOSED + ON/OFF
  void run_wakeup_cycle_(const BatteryStatus &s);        // OPENED + INSERTED
  void run_seatbox_open_loop_(const BatteryStatus &s);   // OFF / OPENED / INSERTED maintenance

  // Dynamically swap polling interval between heartbeat rate (~2 s) and the
  // faster seatbox-open maintenance rate (~800 ms, closer to the Go FSM's
  // 400 ms cycle). Calls stop_poller/start_poller so the change takes effect
  // immediately, not on the next reboot.
  void set_polling_rate_(uint32_t interval_ms);

  // Dump (batched, fresh-inlist retry).
  void run_dump_batch_();

  // Utility.
  std::string uid_to_string_(const std::vector<uint8_t> &poll_resp) const;
};

// ---------------------------------------------------------------------------
// Sub-entity classes — write_state / press calls back into the parent.
// ---------------------------------------------------------------------------

class LSCHighCurrentSwitch : public switch_::Switch,
                              public Parented<LSCBatteryNFC> {
 protected:
  void write_state(bool state) override {
    this->publish_state(state);
    this->parent_->on_high_current_command(state);
  }
};

class LSCSeatboxSwitch : public switch_::Switch,
                         public Parented<LSCBatteryNFC> {
  // Restore mode is set declaratively in __init__.py via
  // default_restore_mode="RESTORE_DEFAULT_ON" — constructor-side
  // set_restore_mode() was getting silently overridden by the YAML default.

 protected:
  void write_state(bool state) override {
    this->publish_state(state);
    this->parent_->on_seatbox_command(state);
  }
};

class LSCFullDumpButton : public button::Button,
                          public Parented<LSCBatteryNFC> {
 protected:
  void press_action() override { this->parent_->on_full_dump_pressed(); }
};

class LSCManualRefreshButton : public button::Button,
                                public Parented<LSCBatteryNFC> {
 protected:
  void press_action() override { this->parent_->on_manual_refresh_pressed(); }
};

}  // namespace lsc_battery_nfc
}  // namespace esphome
