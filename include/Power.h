#pragma once

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "stdint.h"

#define PWR_SENSE_GPIO GPIO_NUM_26

class Power {
public:
  void setup();
  void printState();
  float getBatteryVoltage();
  bool isPowered();

private:
  uint64_t nextStateReportMillis_;
  uint64_t pwrSenseLowDeadline_;
  uint64_t nextVoltageTimeMillis_;
  float lastMeasuredVoltage_;
  bool lastReportPoweredState_;
  adc_oneshot_unit_handle_t adc1Handle_;
  adc_cali_handle_t adcCaliHandle_;
};
