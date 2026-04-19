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
  int age = 0;            // Prozent (FullCapNom / DesignCap)
  float cycles = 0;       // Anzahl Vollzyklen
  float temp = 0;         // °C (aktuell)
  int8_t temp_max = 0;    // °C (seit letztem NV-Save)
  int8_t temp_min = 0;    // °C (seit letztem NV-Save)

 private:
  bool info_logged_ = false;

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

    // 3. Charging Logic (Current > 5000uA / 5mA)
    this->charging = current_uA > 5000;

    // 4. Temperature (Temp Register 01Bh)
    // Two's complement, LSb = 1/256 °C
    int16_t temp_raw = (int16_t)read_reg(0x1B);
    this->temp = (float)temp_raw / 256.0f;

    // 5. MaxMinTemp (009h): D15..D8 = Max, D7..D0 = Min, beide 8-bit two's complement, 1°C
    uint16_t mmt_raw = read_reg(0x09);
    this->temp_max = (int8_t)((mmt_raw >> 8) & 0xFF);
    this->temp_min = (int8_t)(mmt_raw & 0xFF);

    // 6. Age + Cycles (einmalig loggen)
    // Age Register (007h): Percentage, LSb = 1/256 %
    uint16_t age_raw = read_reg(0x07);
    this->age = (int)(age_raw >> 8);

    // Cycles Register (017h): 16-bit CycleCount
    // => Anzahl Vollzyklen = raw * 0.25 / 100 = raw / 400
    uint16_t cycles_raw = read_reg(0x17);
    this->cycles = (float)cycles_raw * 0.25f;

    // 7. EXACT Arduino Format Output (unverändert, bei jedem Read)
    ESP_LOGD("max17301", "SOC: %.0f%% VFSOC: %.0f%%, Current: %.2fmA, Charging: %s, Voltage: %.2fV",
             this->soc,
             this->vfsoc,
             this->current,
             this->charging ? "Yes" : "No",
             this->voltage);

    // 8. Einmalige Info-Ausgabe
    if (!this->info_logged_) {
      ESP_LOGD("max17301", "Age: %d%%, Cycles: %.2f, Temp: %.1f°C (Min: %d°C, Max: %d°C)",
               this->age,
               this->cycles,
               this->temp,
               this->temp_min,
               this->temp_max);
      this->info_logged_ = true;
    }
  }
};

static MAX17301 *my_max17301 = new MAX17301();