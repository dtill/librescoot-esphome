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
  this->ota_start_ms_ = this->ota_last_ack_ms_ = this->ota_last_send_ms_ = 0;
  this->ota_last_report_ms_ = 0;
  this->ota_send_gap_ms_ = 6;  // start fast; back off adaptively on rewinds
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
  this->ota_chunk_ = 240;
  if (mtu >= 27 && (uint16_t)(mtu - 7) < this->ota_chunk_)
    this->ota_chunk_ = mtu - 7;

  bool eff_stage = this->stage_only_;
  ESP_LOGI(OTAG, "START %s %s size=%u chunk=%u bundle='%s'",
           comp_name(component), eff_stage ? "(stage-only)" : "(install)", (unsigned) size,
           this->ota_chunk_, bundle_id.c_str());
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
      this->ota_resume_ = resume;
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
      ESP_LOGI(OTAG, "START_ACK: %s resume=%u window=%u ack_every=%u chunk=%u ring=%u",
               status == 0x00 ? "resume" : "fresh", (unsigned) resume, this->ota_window_chunks_,
               this->ota_ack_every_, this->ota_chunk_, (unsigned) this->ota_cap_);
      break;
    }
    case 0x82: {  // ACK [flags][acked:u32]
      if (len < 6)
        return;
      bool rewind = x[1] & 0x01;
      uint32_t acked = rd_u32le(&x[2]);
      this->ota_last_ack_ms_ = millis();
      if (rewind) {
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
      if (acked > this->ota_acked_)
        this->ota_acked_ = acked;
      this->ota_progress_();
      if (this->ota_acked_ >= this->ota_total_ && this->ota_state_ == OtaState::STREAMING) {
        if (this->stage_only_) {
          ESP_LOGI(OTAG, "stage-only: all %u bytes acked, stopping before COMPLETE", (unsigned) this->ota_total_);
          this->ota_finish_(true);
        } else {
          const uint8_t c = 0x03;  // COMPLETE -> scooter verifies SHA-256 and queues the install
          this->write_now_(CharId::OTA_CONTROL, &c, 1);
          this->ota_set_state_(OtaState::COMPLETING);
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
  const uint32_t window_bytes = (uint32_t) this->ota_window_chunks_ * this->ota_chunk_;
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
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    // The streaming API does not auto-follow redirects; GitHub 302s to the signed CDN URL.
    for (int redir = 0; (status == 301 || status == 302 || status == 307 || status == 308) && redir < 5; redir++) {
      esp_http_client_set_redirection(c);  // reads Location into the client URL
      esp_http_client_close(c);
      if (esp_http_client_open(c, 0) != ESP_OK) {
        status = -1;
        break;
      }
      esp_http_client_fetch_headers(c);
      status = esp_http_client_get_status_code(c);
    }
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
    } else {
      ESP_LOGW(OTAG, "download HTTP %d", status);
    }
  } else {
    ESP_LOGW(OTAG, "download connection failed");
  }
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  this->ota_http_ok_ = ok;
  this->ota_producer_done_ = true;
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
  this->ota_resolve_done_ = true;
}

void LibrescootBleClient::ota_kick_next_job_() {
  if (this->ota_state_ != OtaState::IDLE || this->ota_jobs_.empty())
    return;
  if (this->state() != espbt::ClientState::ESTABLISHED)
    return;  // wait for the (re)connect — don't consume the job while disconnected
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

  // ETA from the rate actually achieved this session (bytes moved since the resume offset).
  // Wait for a couple of seconds and >2% of real data before estimating, or the first sample
  // (a single chunk) produces a nonsense hour-long guess.
  char eta[12] = "--:--:--";
  if (final) {
    strncpy(eta, "00:00:00", sizeof(eta));
  } else if (this->ota_start_ms_ != 0 && this->ota_acked_ > this->ota_resume_) {
    uint32_t dt = millis() - this->ota_start_ms_;
    uint32_t moved = this->ota_acked_ - this->ota_resume_;
    if (dt > 3000 && moved > this->ota_total_ / 50) {
      float bps = (float) moved * 1000.0f / (float) dt;  // bytes/sec
      uint32_t secs = (uint32_t) ((float) (this->ota_total_ - this->ota_acked_) / bps);
      snprintf(eta, sizeof(eta), "%02u:%02u:%02u", (unsigned) (secs / 3600),
               (unsigned) ((secs % 3600) / 60), (unsigned) (secs % 60));
    }
  }
  if (this->ota_eta_ != nullptr)
    this->ota_eta_->publish_state(eta);

  if (this->ota_status_ != nullptr) {
    char b[56];
    snprintf(b, sizeof(b), "Uploading %s %.0f%% (%u/%u)", comp_name(this->ota_component_), pct,
             (unsigned) this->ota_acked_, (unsigned) this->ota_total_);
    this->ota_status_->publish_state(b);
  }
  ESP_LOGI(OTAG, "%s %.0f%% (%u/%u) ETA %s", comp_name(this->ota_component_), pct,
           (unsigned) this->ota_acked_, (unsigned) this->ota_total_, eta);
}

void LibrescootBleClient::ota_fail_(const char *why) {
  ESP_LOGE(OTAG, "transfer failed: %s (sent=%u acked=%u/%u)", why, (unsigned) this->ota_sent_,
           (unsigned) this->ota_acked_, (unsigned) this->ota_total_);
  if (this->ota_status_ != nullptr)
    this->ota_status_->publish_state(std::string("Error: ") + why);
  this->ota_finish_(false);
}

void LibrescootBleClient::ota_finish_(bool ok) {
  this->ota_producer_run_ = false;  // ask the download task to stop; buffer freed in ota_step_
  // Report the bytes actually moved THIS session (acked minus the resume offset) — otherwise a
  // near-complete resume shows a nonsense kB/s (the resumed bytes weren't transferred now).
  if (this->ota_start_ms_ != 0 && this->ota_acked_ > this->ota_resume_) {
    uint32_t dt = millis() - this->ota_start_ms_;
    uint32_t moved = this->ota_acked_ - this->ota_resume_;
    if (dt > 0)
      ESP_LOGI(OTAG, "%s %s: %u bytes this session in %u ms (%.1f kB/s)", comp_name(this->ota_component_),
               ok ? "done" : "stopped", (unsigned) moved, (unsigned) dt, (float) moved / (float) dt);
  }
  if (this->ota_eta_ != nullptr)
    this->ota_eta_->publish_state(ok ? "00:00:00" : "--:--:--");

  if (!ok)
    this->ota_jobs_.clear();  // a failure or abort stops the whole MDB+DBC sequence

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
  if (this->ota_active_update_ != nullptr)
    this->ota_active_update_->set_progress(100.0f);  // stay in "installing" until the reboot confirms
  if (this->ota_status_ != nullptr)
    this->ota_status_->publish_state("Installing — waiting for scooter reboot…");
  ESP_LOGI(OTAG, "install queued; awaiting reboot into %s", this->ota_await_tag_.c_str());
}

}  // namespace librescoot_ble_client
}  // namespace esphome

#endif  // USE_ESP32
