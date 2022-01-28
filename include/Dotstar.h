#pragma once

#include "stdint.h"

class Dotstar {
public:
  void setPower(bool state);
  void setColor(uint8_t color[3]);

private:
  bool state_;
  uint8_t color_[3];

  void swspi_out(uint8_t n);
};
