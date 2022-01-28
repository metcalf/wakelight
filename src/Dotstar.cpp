#include "Dotstar.h"

#include "Arduino.h"
#include "driver/rtc_io.h"

#define DOTSTAR_CLK 12
#define DOTSTAR_DATA 2
#define DOTSTAR_PWR GPIO_NUM_13

void Dotstar::setPower(bool state) {
  if (state == state_) {
    return;
  }

  rtc_gpio_hold_dis(DOTSTAR_PWR);

  pinMode(DOTSTAR_PWR, state ? OUTPUT : INPUT);
  digitalWrite(DOTSTAR_PWR, !state);
  pinMode(DOTSTAR_DATA, state ? OUTPUT : INPUT_PULLDOWN);
  pinMode(DOTSTAR_CLK, state ? OUTPUT : INPUT_PULLDOWN);

  if (!state) {
    rtc_gpio_isolate(DOTSTAR_PWR);
  }

  state_ = state;
}

void Dotstar::setColor(uint8_t color[3]) {
  bool color_changed = false;

  if (!state_) {
    color_changed = true;
    setPower(true);
    digitalWrite(DOTSTAR_DATA, LOW);
    digitalWrite(DOTSTAR_CLK, LOW);
    delay(10);
  }

  for (int i = 0; i < 3; i++) {
    color_changed = color_changed || (color_[i] != color[i]);
    color_[i] = color[i];
  }

  if (!color_changed) {
    return;
  }

  // Start-frame marker
  for (int i = 0; i < 4; i++)
    swspi_out(0x00);

  // Pixel start
  swspi_out(0xFF);

  for (int i = 2; i >= 0; i--) {
    swspi_out(color[i]); // R,G,B @Full brightness (no scaling)
  }

  // // End frame marker
  swspi_out(0xFF);
}

void Dotstar::swspi_out(uint8_t n) {
  for (uint8_t i = 8; i--; n <<= 1) {
    if (n & 0x80)
      digitalWrite(DOTSTAR_DATA, HIGH);
    else
      digitalWrite(DOTSTAR_DATA, LOW);
    digitalWrite(DOTSTAR_CLK, HIGH);
    digitalWrite(DOTSTAR_CLK, LOW);
  }
  delay(1);
}
