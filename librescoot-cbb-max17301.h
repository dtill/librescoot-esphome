#pragma once
#include "esphome.h"

using namespace esphome;

class MAX17301 {
 public:
  float soc = 0;
  float vfsoc = 0;
  float current = 0;
  float voltage = 0;
  bool charging = false;

  // Diagnostic
  int age = 0;            // Percent (FullCapNom / DesignCap)
  float cycles = 0;       // Full charge/discharge cycles
  float temp = 0;         // Current temperature in deg C
  int8_t temp_max = 0;    // Max temp since last NV save
  int8_t temp_min = 0;    // Min temp since last NV save

  // Capacity & time estimates
  float rep_cap = 0;      // Remaining capacity in mAh
  float full_cap = 0;     // Full capacity reported in mAh
  float tte = 0;          // Time to empty in minutes (NAN if > ~65500 min)
  float ttf = 0;          // Time to full in minutes  (NAN if > ~65500 min)

  // Status / protection
  uint16_t status_raw = 0;       // 0x00 Status
  uint16_t comm_stat_raw = 0;    // 0x61 CommStat
  uint16_t prot_status_raw = 0;  // 0xD9 ProtStatus
  bool dis_fet_disabled = false; // CommStat bit 9 (DIS)
  bool chg_fet_disabled = false; // CommStat bit 8 (CHG)
  bool nv_error = false;         // CommStat bit 2 (NVError)
  std::string status_hex;
  std::string comm_stat_hex;
  std::string prot_status_hex;

  // Identification
  std::string serial_number;
  std::string manufacturer;
  std::string device_name;
  std::string unique_id;       // 0x1BC..0x1BF concatenated (16 hex chars)
  std::string chip_part;       // decoded from 0x21 DevName/PartNo

 private:
  bool info_logged_ = false;

  std::string read_string_words(std::initializer_list<uint16_t> regs) {
    std::string result;
    for (uint16_t reg : regs) {
      uint16_t w = read_reg(reg);
      // ASCII bytes, low byte first then high byte
      char lo = (char)(w & 0xFF);
      char hi = (char)((w >> 8) & 0xFF);
      if (lo >= 0x20 && lo < 0x7F) result += lo;
      if (hi >= 0x20 && hi < 0x7F) result += hi;
    }
    return result;
  }

  std::string to_hex4_(uint16_t v) {
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", v);
    return std::string(buf);
  }

 public:
  uint16_t read_reg(uint16_t reg) {
    uint8_t addr = (reg >> 8) >= 1 ? 0x0B : 0x36;
    uint8_t reg_addr = (uint8_t)(reg & 0xFF);
    uint8_t buffer[2] = {0, 0};

    if (id(lsc_i2c_bus_1).write(addr, &reg_addr, 1) != i2c::ERROR_OK) {
        return 0;
    }
    if (id(lsc_i2c_bus_1).read(addr, buffer, 2) != i2c::ERROR_OK) {
        return 0;
    }
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
  }

  void update() {
    // 1. Get Sense Resistor (NRSense)
    uint16_t nr_raw = read_reg(0x1CF);
    float nrSense = (float)nr_raw * 0.000010f;
    if (nrSense <= 0) nrSense = 0.01f;

    // 2. Read Values
    uint16_t soc_raw = read_reg(0x06);
    this->soc = (float)(soc_raw >> 8);

    uint16_t vfsoc_raw = read_reg(0xFF);
    this->vfsoc = (float)(vfsoc_raw >> 8);

    uint16_t v_raw = read_reg(0x1A);
    this->voltage = (float)v_raw * 0.000078125f;

    int16_t c_raw = (int16_t)read_reg(0x1C);
    double current_uA = (double)c_raw * 1.5625 / (double)nrSense;
    this->current = (float)(current_uA / 1000.0);

    // 3. Charging logic (current > 5000uA / 5mA)
    this->charging = current_uA > 5000;

    // 4. Temperature register (01Bh), two's complement, LSb = 1/256 deg C
    int16_t temp_raw = (int16_t)read_reg(0x1B);
    this->temp = (float)temp_raw / 256.0f;

    // 5. MaxMinTemp (009h): D15..D8 = Max, D7..D0 = Min, both 8-bit two's complement, 1 deg C
    uint16_t mmt_raw = read_reg(0x09);
    this->temp_max = (int8_t)((mmt_raw >> 8) & 0xFF);
    this->temp_min = (int8_t)(mmt_raw & 0xFF);

    // 6. Age + Cycles (logged once)
    // Age register (007h): percentage, LSb = 1/256 %
    uint16_t age_raw = read_reg(0x07);
    this->age = (int)(age_raw >> 8);

    // Cycles register (017h): 16-bit CycleCount, LSb = 25% of a full cycle
    uint16_t cycles_raw = read_reg(0x17);
    this->cycles = (float)cycles_raw * 0.25f;

    // 6b. Capacity registers — LSb = 5 µVh / nRSense → mAh
    // cap_lsb_mAh = 5e-6 Vh / nrSense(Ω) * 1000 mA/A = 0.005 / nrSense
    float cap_lsb_mAh = 0.005f / nrSense;
    uint16_t rep_cap_raw  = read_reg(0x05);
    uint16_t full_cap_raw = read_reg(0x10);
    this->rep_cap  = (float)rep_cap_raw  * cap_lsb_mAh;
    this->full_cap = (float)full_cap_raw * cap_lsb_mAh;

    // 6c. TTE / TTF — LSb = 5.625 s → convert to minutes
    // 0xFFFF means "not computed yet" / "infinite" → expose as 0
    uint16_t tte_raw = read_reg(0x11);
    uint16_t ttf_raw = read_reg(0x20);
    ESP_LOGD("max17301", "TTE raw: 0x%04X (%u), TTF raw: 0x%04X (%u)", tte_raw, tte_raw, ttf_raw, ttf_raw);
    this->tte = (tte_raw == 0xFFFF) ? 0.0f : (float)tte_raw * 5.625f / 60.0f;
    this->ttf = (ttf_raw == 0xFFFF) ? 0.0f : (float)ttf_raw * 5.625f / 60.0f;

    // 6d. Status / protection registers
    this->status_raw      = read_reg(0x00);
    this->comm_stat_raw   = read_reg(0x61);
    this->prot_status_raw = read_reg(0xD9);
    this->status_hex      = this->to_hex4_(this->status_raw);
    this->comm_stat_hex   = this->to_hex4_(this->comm_stat_raw);
    this->prot_status_hex = this->to_hex4_(this->prot_status_raw);
    // CommStat bit layout (MAX1730x): bit 8 = CHG FET disabled, bit 9 = DIS FET disabled, bit 2 = NVError
    this->chg_fet_disabled = (this->comm_stat_raw & (1 << 8)) != 0;
    this->dis_fet_disabled = (this->comm_stat_raw & (1 << 9)) != 0;
    this->nv_error         = (this->comm_stat_raw & (1 << 2)) != 0;

    // 7. Arduino format output (every read, unchanged)
    ESP_LOGD("max17301", "SOC: %.0f%% VFSOC: %.0f%%, Current: %.2fmA, Charging: %s, Voltage: %.2fV",
             this->soc,
             this->vfsoc,
             this->current,
             this->charging ? "Yes" : "No",
             this->voltage);

    // 8. One-shot info output
    if (!this->info_logged_) {
      // Identification (read once, static data)
      // SerialNumber (nv 0xE8–0xEF = read_reg 0x1E8–0x1EF): 8 x 16-bit = 16 ASCII bytes
      this->serial_number = read_string_words({0x1E8, 0x1E9, 0x1EA, 0x1EB, 0x1EC, 0x1ED, 0x1EE, 0x1EF});

      // ManfctrName: 0x120 + 0x146..0x14A (6 words = 12 ASCII bytes)
      this->manufacturer = read_string_words({0x120, 0x146, 0x147, 0x148, 0x149, 0x14A});

      // DeviceName: 0x121 + 0x140..0x143 (5 words = 10 ASCII bytes)
      this->device_name = read_string_words({0x121, 0x140, 0x141, 0x142, 0x143});

      // UniqueID (1BCh–1BFh): 4 x 16-bit = 64 bit, 16-char hex
      uint16_t uid0 = read_reg(0x1BC);
      uint16_t uid1 = read_reg(0x1BD);
      uint16_t uid2 = read_reg(0x1BE);
      uint16_t uid3 = read_reg(0x1BF);
      char uid_buf[20];
      snprintf(uid_buf, sizeof(uid_buf), "%04X%04X%04X%04X", uid3, uid2, uid1, uid0);
      this->unique_id = uid_buf;

      // DevName / PartNo (021h): low nibble distinguishes MAX17301/2/3 variant
      uint16_t devname_raw = read_reg(0x21);
      uint8_t variant = devname_raw & 0x000F;
      char part_buf[16];
      switch (variant) {
        case 0x1: snprintf(part_buf, sizeof(part_buf), "MAX17301"); break;
        case 0x2: snprintf(part_buf, sizeof(part_buf), "MAX17302"); break;
        case 0x3: snprintf(part_buf, sizeof(part_buf), "MAX17303"); break;
        default:  snprintf(part_buf, sizeof(part_buf), "MAX1730x(%X)", variant); break;
      }
      this->chip_part = part_buf;

      ESP_LOGD("max17301", "Age: %d%%, Cycles: %.2f, Temp: %.1fC (Min: %dC, Max: %dC)",
               this->age,
               this->cycles,
               this->temp,
               this->temp_min,
               this->temp_max);
      ESP_LOGD("max17301", "Manufacturer: '%s', Device: '%s', Serial: %s",
               this->manufacturer.c_str(),
               this->device_name.c_str(),
               this->serial_number.c_str());
      ESP_LOGD("max17301", "Chip: %s (DevName raw 0x%04X), UniqueID: %s",
               this->chip_part.c_str(),
               devname_raw,
               this->unique_id.c_str());
      ESP_LOGD("max17301", "Capacity: Rep %.0fmAh / Full %.0fmAh, TTE: %.1fmin, TTF: %.1fmin",
               this->rep_cap,
               this->full_cap,
               this->tte,
               this->ttf);
      ESP_LOGD("max17301", "Status: %s, CommStat: %s (CHG_off=%d DIS_off=%d NVErr=%d), ProtStatus: %s",
               this->status_hex.c_str(),
               this->comm_stat_hex.c_str(),
               this->chg_fet_disabled ? 1 : 0,
               this->dis_fet_disabled ? 1 : 0,
               this->nv_error ? 1 : 0,
               this->prot_status_hex.c_str());
      this->info_logged_ = true;
    }



  }
};

static MAX17301 *my_max17301 = new MAX17301();