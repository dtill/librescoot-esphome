// BLE-OTA firmware transfer engine (GATT service 9a590500).
// Protocol: https://reference.librescoot.org/dev/bluetooth/ota-transfer/
//
// An HTTP producer task fills a ring buffer; the BLE consumer (driven from loop()) streams
// [offset][chunk] on OTA_DATA, honouring the window/ack_every/rewind negotiated in START_ACK.
// `stage_only` stops after the DATA phase so nothing is installed.
#include "librescoot_ble_client.h"
#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <cstdlib>
#include <cstring>

#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "miniz.h"  // tinfl_* live in ROM on both ESP32 and ESP32-S3 — inflate costs no flash
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "github_ca.h"

namespace esphome {
namespace librescoot_ble_client {

static const char *const OTAG = "ota";

static const char *comp_name(uint8_t c) { return c == 0x01 ? "DBC" : "MDB"; }

static uint16_t u16le(const uint8_t *v) { return (uint16_t) v[0] | ((uint16_t) v[1] << 8); }
static uint32_t rd_u32le(const uint8_t *v) {
  return (uint32_t) v[0] | ((uint32_t) v[1] << 8) | ((uint32_t) v[2] << 16) | ((uint32_t) v[3] << 24);
}

// TLS trust for the GitHub HTTPS requests, chosen from the YAML config. Single source of truth
// used by both the update-check stream and the OTA asset resolve/download.
void LibrescootBleClient::ota_http_tls_(esp_http_client_config_t *cfg) {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  if (this->use_cert_bundle_) {
    cfg->crt_bundle_attach = esp_crt_bundle_attach;  // ESP-IDF Mozilla bundle (enabled in YAML)
    return;
  }
#endif
  // Explicit PEM from YAML, or the two pinned GitHub roots as the built-in fallback default.
  cfg->cert_pem = this->ca_cert_.empty() ? GITHUB_ROOTS : this->ca_cert_.c_str();
}

// ---------------------------------------------------------------------------
// entry points
// ---------------------------------------------------------------------------
// Self-heal bounds: a recoverable transfer-phase failure (rewinds/stall/timeout) auto-resumes from
// the staged offset after a backoff, instead of aborting the whole update. The consecutive count is
// bounded so a truly dead link eventually gives up, but it resets after real forward progress, so a
// long transfer over a flaky link keeps healing as long as it keeps moving.
static constexpr uint16_t OTA_SELFHEAL_MAX = 30;              // consecutive auto-resumes before giving up
static constexpr uint32_t OTA_SELFHEAL_BACKOFF_MS = 15000;    // wait between auto-resumes (avoid hot-loop)
static constexpr uint32_t OTA_SELFHEAL_PROGRESS_RESET = 262144;  // 256 KB of new data clears the count

void LibrescootBleClient::ota_start(const std::string &url, const std::string &sha256_hex, uint32_t size,
                                const std::string &bundle_id, uint8_t component) {
  if (this->ota_state_ != OtaState::IDLE) {
    ESP_LOGW(OTAG, "transfer already in progress");
    return;
  }
  if (this->state() != espbt::ClientState::ESTABLISHED) {
    ESP_LOGW(OTAG, "not connected; connect first");
    return;
  }
  auto *data = this->find_char_(CharId::OTA_DATA);
  auto *ctrl = this->find_char_(CharId::OTA_CONTROL);
  if (data == nullptr || data->handle == 0 || ctrl == nullptr || ctrl->handle == 0) {
    ESP_LOGW(OTAG, "OTA service not present on this firmware");
    return;
  }
  if (size == 0 || bundle_id.empty() || bundle_id.size() > 64) {
    ESP_LOGW(OTAG, "bad parameters (size=%u, bundle_id='%s')", (unsigned) size, bundle_id.c_str());
    return;
  }

  this->ota_url_ = url;
  this->ota_bundle_id_ = bundle_id;
  this->ota_component_ = component;
  this->ota_total_ = size;
  this->ota_rewinds_ = 0;
  this->ota_congested_ = false;
  this->ota_sent_ = this->ota_acked_ = this->ota_produced_ = 0;
  this->ota_rate_ms_ = 0;  // re-baseline the throughput sensors on the first streaming tick
  this->ota_ul_ema_bpms_ = 0;  // reset the ETA's smoothed upload rate for the new/resumed transfer
  this->ota_log_ms_ = 0;       // re-baseline the 1 s speed log
  this->ota_start_ms_ = this->ota_last_ack_ms_ = this->ota_last_send_ms_ = 0;
  this->ota_last_report_ms_ = 0;
  // Pacing is learned per link, not per session: a resume of the same job keeps the gap the
  // previous attempts backed off to, so a weak link is not re-flooded at full speed every time.
  // A fresh transfer starts from what the signal suggests — 6 ms saturates a good link, but at
  // weak RSSI that burst is dropped on the air, and since the losses show up as a dead link rather
  // than a REWIND, starting fast means never converging.
  if (this->ota_selfheal_count_ == 0)
    this->ota_send_gap_ms_ = (this->last_rssi_dbm_ != 0 && this->last_rssi_dbm_ < -75) ? 24 : 6;
  this->ota_producer_done_ = true;  // no producer yet
  this->ota_install_pct_ = this->ota_install_sub_ = 0;
  // Split the progress bar by time: upload (size / ~8 kB/s) vs the on-scooter install (~10 min).
  {
    uint32_t t_tx = size / 8000u;
    this->ota_transfer_share_ = (float) t_tx / (float) (t_tx + 600u);
  }
  if (this->ota_buf_ == nullptr || this->ota_cap_ == 0) {
    ESP_LOGW(OTAG, "no ring buffer (OTA disabled)");
    return;
  }

  // Ask for a fast connection interval (15–30 ms) so throughput isn't throttled to one
  // packet per slow interval — that overflow is what drops write-without-response chunks.
  esp_ble_conn_update_params_t cp = {};
  memcpy(cp.bda, this->get_remote_bda(), sizeof(esp_bd_addr_t));
  cp.min_int = 12;   // 15 ms
  cp.max_int = 24;   // 30 ms
  cp.latency = 0;
  cp.timeout = 800;  // 8 s supervision — tolerate brief dropouts at weak signal before the link dies
  esp_ble_gap_update_conn_params(&cp);
  memset(this->ota_sha_, 0, sizeof(this->ota_sha_));
  if (sha256_hex.size() == 64) {
    for (int i = 0; i < 32; i++)
      this->ota_sha_[i] = (uint8_t) strtoul(sha256_hex.substr(i * 2, 2).c_str(), nullptr, 16);
  }
  // chunk = min(240, ATT_MTU - 3 (ATT hdr) - 4 (offset hdr)); confirmed/limited by START_ACK.
  uint16_t mtu = this->att_mtu_ > this->mtu_ ? this->att_mtu_ : this->mtu_;
  this->ota_chunk_ = OTA_CHUNK_MAX;
  if (mtu >= 27 && (uint16_t)(mtu - 7) < this->ota_chunk_)
    this->ota_chunk_ = mtu - 7;
  // Apply what this link was last seen to carry. A brand new transfer starts one step above it, so
  // a link that has since improved (or a different scooter) climbs back to the full chunk instead
  // of being stuck at a size some bad afternoon taught it.
  uint16_t limit = this->ota_chunk_limit_;
  if (this->ota_selfheal_count_ == 0 && this->ota_resume_count_ == 0 && limit < OTA_CHUNK_MAX)
    limit *= 2;
  if (limit < this->ota_chunk_)
    this->ota_chunk_ = limit;

  bool eff_stage = this->stage_only_;
  ESP_LOGI(OTAG, "START %s %s size=%u chunk=%u gap=%ums bundle='%s'",
           comp_name(component), eff_stage ? "(stage-only)" : "(install)", (unsigned) size,
           this->ota_chunk_, this->ota_send_gap_ms_, bundle_id.c_str());
  this->ota_set_state_(OtaState::STARTING);
  this->ota_send_start_();
}

void LibrescootBleClient::ota_user_abort() {
  bool active = this->ota_state_ != OtaState::IDLE || this->ota_resolve_running_ ||
                !this->ota_jobs_.empty() || this->ota_awaiting_version_;
  if (!active)
    return;
  ESP_LOGW(OTAG, "user abort");
  bool was_awaiting = this->ota_awaiting_version_;
  // ota_active_ (which pins the BLE link while the scooter installs) is derived purely from
  // incoming OTA_STATUS notifications. If the ABORT_ACK never arrives — a dropped link, a scooter
  // that stays quiet — it stays latched and the link is held even in a mode that should release it.
  // The user asked to stop, so drop the pin here instead of waiting to be told.
  if (this->ota_active_) {
    this->ota_active_ = false;
    this->apply_link_state_();
  }
  this->ota_jobs_.clear();               // don't start the next queued component (e.g. DBC)
  this->ota_cancel_ = true;              // discard a resolve result that may still be in flight
  this->ota_awaiting_version_ = false;   // stop waiting for the post-install reboot/version
  if (this->ota_state_ != OtaState::IDLE) {
    const uint8_t m[2] = {0x04, 0x00};   // ABORT, reason=user cancel
    this->write_now_(CharId::OTA_CONTROL, m, sizeof(m));
    this->ota_finish_(false);            // settles the update entity
  } else {
    // Nothing is transferring (resolve pending, or just waiting for the reboot) — reset locally
    // without kicking off an online check; the scheduled check will re-evaluate later.
    this->ota_settle_update_entity_();
    if (this->ota_status_ != nullptr)
      this->ota_status_->publish_state(was_awaiting ? "Install wait cancelled" : "Aborted");
  }
}

void LibrescootBleClient::ota_send_start_() {
  std::vector<uint8_t> m;
  m.push_back(0x01);               // START
  m.push_back(0x01);               // version
  m.push_back(this->ota_component_);
  m.push_back(this->ota_chunk_ & 0xFF);
  m.push_back(this->ota_chunk_ >> 8);
  m.push_back(this->ota_total_ & 0xFF);
  m.push_back((this->ota_total_ >> 8) & 0xFF);
  m.push_back((this->ota_total_ >> 16) & 0xFF);
  m.push_back((this->ota_total_ >> 24) & 0xFF);
  m.insert(m.end(), this->ota_sha_, this->ota_sha_ + 32);
  m.push_back((uint8_t) this->ota_bundle_id_.size());
  m.insert(m.end(), this->ota_bundle_id_.begin(), this->ota_bundle_id_.end());
  this->write_now_(CharId::OTA_CONTROL, m.data(), m.size());
}

// ---------------------------------------------------------------------------
// OTA_STATUS dispatch (called from parse_ota_status_)
// ---------------------------------------------------------------------------
void LibrescootBleClient::ota_handle_status_(uint8_t *x, uint16_t len) {
  if (this->ota_state_ == OtaState::IDLE || len == 0)
    return;
  uint8_t op = x[0];
  switch (op) {
    case 0x81: {  // START_ACK [status][resume:u32][window:u16][ack_every:u8][max_chunk:u16]
      if (len < 11)
        return;
      uint8_t status = x[1];
      uint32_t resume = rd_u32le(&x[2]);
      uint16_t window = u16le(&x[6]);
      uint8_t ack_every = x[8];
      uint16_t max_chunk = u16le(&x[9]);
      if (status != 0x00 && status != 0x01) {
        this->ota_fail_(status == 0x11   ? "scooter busy"
                        : status == 0x13 ? "install already in progress"
                        : status == 0x10 ? "insufficient staging space"
                                         : "bad parameters");
        return;
      }
      if (status == 0x01)
        resume = 0;
      if (max_chunk && max_chunk < this->ota_chunk_)
        this->ota_chunk_ = max_chunk;
      // Cap the in-flight window to what the pre-allocated ring holds (and >= ack_every so
      // the scooter can still ACK). The scooter permits more; sending less is always safe.
      uint16_t ring_chunks = this->ota_cap_ / this->ota_chunk_;
      this->ota_window_chunks_ = window ? window : 64;
      if (this->ota_window_chunks_ > ring_chunks)
        this->ota_window_chunks_ = ring_chunks;
      this->ota_ack_every_ = ack_every ? ack_every : 16;
      // Slow start. The scooter permits a 64-chunk window, but filling it in one burst is what a
      // weak link cannot survive: write-without-response has no flow control, so the whole window
      // goes to the controller at once, the connection goes silent and dies on its supervision
      // timeout before a single ACK comes back. Open with a small window and widen it only as
      // ACKs actually arrive.
      if (this->ota_window_open_ > this->ota_window_chunks_ || this->ota_window_open_ < 4)
        this->ota_window_open_ = 8;
      this->ota_resume_ = resume;
      this->ota_selfheal_anchor_ = resume;  // progress detector baseline for the self-heal streak
      this->ota_acked_ = this->ota_sent_ = this->ota_produced_ = resume;
      this->ota_producer_run_ = true;
      this->ota_producer_done_ = false;
      this->ota_http_ok_ = false;
      if (xTaskCreate(&LibrescootBleClient::ota_producer_task_, "librescoot_dl", 16384, this, 6, nullptr) != pdPASS) {
        this->ota_fail_("could not start download task");
        return;
      }
      this->ota_start_ms_ = millis();
      this->ota_last_ack_ms_ = millis();
      this->ota_set_state_(OtaState::STREAMING);
      ESP_LOGI(OTAG, "START_ACK: %s resume=%u window=%u (open %u) ack_every=%u chunk=%u ring=%u",
               status == 0x00 ? "resume" : "fresh", (unsigned) resume, this->ota_window_chunks_,
               this->ota_window_open_, this->ota_ack_every_, this->ota_chunk_, (unsigned) this->ota_cap_);
      break;
    }
    case 0x82: {  // ACK [flags][acked:u32]
      if (len < 6)
        return;
      bool rewind = x[1] & 0x01;
      uint32_t acked = rd_u32le(&x[2]);
      this->ota_last_ack_ms_ = millis();
      if (rewind) {
        if (this->ota_window_open_ > 4)
          this->ota_window_open_ /= 2;  // loss: back off the in-flight window as well as the pacing
        if (++this->ota_rewinds_ > 60) {
          this->ota_fail_("too many rewinds");
          return;
        }
        this->ota_sent_ = acked;  // go-back-N
        // Loss -> we're sending too fast for the link to drain; back off the pacing.
        if (this->ota_send_gap_ms_ < 60)
          this->ota_send_gap_ms_ += 3;
        ESP_LOGD(OTAG, "REWIND to %u (#%d, gap now %u ms)", (unsigned) acked, this->ota_rewinds_,
                 this->ota_send_gap_ms_);
      }
      if (acked > this->ota_acked_) {
        this->ota_acked_ = acked;
        if (!rewind && this->ota_window_open_ < this->ota_window_chunks_)
          this->ota_window_open_ += 4;  // the link carried that much; allow a little more in flight
        // Real forward progress clears BOTH the rewind self-heal streak and the disconnect-resume
        // streak, so a long transfer over a flaky link (or one that keeps dropping out of range)
        // keeps its full budget as long as it keeps advancing — it only gives up after that many
        // resumes with NO progress (a truly dead link) or a user abort.
        if (this->ota_acked_ - this->ota_selfheal_anchor_ > OTA_SELFHEAL_PROGRESS_RESET) {
          this->ota_selfheal_count_ = 0;
          this->ota_resume_count_ = 0;
          this->ota_selfheal_anchor_ = this->ota_acked_;
        }
      }
      this->ota_progress_();
      if (this->ota_acked_ >= this->ota_total_ && this->ota_state_ == OtaState::STREAMING) {
        if (this->stage_only_) {
          ESP_LOGI(OTAG, "stage-only: all %u bytes acked, stopping before COMPLETE", (unsigned) this->ota_total_);
          this->ota_finish_(true);
        } else {
          const uint8_t c = 0x03;  // COMPLETE -> scooter verifies SHA-256 and queues the install
          this->write_now_(CharId::OTA_CONTROL, &c, 1);
          this->ota_set_state_(OtaState::COMPLETING);
          // The upload is over; only the scooter-side install is still running. Park the speeds now
          // instead of leaving them frozen at the last streaming value for the whole install.
          this->ota_park_rates_();
          if (this->ota_eta_ != nullptr)
            this->ota_eta_->publish_state("00:00:00");
          ESP_LOGW(OTAG, "all bytes acked -> COMPLETE (installing)");
        }
      }
      break;
    }
    case 0x83: {  // COMPLETE_ACK [status]
      if (len < 2)
        return;
      if (x[1] == 0x00) {
        ESP_LOGI(OTAG, "COMPLETE_ACK: verified & queued");
        this->ota_set_state_(OtaState::INSTALLING);
      } else {
        this->ota_fail_(x[1] == 0x01   ? "SHA-256 mismatch"
                        : x[1] == 0x02 ? "size mismatch"
                                       : "queueing failed");
      }
      break;
    }
    case 0x84: {  // INSTALL_PROGRESS [phase][percent] — text relayed by parse_ota_status_
      if (len < 3)
        break;
      uint8_t phase = x[1], pct = x[2];
      this->ota_state_ms_ = millis();  // progress arrived → reset the no-progress timeout
      if (phase == 0x00 || phase == 0x01) {  // verify / install — two 0-100% sub-phases
        if (this->ota_install_sub_ == 0 && this->ota_install_pct_ > 60 && pct + 30 < this->ota_install_pct_)
          this->ota_install_sub_ = 1;  // percent dropped a lot → second sub-phase started
        this->ota_install_pct_ = pct;
        this->ota_install_progress_();
      } else if (phase == 0x02 || phase == 0x04 || phase == 0x05) {
        ESP_LOGI(OTAG, "install terminal phase 0x%02X", phase);
        this->ota_finish_(phase != 0x05);
      }
      break;
    }
    case 0x85:  // ABORT_ACK
      this->ota_finish_(false);
      break;
    case 0x86:  // ERROR [code]
      this->ota_fail_(len >= 2 && x[1] == 0x01   ? "iMX suspended"
                      : len >= 2 && x[1] == 0x04 ? "staging write failed"
                      : len >= 2 && x[1] == 0x05 ? "no staging space"
                                                 : "scooter error");
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// BLE consumer (loop)
// ---------------------------------------------------------------------------
void LibrescootBleClient::ota_step_() {
  if (this->ota_state_ == OtaState::IDLE)
    return;
  const uint32_t now = millis();
  switch (this->ota_state_) {
    case OtaState::STARTING:
      if (now - this->ota_state_ms_ > 5000)
        this->ota_fail_("START_ACK timeout");
      break;
    case OtaState::STREAMING:
      this->ota_send_data_();
      this->ota_publish_rates_();
      // The ACK timeout measures how long the SCOOTER stays silent with data outstanding — never
      // how long the HTTP producer needs before its first bytes land. After a resume the ring
      // buffer starts empty and opening the stream costs a TLS handshake plus a redirect, which
      // can be seconds: with the clock running from START_ACK that alone burned the whole budget,
      // so every auto-resume failed again the moment the download finally started (a livelock that
      // advanced a couple of chunks per cycle).
      if (this->ota_sent_ <= this->ota_acked_)
        this->ota_last_ack_ms_ = now;  // nothing in flight -> nothing that could time out
      if (this->ota_sent_ > this->ota_acked_ && now - this->ota_last_ack_ms_ > 5000)
        this->ota_fail_("ACK stalled");
      else if (this->ota_producer_done_ && !this->ota_http_ok_ && this->ota_produced_ < this->ota_total_)
        this->ota_fail_("download failed");
      break;
    case OtaState::COMPLETING:
      if (now - this->ota_state_ms_ > 30000)
        this->ota_fail_("COMPLETE_ACK timeout");
      break;
    case OtaState::INSTALLING:
      // No-progress timeout: the on-scooter install can take ~10 min (verify + install), so only
      // fail if NO INSTALL_PROGRESS arrives for a while (ota_state_ms_ is reset on each 0x84).
      if (now - this->ota_state_ms_ > 180000)
        this->ota_fail_("install stalled (no progress)");
      break;
    case OtaState::DONE:
    case OtaState::FAILED:
      // Ring buffer is persistent (allocated at setup); just wait for the download task to
      // exit, then go idle so the next transfer can start.
      if (this->ota_producer_done_)
        this->ota_state_ = OtaState::IDLE;
      break;
    default:
      break;
  }
}

// Throughput/byte sensors measured straight from the transfer counters (ota_produced_ =
// HTTP -> ring buffer from GitHub/relay; ota_acked_ = in-order bytes confirmed by the scooter over
// BLE). Called only from the STREAMING case, so nothing publishes while idle. 10 s cadence.
void LibrescootBleClient::ota_publish_rates_() {
  const uint32_t now = millis();
  // 1 s cadence: log the live speeds to the ESP log only (NOT to HA — logs don't reach HA; only the
  // 10 s sensor publishes below do, kept sparse so HA isn't flooded). Own baselines so this doesn't
  // disturb the 10 s window math.
  if (this->ota_log_ms_ == 0 || now - this->ota_log_ms_ >= 1000) {
    if (this->ota_log_ms_ != 0) {
      const uint32_t ldt = now - this->ota_log_ms_;
      ESP_LOGD(OTAG, "speed: down %.2f  up %.2f kB/s  (%u/%u)",
               (float) (this->ota_produced_ - this->ota_log_last_prod_) / (float) ldt,
               (float) (this->ota_acked_ - this->ota_log_last_ack_) / (float) ldt,
               (unsigned) this->ota_acked_, (unsigned) this->ota_total_);
    }
    this->ota_log_ms_ = now;
    this->ota_log_last_prod_ = this->ota_produced_;
    this->ota_log_last_ack_ = this->ota_acked_;
  }
  // Re-baseline at streaming start and right after a resume (ota_rate_ms_ == 0), so the resume
  // offset is never counted as bytes "transferred now".
  if (this->ota_rate_ms_ == 0) {
    this->ota_rate_ms_ = now;
    this->ota_rate_last_prod_ = this->ota_produced_;
    this->ota_rate_last_ack_ = this->ota_acked_;
    return;
  }
  const uint32_t dt = now - this->ota_rate_ms_;
  if (dt < 10000)
    return;
  // ota_acked_ is cumulative and monotonic; ota_produced_ only grows. bytes/ms == kB/s.
  const uint32_t d_prod = this->ota_produced_ - this->ota_rate_last_prod_;
  const uint32_t d_ack = this->ota_acked_ - this->ota_rate_last_ack_;
  const float ul = (float) d_ack / (float) dt;  // bytes/ms == kB/s
  this->ota_ble_bytes_total_v_ += (double) d_ack;
  if (this->ota_dl_speed_ != nullptr)
    this->ota_dl_speed_->publish_state((float) d_prod / (float) dt);
  if (this->ota_ul_speed_ != nullptr)
    this->ota_ul_speed_->publish_state(ul);
  if (this->ota_ble_bytes_total_ != nullptr)
    this->ota_ble_bytes_total_->publish_state((float) this->ota_ble_bytes_total_v_);
  if (this->ota_target_transferred_ != nullptr)
    this->ota_target_transferred_->publish_state((float) this->ota_acked_);
  // OTA Upload ETA, based on the OTA Speed BLE Upload rate. Smooth it (EMA) so a single slow/fast
  // 10 s window doesn't swing the estimate; freeze the ETA while stalled (rate ~0) rather than
  // showing infinity. bytes/ms * 1000 = bytes/s.
  if (ul > 0.02f)
    this->ota_ul_ema_bpms_ = (this->ota_ul_ema_bpms_ <= 0.0f) ? ul : (0.6f * this->ota_ul_ema_bpms_ + 0.4f * ul);
  if (this->ota_eta_ != nullptr && this->ota_ul_ema_bpms_ > 0.0f && this->ota_total_ > this->ota_acked_) {
    const uint32_t secs = (uint32_t) ((float) (this->ota_total_ - this->ota_acked_) / (this->ota_ul_ema_bpms_ * 1000.0f));
    char eta[12];
    snprintf(eta, sizeof(eta), "%02u:%02u:%02u", (unsigned) (secs / 3600), (unsigned) ((secs % 3600) / 60),
             (unsigned) (secs % 60));
    this->ota_eta_->publish_state(eta);
  }
  this->ota_rate_ms_ = now;
  this->ota_rate_last_prod_ = this->ota_produced_;
  this->ota_rate_last_ack_ = this->ota_acked_;
}

// Nothing is streaming any more (upload finished and the scooter is installing, a self-heal
// backoff, or the end of the session): flush the final partial window into the lifetime/target
// counters and park both speed sensors at 0. Without this they keep displaying the last 10 s
// window value for as long as the install runs.
void LibrescootBleClient::ota_park_rates_() {
  if (this->ota_rate_ms_ != 0 && this->ota_acked_ > this->ota_rate_last_ack_)
    this->ota_ble_bytes_total_v_ += (double) (this->ota_acked_ - this->ota_rate_last_ack_);
  if (this->ota_ble_bytes_total_ != nullptr)
    this->ota_ble_bytes_total_->publish_state((float) this->ota_ble_bytes_total_v_);
  if (this->ota_target_transferred_ != nullptr)
    this->ota_target_transferred_->publish_state((float) this->ota_acked_);
  if (this->ota_dl_speed_ != nullptr)
    this->ota_dl_speed_->publish_state(0.0f);
  if (this->ota_ul_speed_ != nullptr)
    this->ota_ul_speed_->publish_state(0.0f);
  this->ota_rate_ms_ = 0;  // re-baseline on the next streaming tick (a resume must not be counted)
  this->ota_log_ms_ = 0;
}

void LibrescootBleClient::ota_send_data_() {
  if (this->ota_congested_ || this->ota_buf_ == nullptr)
    return;
  if (this->ota_sent_ < this->ota_acked_)
    this->ota_sent_ = this->ota_acked_;  // a late ACK can outrun a rewind; never resend acked data
  // Bluedroid gives no usable backpressure for write-without-response (no congestion event,
  // writes always return OK), so over-sending silently drops chunks -> gaps -> rewinds. Pace
  // to one chunk per ota_send_gap_ms_, which backs off on rewinds and stays low on fast links.
  const uint32_t now = millis();
  if (now - this->ota_last_send_ms_ < this->ota_send_gap_ms_)
    return;
  if (this->ota_sent_ >= this->ota_total_)
    return;
  const uint32_t window_bytes = (uint32_t) this->ota_window_open_ * this->ota_chunk_;
  if (this->ota_sent_ - this->ota_acked_ >= window_bytes)
    return;  // window full — wait for ACKs
  const uint32_t avail = this->ota_produced_ - this->ota_sent_;
  const uint32_t remain = this->ota_total_ - this->ota_sent_;
  uint16_t len;
  if (remain >= this->ota_chunk_) {
    if (avail < this->ota_chunk_)
      return;  // not enough produced for a full chunk
    len = this->ota_chunk_;
  } else {
    if (this->ota_produced_ < this->ota_total_)
      return;  // wait for the final bytes to arrive
    len = (uint16_t) remain;
  }
  uint8_t tmp[240];
  const uint32_t idx = this->ota_sent_ % this->ota_cap_;
  const uint32_t lin = this->ota_cap_ - idx;
  if (len <= lin) {
    memcpy(tmp, this->ota_buf_ + idx, len);
  } else {
    memcpy(tmp, this->ota_buf_ + idx, lin);
    memcpy(tmp + lin, this->ota_buf_, len - lin);
  }
  if (!this->ota_write_data_(this->ota_sent_, tmp, len)) {
    this->ota_congested_ = true;
    return;
  }
  this->ota_sent_ += len;
  this->ota_last_send_ms_ = now;
}

bool LibrescootBleClient::ota_write_data_(uint32_t offset, const uint8_t *data, uint16_t len) {
  auto *e = this->find_char_(CharId::OTA_DATA);
  if (e == nullptr || e->handle == 0 || this->state() != espbt::ClientState::ESTABLISHED)
    return false;
  uint8_t pkt[244];
  pkt[0] = offset & 0xFF;
  pkt[1] = (offset >> 8) & 0xFF;
  pkt[2] = (offset >> 16) & 0xFF;
  pkt[3] = (offset >> 24) & 0xFF;
  memcpy(pkt + 4, data, len);
  esp_err_t err = esp_ble_gattc_write_char(this->get_gattc_if(), this->get_conn_id(), e->handle, len + 4, pkt,
                                           ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    static uint32_t ec = 0;
    if ((ec++ % 25) == 0)
      ESP_LOGD("ota", "OTA_DATA write err=%s (#%u)", esp_err_to_name(err), (unsigned) ec);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// HTTP producer (own task)
// ---------------------------------------------------------------------------
void LibrescootBleClient::ota_producer_task_(void *arg) {
  static_cast<LibrescootBleClient *>(arg)->ota_producer_();
  vTaskDelete(nullptr);
}

// Open a request and follow redirects by hand: the streaming API (open + fetch_headers, as opposed
// to esp_http_client_perform) does not auto-follow, and both GitHub asset URLs and the HA relay 302
// to the signed CDN URL. Returns the final HTTP status, or -1 if the connection could not be opened.
static int http_open_following_(esp_http_client_handle_t c) {
  if (esp_http_client_open(c, 0) != ESP_OK)
    return -1;
  esp_http_client_fetch_headers(c);
  int status = esp_http_client_get_status_code(c);
  for (int redir = 0; (status == 301 || status == 302 || status == 307 || status == 308) && redir < 5;
       redir++) {
    esp_http_client_set_redirection(c);  // reads Location into the client URL
    esp_http_client_close(c);
    if (esp_http_client_open(c, 0) != ESP_OK)
      return -1;
    esp_http_client_fetch_headers(c);
    status = esp_http_client_get_status_code(c);
  }
  return status;
}

void LibrescootBleClient::ota_producer_() {
  esp_http_client_config_t cfg = {};
  cfg.url = this->ota_url_.c_str();
  if (this->ota_url_.rfind("https", 0) == 0)
    this->ota_http_tls_(&cfg);
  cfg.timeout_ms = 8000;
  cfg.buffer_size = 4096;  // RX: response headers/data (CDN headers are modest)
  // TX buffer holds the request LINE ("GET <path>?<query> HTTP/1.1"). After GitHub's 302 the CDN URL
  // is a long signed objects.githubusercontent.com URL (~1 KB of X-Amz-* query), so the default 512
  // overflows and esp_http_client fails the request with "Out of buffer" — that was the real cause of
  // the failed direct download (the metadata fetch has short api.github.com URLs, so it was fine).
  cfg.buffer_size_tx = 4096;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  esp_http_client_set_header(c, "User-Agent", "esphome-lsc-bluetooth-nrf");
  char range[48];
  snprintf(range, sizeof(range), "bytes=%u-", (unsigned) this->ota_resume_);
  esp_http_client_set_header(c, "Range", range);

  bool ok = false;
  {
    const uint32_t open_ms = millis();
    int status = http_open_following_(c);
    ESP_LOGD(OTAG, "download: HTTP %d after %u ms (from offset %u)", status,
             (unsigned) (millis() - open_ms), (unsigned) this->ota_resume_);
    if (status == 200 || status == 206) {
      int idle = 0;
      while (this->ota_producer_run_ && this->ota_produced_ < this->ota_total_) {
        uint32_t used = this->ota_produced_ - this->ota_acked_;
        if (used >= this->ota_cap_) {  // ring full — wait for acks to free space
          vTaskDelay(pdMS_TO_TICKS(2));
          continue;
        }
        uint32_t idx = this->ota_produced_ % this->ota_cap_;
        uint32_t want = this->ota_cap_ - used;       // free space
        if (want > this->ota_cap_ - idx)             // don't wrap in one read
          want = this->ota_cap_ - idx;
        if (want > this->ota_total_ - this->ota_produced_)
          want = this->ota_total_ - this->ota_produced_;
        if (want > 512)
          want = 512;
        int r = esp_http_client_read(c, (char *) (this->ota_buf_ + idx), want);
        if (r > 0) {
          this->ota_produced_ += r;  // published after the bytes are written
          idle = 0;
        } else {
          if (esp_http_client_is_complete_data_received(c) || this->ota_produced_ >= this->ota_total_)
            break;
          if (r < 0 || ++idle > 500)
            break;  // error or stuck
          vTaskDelay(pdMS_TO_TICKS(4));
        }
      }
      ok = this->ota_produced_ >= this->ota_total_;
      // Only a *stream* that ends by itself is worth a warning: when the consumer stops the
      // transfer it clears ota_producer_run_, and the short read is then the intended shutdown.
      if (!ok && this->ota_producer_run_)
        ESP_LOGW(OTAG, "download ended early: %u/%u B (complete=%d)", (unsigned) this->ota_produced_,
                 (unsigned) this->ota_total_, (int) esp_http_client_is_complete_data_received(c));
    } else if (status < 0) {
      ESP_LOGW(OTAG, "download connection failed");
    } else {
      ESP_LOGW(OTAG, "download HTTP %d", status);
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  this->ota_http_ok_ = ok;
  this->ota_producer_done_ = true;
}

// ---------------------------------------------------------------------------
// Delta pre-flight: which release does this .delta patch against?
// ---------------------------------------------------------------------------
// A .delta is a gzip'd tar whose first regular member is metadata.json, carrying
// "old_artifact_name" — the exact release the patch was generated against. The scooter does not
// check it: it assumes the running version IS that release, applies the patch, and only notices
// the mismatch at the very end as a checksum failure. Reading it here costs one ranged GET of the
// archive head and turns a doomed multi-minute transfer into an immediate, explicit refusal.

// 4 kB of the archive inflates to ~15 kB, and the tar carries PAX headers before every member, so
// metadata.json's body sits around offset 3–4.5 kB — the 16 kB window keeps a wide margin for
// further members appearing ahead of it.
static const size_t DELTA_PROBE_BYTES = 4096;
static const size_t DELTA_INFLATE_CAP = 8192;
// The inflate state alone is ~11 kB and a board without PSRAM runs the BLE stack, WiFi and this
// request out of one small heap. The probe is a safeguard, not a requirement: below this much free
// heap it is skipped rather than risk starving the stack that is about to stream the update.
static const size_t DELTA_PROBE_HEAP_MIN = 44 * 1024;

// Skip the gzip framing (RFC 1952): 10 fixed bytes, then the optional FEXTRA/FNAME/FCOMMENT/FHCRC
// fields selected by the flag byte. What follows is the raw deflate stream tinfl consumes.
static const uint8_t *gz_deflate_start_(const uint8_t *in, size_t len) {
  if (len < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 0x08)
    return nullptr;
  uint8_t flg = in[3];
  size_t p = 10;
  if (flg & 0x04) {  // FEXTRA
    if (p + 2 > len)
      return nullptr;
    p += 2 + ((size_t) in[p] | ((size_t) in[p + 1] << 8));
  }
  if (flg & 0x08)  // FNAME
    while (p < len && in[p++] != 0) {
    }
  if (flg & 0x10)  // FCOMMENT
    while (p < len && in[p++] != 0) {
    }
  if (flg & 0x02)  // FHCRC
    p += 2;
  return p < len ? in + p : nullptr;
}

// Inflate the head of a deliberately truncated deflate stream. HAS_MORE_INPUT is what makes the
// truncation clean: tinfl then reports "needs more input" and keeps the bytes it already produced,
// instead of treating the missing tail as corrupt. Returns the number of bytes written.
// Prefer PSRAM for these transient blocks so the internal heap the BLE stack needs is untouched;
// on a board without it this is a plain malloc.
static void *probe_alloc_(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p != nullptr ? p : malloc(n);
}

static size_t inflate_head_(const uint8_t *deflate, size_t in_len, uint8_t *out, size_t out_cap) {
  auto *d = (tinfl_decompressor *) probe_alloc_(sizeof(tinfl_decompressor));
  if (d == nullptr) {
    ESP_LOGW(OTAG, "delta probe: out of memory for the %u B inflate state", (unsigned) sizeof(*d));
    return 0;
  }
  tinfl_init(d);
  size_t in_sz = in_len, out_sz = out_cap;
  tinfl_status st = tinfl_decompress(d, deflate, &in_sz, out, out, &out_sz,
                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_HAS_MORE_INPUT);
  heap_caps_free(d);
  bool usable = st == TINFL_STATUS_DONE || st == TINFL_STATUS_NEEDS_MORE_INPUT ||
                st == TINFL_STATUS_HAS_MORE_OUTPUT;
  return usable ? out_sz : 0;
}

// Walk the tar members in an inflated archive head and return the "old_artifact_name" from
// metadata.json, with the "release-" prefix stripped so it compares against a release tag. PAX
// headers (./@PaxHeader, one before each real member) simply do not match the name and are skipped
// like any other member.
static std::string delta_base_from_tar_(const uint8_t *tar, size_t len) {
  static const char WANT[] = "metadata.json";
  static const size_t WANT_LEN = sizeof(WANT) - 1;
  for (size_t p = 0; p + 512 <= len;) {
    const char *name = (const char *) tar + p;
    if (name[0] == '\0')
      break;  // end-of-archive padding
    size_t nlen = strnlen(name, 100);
    char oct[13];
    memcpy(oct, tar + p + 124, 12);
    oct[12] = '\0';
    size_t size = (size_t) strtoul(oct, nullptr, 8);
    p += 512;
    if (nlen >= WANT_LEN && memcmp(name + nlen - WANT_LEN, WANT, WANT_LEN) == 0) {
      if (p + size > len)
        return "";  // the member did not fit in the range we asked for
      const std::string js((const char *) tar + p, size);
      size_t k = js.find("\"old_artifact_name\"");
      if (k == std::string::npos)
        return "";
      k = js.find('"', js.find(':', k) + 1);
      if (k == std::string::npos)
        return "";
      size_t e = js.find('"', ++k);
      if (e == std::string::npos)
        return "";
      std::string v = js.substr(k, e - k);
      if (v.rfind("release-", 0) == 0)
        v = v.substr(8);
      return v;
    }
    p += (size + 511) & ~(size_t) 511;
  }
  return "";
}

// Ask the Home Assistant relay for the base instead of reading the archive here. It fetches the
// same bytes anyway, has the memory to decompress them, and answers a short tag — so a board with
// only internal RAM gets the check too. Empty means "could not tell", never "mismatch".
std::string LibrescootBleClient::ota_delta_base_relay_(const std::string &url) {
  // http://host:port/ota/<secret>/<tag>/<file> -> .../base/<secret>/<tag>/<file>, the same
  // rewrite the scan endpoint uses.
  std::string q = url;
  size_t p = q.find("/ota/");
  if (p == std::string::npos)
    return "";
  q.replace(p, 5, "/base/");

  esp_http_client_config_t cfg = {};
  cfg.url = q.c_str();
  cfg.timeout_ms = 20000;  // the relay may have to fetch from GitHub before it can answer
  cfg.buffer_size = 512;
  cfg.buffer_size_tx = 1024;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr)
    return "";
  esp_http_client_set_header(c, "User-Agent", "esphome-lsc-bluetooth-nrf");

  std::string base;
  int status = http_open_following_(c);
  if (status == 200) {
    char buf[80];
    int r = esp_http_client_read(c, buf, sizeof(buf) - 1);
    if (r > 0) {
      buf[r] = '\0';
      base.assign(buf);
      while (!base.empty() && (base.back() == '\n' || base.back() == '\r' || base.back() == ' '))
        base.pop_back();
    }
  } else if (status != 204) {
    // 204 is the relay saying "I could not determine it" — expected, not worth a warning.
    ESP_LOGW(OTAG, "delta base via relay: HTTP %d", status);
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  ESP_LOGD(OTAG, "delta base via relay: '%s'", base.c_str());
  return base;
}

std::string LibrescootBleClient::ota_delta_base_(const std::string &url) {
  // A plain-HTTP source is the HA relay, which can answer this directly; https means GitHub, where
  // the archive has to be read here.
  if (url.rfind("http://", 0) == 0)
    return this->ota_delta_base_relay_(url);
  return this->ota_delta_base_onchip_(url);
}

std::string LibrescootBleClient::ota_delta_base_onchip_(const std::string &url) {
  size_t heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (heap < DELTA_PROBE_HEAP_MIN) {
    ESP_LOGW(OTAG, "delta probe: skipped, only %u B heap free", (unsigned) heap);
    return "";
  }
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  if (url.rfind("https", 0) == 0)
    this->ota_http_tls_(&cfg);
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 4096;  // the CDN redirect target is a ~1 kB signed URL (see ota_producer_)
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  esp_http_client_set_header(c, "User-Agent", "esphome-lsc-bluetooth-nrf");
  esp_http_client_set_header(c, "Range", "bytes=0-4095");

  std::string base;
  {
    // Same request path as the transfer itself, redirects included — the asset URL 302s to the CDN.
    int status = http_open_following_(c);
    ESP_LOGD(OTAG, "delta probe: status=%d free heap %u B (largest block %u B)", status,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // 206 is what both GitHub and the relay answer; a 200 (range ignored) is fine too — we simply
    // stop reading after the head.
    if (status == 200 || status == 206) {
      auto *raw = (uint8_t *) probe_alloc_(DELTA_PROBE_BYTES);
      if (raw == nullptr) {
        ESP_LOGW(OTAG, "delta probe: out of memory for the %u B read buffer", (unsigned) DELTA_PROBE_BYTES);
      } else {
        size_t got = 0;
        for (int empty = 0; got < DELTA_PROBE_BYTES && empty < 5;) {
          int r = esp_http_client_read(c, (char *) raw + got, DELTA_PROBE_BYTES - got);
          if (r > 0) {
            got += (size_t) r;
            empty = 0;
          } else if (r == 0) {
            empty++;  // nothing available yet — a short head would read as "base unknown"
            vTaskDelay(pdMS_TO_TICKS(20));
          } else {
            break;
          }
        }
        const uint8_t *df = gz_deflate_start_(raw, got);
        size_t inflated = 0;
        if (df != nullptr) {
          // The generous window is a margin, not a requirement — metadata.json's body ends around
          // 4.5 kB, so an internal-RAM-only board falls back rather than skipping the check.
          size_t cap = DELTA_INFLATE_CAP;
          auto *inf = (uint8_t *) probe_alloc_(cap);
          if (inf == nullptr) {
            ESP_LOGW(OTAG, "delta probe: out of memory for the inflate window");
          } else {
            inflated = inflate_head_(df, got - (size_t)(df - raw), inf, cap);
            base = delta_base_from_tar_(inf, inflated);
            heap_caps_free(inf);
          }
        }
        ESP_LOGD(OTAG, "delta probe: http=%d got=%u gzhdr=%d inflated=%u base='%s'", status,
                 (unsigned) got, df != nullptr ? (int) (df - raw) : -1, (unsigned) inflated,
                 base.c_str());
        heap_caps_free(raw);
      }
    } else if (status < 0) {
      ESP_LOGW(OTAG, "delta probe: connection failed");
    } else {
      ESP_LOGW(OTAG, "delta probe: HTTP %d", status);
    }
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  return base;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// install path: resolve the delta assets, then transfer MDB then DBC
// ---------------------------------------------------------------------------
void LibrescootBleClient::ota_resolve_task_(void *arg) {
  static_cast<LibrescootBleClient *>(arg)->ota_resolve_();
  vTaskDelete(nullptr);
}

void LibrescootBleClient::ota_resolve_() {
  const std::string url =
      "https://api.github.com/repos/" + this->github_repo_ + "/releases/tags/" + this->rs_tag_;
  // "delta" → small patch asset; "full" → complete .mender image (hundreds of MB).
  const std::string ext = (this->ota_method_str_ == "full") ? ".mender" : ".delta";
  const std::string tmdb = "librescoot-unu-mdb-" + this->rs_tag_ + ext;
  const std::string tdbc = "librescoot-unu-dbc-" + this->rs_tag_ + ext;

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  this->ota_http_tls_(&cfg);
  cfg.timeout_ms = 15000;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 512;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  esp_http_client_set_header(c, "User-Agent", "esphome-lsc-bluetooth-nrf");
  esp_http_client_set_header(c, "Accept", "application/vnd.github+json");

  // Scan the release JSON for each delta asset's name/size/digest (they appear in that order
  // within an asset object). browser_download_url is not needed — the URL is built from the
  // configured source base so a local mirror can serve the bytes without TLS.
  static const char N_NAME[] = "\"name\":\"";
  static const char N_SIZE[] = "\"size\":";
  static const char N_DIG[] = "\"digest\":\"";
  size_t mN = 0, mS = 0, mD = 0;
  enum { NONE, CN, CS, CD } cap = NONE;
  std::string cur_name, cur_size, cur_dig;
  auto eval = [&]() {
    std::string sha = cur_dig;
    if (sha.rfind("sha256:", 0) == 0)
      sha = sha.substr(7);
    if (cur_name == tmdb) {
      this->rs_mdb_name_ = cur_name;
      this->rs_mdb_size_ = (uint32_t) strtoul(cur_size.c_str(), nullptr, 10);
      this->rs_mdb_sha_ = sha;
      this->rs_mdb_ok_ = true;
    } else if (cur_name == tdbc) {
      this->rs_dbc_name_ = cur_name;
      this->rs_dbc_size_ = (uint32_t) strtoul(cur_size.c_str(), nullptr, 10);
      this->rs_dbc_sha_ = sha;
      this->rs_dbc_ok_ = true;
    }
  };

  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status == 200) {
      char buf[512];
      int r;
      while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r; i++) {
          char ch = buf[i];
          if (cap == CN) {
            if (ch == '"')
              cap = NONE;
            else
              cur_name += ch;
            continue;
          }
          if (cap == CS) {
            if (ch >= '0' && ch <= '9')
              cur_size += ch;
            else
              cap = NONE;
            continue;
          }
          if (cap == CD) {
            if (ch == '"') {
              cap = NONE;
              eval();
              cur_size.clear();
              cur_dig.clear();
            } else {
              cur_dig += ch;
            }
            continue;
          }
          mN = (ch == N_NAME[mN]) ? mN + 1 : (ch == N_NAME[0] ? 1 : 0);
          if (N_NAME[mN] == 0) { cap = CN; cur_name.clear(); mN = mS = mD = 0; continue; }
          mS = (ch == N_SIZE[mS]) ? mS + 1 : (ch == N_SIZE[0] ? 1 : 0);
          if (N_SIZE[mS] == 0) { cap = CS; cur_size.clear(); mN = mS = mD = 0; continue; }
          mD = (ch == N_DIG[mD]) ? mD + 1 : (ch == N_DIG[0] ? 1 : 0);
          if (N_DIG[mD] == 0) { cap = CD; cur_dig.clear(); mN = mS = mD = 0; continue; }
        }
      }
    } else {
      ESP_LOGW(OTAG, "resolve: HTTP %d", status);
    }
  } else {
    ESP_LOGW(OTAG, "resolve: connection failed");
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);

  ESP_LOGI(OTAG, "resolve: MDB %s DBC %s", this->rs_mdb_ok_ ? this->rs_mdb_name_.c_str() : "(none)",
           this->rs_dbc_ok_ ? this->rs_dbc_name_.c_str() : "(none)");

  // Delta pre-flight, for the one component this install is for (see ota_delta_base_). Full images
  // carry no base requirement, so they are not probed.
  this->rs_delta_base_.clear();
  if (this->ota_method_str_ != "full") {
    bool mdb = this->ota_install_component_ == 0;
    if (mdb ? this->rs_mdb_ok_ : this->rs_dbc_ok_) {
      const std::string &nm = mdb ? this->rs_mdb_name_ : this->rs_dbc_name_;
      this->rs_delta_base_ = this->ota_delta_base_(this->rs_source_ + "/" + this->rs_tag_ + "/" + nm);
      ESP_LOGI(OTAG, "resolve: %s delta patches %s", comp_name(this->ota_install_component_),
               this->rs_delta_base_.empty() ? "(base unknown)" : this->rs_delta_base_.c_str());
    }
  }
  this->ota_resolve_done_ = true;
}

void LibrescootBleClient::ota_kick_next_job_() {
  if (this->ota_state_ != OtaState::IDLE || this->ota_jobs_.empty())
    return;
  if (this->state() != espbt::ClientState::ESTABLISHED)
    return;  // wait for the (re)connect — don't consume the job while disconnected
  if (this->ota_selfheal_at_ms_ != 0 && millis() < this->ota_selfheal_at_ms_)
    return;  // self-heal backoff not elapsed yet (lets the old producer drain, throttles retries)
  this->ota_selfheal_at_ms_ = 0;
  OtaJob job = this->ota_jobs_.front();
  this->ota_jobs_.erase(this->ota_jobs_.begin());
  this->ota_install_component_ = job.component;
  this->ota_active_update_ = (job.component == 1) ? this->dbc_update_ : this->mdb_update_;
  this->ota_current_job_ = job;         // keep it so a mid-transfer disconnect can resume it
  this->ota_have_current_job_ = true;
  ESP_LOGI(OTAG, "install: starting %s (%s, %u B)", comp_name(job.component), job.bundle_id.c_str(),
           (unsigned) job.size);
  this->ota_start(job.url, job.sha, job.size, job.bundle_id, job.component);
}

// Map the on-scooter install (two 0-100% sub-phases) onto the second slice of the progress bar.
void LibrescootBleClient::ota_install_progress_() {
  float inst = (float) (this->ota_install_sub_ * 50) + (float) this->ota_install_pct_ / 2.0f;  // 0-100
  if (inst > 100.0f)
    inst = 100.0f;
  float bar = this->ota_transfer_share_ * 100.0f + (1.0f - this->ota_transfer_share_) * inst;
  if (this->ota_active_update_ != nullptr)
    this->ota_active_update_->set_progress(bar);
}

// A BLE drop mid-OTA: during the transfer, re-queue the job so it resumes on reconnect; during
// the install, the scooter finishes on its own, so just wait for the reboot + new version.
void LibrescootBleClient::ota_handle_disconnect_() {
  if (this->ota_state_ == OtaState::STARTING || this->ota_state_ == OtaState::STREAMING) {
    this->ota_producer_run_ = false;
    this->ota_park_rates_();
    this->ota_note_no_progress_();
    // A supervision timeout with chunks still unacknowledged is the same evidence a REWIND gives:
    // more was pushed into the link than it can carry. Widen the pacing here too — on a weak link
    // the burst kills the connection before any ACK (or REWIND) can come back, so without this the
    // only feedback path that slows us down never runs.
    if (this->ota_sent_ > this->ota_acked_) {
      if (this->ota_send_gap_ms_ < 60)
        this->ota_send_gap_ms_ += 3;
      if (this->ota_window_open_ > 4)
        this->ota_window_open_ /= 2;
    }
    if (this->ota_have_current_job_ && ++this->ota_resume_count_ <= 40) {
      ESP_LOGW(OTAG, "link dropped mid-transfer — will resume on reconnect (#%d)", this->ota_resume_count_);
      this->ota_jobs_.insert(this->ota_jobs_.begin(), this->ota_current_job_);  // resume this one first
      this->ota_set_state_(OtaState::IDLE);  // ota_kick_next_job_ re-runs it once reconnected
      if (this->ota_status_ != nullptr)
        this->ota_status_->publish_state("Uploading — reconnecting…");
    } else {
      this->ota_fail_("too many reconnects");
    }
  } else if (this->ota_state_ == OtaState::COMPLETING || this->ota_state_ == OtaState::INSTALLING) {
    ESP_LOGW(OTAG, "link dropped during install — the scooter finishes on its own");
    this->ota_begin_await_version_();
    this->ota_set_state_(OtaState::IDLE);
  }
}

void LibrescootBleClient::ota_set_state_(OtaState s) {
  this->ota_state_ = s;
  this->ota_state_ms_ = millis();
}

// Report cadence scales with bundle size so a big transfer doesn't flood the log / HA:
//   <= 500 kB -> every 5 s,  <= 30 MB -> every 60 s,  > 30 MB -> every 5 min.
bool LibrescootBleClient::ota_report_due_() {
  uint32_t interval = 5000;
  if (this->ota_total_ > 30u * 1024 * 1024)
    interval = 300000;
  else if (this->ota_total_ > 500u * 1024)
    interval = 60000;
  uint32_t now = millis();
  if (this->ota_last_report_ms_ != 0 && now - this->ota_last_report_ms_ < interval)
    return false;
  this->ota_last_report_ms_ = now;
  return true;
}

void LibrescootBleClient::ota_progress_() {
  if (this->ota_total_ == 0)
    return;
  bool final = this->ota_acked_ >= this->ota_total_;
  if (!final && !this->ota_report_now_)  // throttled by ota_report_due_() (per notification)
    return;

  float pct = 100.0f * (float) this->ota_acked_ / (float) this->ota_total_;
  if (this->ota_active_update_ != nullptr)
    this->ota_active_update_->set_progress(this->ota_transfer_share_ * pct);  // upload = first slice

  // Live ETA is owned by ota_publish_rates_ (driven off the OTA Speed BLE Upload rate); here we only
  // stamp the terminal 00:00:00 when the upload is complete.
  if (final && this->ota_eta_ != nullptr)
    this->ota_eta_->publish_state("00:00:00");

  if (this->ota_status_ != nullptr) {
    char b[56];
    snprintf(b, sizeof(b), "Uploading %s %.0f%% (%u/%u)", comp_name(this->ota_component_), pct,
             (unsigned) this->ota_acked_, (unsigned) this->ota_total_);
    this->ota_status_->publish_state(b);
  }
  ESP_LOGI(OTAG, "%s %.0f%% (%u/%u)", comp_name(this->ota_component_), pct,
           (unsigned) this->ota_acked_, (unsigned) this->ota_total_);
}

// A whole session with the link up and not one new byte acknowledged is the signature of writes the
// link cannot deliver at all — at full MTU a DATA write fragments into ~10 link-layer packets, and
// on a weak link the peripheral wedges on the incomplete reassembly rather than dropping it. Halve
// the chunk (never below OTA_CHUNK_MIN) and remember it; halving keeps every staged resume offset an
// exact multiple of the new size, so the transfer picks up where it left off.
void LibrescootBleClient::ota_note_no_progress_() {
  // Only when chunks were actually written and none came back acknowledged. A session that died
  // before the download delivered its first bytes says nothing about the chunk size.
  if (this->ota_sent_ <= this->ota_resume_ || this->ota_acked_ > this->ota_resume_ ||
      this->ota_chunk_limit_ <= OTA_CHUNK_MIN)
    return;
  this->ota_chunk_limit_ /= 2;
  if (this->ota_chunk_limit_ < OTA_CHUNK_MIN)
    this->ota_chunk_limit_ = OTA_CHUNK_MIN;
  ESP_LOGW(OTAG, "no bytes got through at chunk %u — retrying with %u B chunks", this->ota_chunk_,
           this->ota_chunk_limit_);
  this->ota_chunk_pref_.save(&this->ota_chunk_limit_);
}

void LibrescootBleClient::ota_fail_(const char *why) {
  ESP_LOGE(OTAG, "transfer failed: %s (sent=%u acked=%u/%u)", why, (unsigned) this->ota_sent_,
           (unsigned) this->ota_acked_, (unsigned) this->ota_total_);
  // Self-heal: a recoverable transfer-phase failure (rewinds/stall/timeout) re-queues the current
  // job and auto-resumes from the staged offset after a backoff, instead of aborting the whole
  // update. Bounded (OTA_SELFHEAL_MAX consecutive, reset on real progress). Never on a user abort
  // (ota_cancel_) and never past the transfer (COMPLETING/INSTALLING install failures hard-fail).
  const bool transfer_phase = this->ota_state_ == OtaState::STARTING || this->ota_state_ == OtaState::STREAMING;
  if (this->ota_auto_resume_ && !this->ota_cancel_ && this->ota_have_current_job_ && transfer_phase &&
      ++this->ota_selfheal_count_ <= OTA_SELFHEAL_MAX) {
    this->ota_producer_run_ = false;  // stop the download task; ota_start re-inits it on resume
    this->ota_park_rates_();
    this->ota_note_no_progress_();
    // A stall with chunks outstanding means the link could not drain what we pushed into it (the
    // scooter never got far enough to answer with a REWIND). Back the pacing off like a rewind
    // would, so repeated resumes converge on a rate the link actually sustains.
    if (this->ota_send_gap_ms_ < 60)
      this->ota_send_gap_ms_ += 3;
    this->ota_selfheal_total_ += 1.0;
    if (this->ota_selfheal_resumes_ != nullptr)
      this->ota_selfheal_resumes_->publish_state((float) this->ota_selfheal_total_);
    this->ota_jobs_.insert(this->ota_jobs_.begin(), this->ota_current_job_);  // resume this one first
    this->ota_selfheal_at_ms_ = millis() + OTA_SELFHEAL_BACKOFF_MS;
    if (this->ota_selfheal_at_ms_ == 0)
      this->ota_selfheal_at_ms_ = 1;  // 0 is the "no backoff" sentinel; never store it
    this->ota_set_state_(OtaState::IDLE);  // ota_kick_next_job_ re-runs it after the backoff
    ESP_LOGW(OTAG, "self-heal: '%s' — auto-resume #%u (streak %u/%u) in %u s", why,
             (unsigned) this->ota_selfheal_total_, (unsigned) this->ota_selfheal_count_, OTA_SELFHEAL_MAX,
             OTA_SELFHEAL_BACKOFF_MS / 1000);
    if (this->ota_status_ != nullptr)
      this->ota_status_->publish_state(std::string("Auto-resume #") + std::to_string((unsigned) this->ota_selfheal_count_) +
                                       " (" + why + ")");
    return;
  }
  if (this->ota_status_ != nullptr)
    this->ota_status_->publish_state(std::string("Error: ") + why);
  this->ota_finish_(false);
}

// Fold a finished session's throughput into the estimate used for the NEXT transfer. Both an
// abort and a completed transfer count — the rate they measured is equally real. Short bursts are
// ignored: the first seconds are dominated by the START handshake and the initial window fill.
void LibrescootBleClient::ota_learn_rate_(uint32_t moved, uint32_t dt_ms) {
  static constexpr uint32_t LEARN_MIN_BYTES = 64u * 1024;
  static constexpr uint32_t LEARN_MIN_MS = 30000;
  static constexpr float LEARN_WEIGHT = 0.3f;  // damped: one bad-range session shouldn't own it
  if (moved < LEARN_MIN_BYTES || dt_ms < LEARN_MIN_MS)
    return;
  float session = (float) moved * 1000.0f / (float) dt_ms;
  if (!(session > 200.0f) || session > 200000.0f)
    return;
  float updated = this->ota_rate_bps_ * (1.0f - LEARN_WEIGHT) + session * LEARN_WEIGHT;
  ESP_LOGI(OTAG, "upload rate estimate: %.1f -> %.1f kB/s (this session %.1f)",
           this->ota_rate_bps_ / 1000.0f, updated / 1000.0f, session / 1000.0f);
  this->ota_rate_bps_ = updated;
  this->ota_rate_pref_.save(&updated);
}

void LibrescootBleClient::ota_finish_(bool ok) {
  this->ota_producer_run_ = false;  // ask the download task to stop; buffer freed in ota_step_
  // Flush the final partial window into the lifetime byte counter and the target-transferred
  // sensor, then park the two speed sensors at 0 (a transfer is no longer running).
  this->ota_park_rates_();
  // Report the bytes actually moved THIS session (acked minus the resume offset) — otherwise a
  // near-complete resume shows a nonsense kB/s (the resumed bytes weren't transferred now).
  if (this->ota_start_ms_ != 0 && this->ota_acked_ > this->ota_resume_) {
    uint32_t dt = millis() - this->ota_start_ms_;
    uint32_t moved = this->ota_acked_ - this->ota_resume_;
    if (dt > 0) {
      ESP_LOGI(OTAG, "%s %s: %u bytes this session in %u ms (%.1f kB/s)", comp_name(this->ota_component_),
               ok ? "done" : "stopped", (unsigned) moved, (unsigned) dt, (float) moved / (float) dt);
      this->ota_learn_rate_(moved, dt);
    }
  }
  if (this->ota_eta_ != nullptr)
    this->ota_eta_->publish_state(ok ? "00:00:00" : "--:--:--");

  if (!ok)
    this->ota_jobs_.clear();  // a failure or abort stops the whole MDB+DBC sequence

  // Safety: if an auto-update-triggered install ends in a terminal failure (bad delta base, abort,
  // scooter error), switch OTA Auto Update OFF so it never retries the same broken update overnight.
  if (!ok && this->ota_auto_update_ && this->ota_auto_check_pending_) {
    ESP_LOGW(OTAG, "auto-update: install failed — switching OTA Auto Update off (no unattended retry)");
    this->ota_auto_update_ = false;
    this->ota_auto_check_pending_ = false;
    if (this->auto_update_sw_ != nullptr)
      this->auto_update_sw_->publish_state(false);
  }

  // Same latch as on a user abort: ota_active_ pins the BLE link and is only ever cleared by an
  // incoming OTA_STATUS. When a failure ends the whole sequence — nothing queued, no self-heal
  // retry pending — there is nothing left to hold the link for, so release it rather than wait for
  // a notification that may never come. A pending retry keeps the pin: the resume needs the link.
  if (!ok && this->ota_active_ && this->ota_jobs_.empty() && this->ota_selfheal_at_ms_ == 0) {
    this->ota_active_ = false;
    this->apply_link_state_();
  }

  if (ok && !this->ota_jobs_.empty()) {
    // More components queued (DBC after MDB) — keep the progress bar; the next one starts.
  } else if (ok && !this->stage_only_) {
    // Last component's install is queued — keep "installing" until the scooter reboots into the
    // new version (or the user aborts the wait).
    this->ota_begin_await_version_();
  } else {
    // Failed, aborted, or stage-only complete — settle the entity from the versions we know
    // (no online check; the scheduled check runs on its own timer).
    this->ota_settle_update_entity_();
  }
  this->ota_set_state_(ok ? OtaState::DONE : OtaState::FAILED);
}

void LibrescootBleClient::ota_begin_await_version_() {
  this->ota_awaiting_version_ = true;
  this->ota_await_tag_ = this->rs_tag_;
  this->ota_await_until_ms_ = millis() + 30UL * 60 * 1000;  // safety cap; user can abort sooner
  this->ota_await_poll_ms_ = millis() + 15000;  // then re-ask the version every 15 s until it moves
  if (this->ota_active_update_ != nullptr)
    this->ota_active_update_->set_progress(100.0f);  // stay in "installing" until the reboot confirms
  if (this->ota_status_ != nullptr)
    this->ota_status_->publish_state("Installing — waiting for scooter reboot…");
  ESP_LOGI(OTAG, "install queued; awaiting reboot into %s", this->ota_await_tag_.c_str());
}

}  // namespace librescoot_ble_client
}  // namespace esphome

#endif  // USE_ESP32
