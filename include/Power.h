#pragma once

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
};
