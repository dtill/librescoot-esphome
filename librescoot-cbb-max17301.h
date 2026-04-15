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

    // 3. Charging Logic (Current > 5000uA / 5mA)
    this->charging = current_uA > 5000;

    // 4. EXACT Arduino Format Output
    // SOC: 99% VFSOC: 50%, Current: -0.52mA, Charging: No, Voltage: 3.71V
    ESP_LOGD("max17301", "SOC: %.0f%% VFSOC: %.0f%%, Current: %.2fmA, Charging: %s, Voltage: %.2fV",
             this->soc,
             this->vfsoc,
             this->current,
             this->charging ? "Yes" : "No",
             this->voltage);
  }
};

static MAX17301 *my_max17301 = new MAX17301();