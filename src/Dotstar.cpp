#include "Dotstar.h"

#include "driver/rtc_io.h"

#define DOTSTAR_CLK GPIO_NUM_12
#define DOTSTAR_DATA GPIO_NUM_2
#define DOTSTAR_PWR GPIO_NUM_13

void Dotstar::setPower(bool state) {
  if (state == state_) {
    return;
  }

  rtc_gpio_hold_dis(DOTSTAR_PWR);

  ESP_ERROR_CHECK(gpio_set_direction(DOTSTAR_PWR, state ? GPIO_MODE_OUTPUT
                                                        : GPIO_MODE_INPUT));
  if (state) {
    ESP_ERROR_CHECK(gpio_set_level(DOTSTAR_PWR, 0));
  };

  gpio_set_direction(DOTSTAR_DATA, state ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
  gpio_set_direction(DOTSTAR_CLK, state ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
  if (state) {
    gpio_pulldown_dis(DOTSTAR_DATA);
    gpio_pulldown_dis(DOTSTAR_CLK);
  } else {
    gpio_pulldown_en(DOTSTAR_DATA);
    gpio_pulldown_en(DOTSTAR_CLK);
  }

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
    gpio_set_level(DOTSTAR_DATA, 0);
    gpio_set_level(DOTSTAR_CLK, 0);
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
      gpio_set_level(DOTSTAR_DATA, 1);
    else
      gpio_set_level(DOTSTAR_DATA, 0);
    gpio_set_level(DOTSTAR_CLK, 1);
    gpio_set_level(DOTSTAR_CLK, 0);
  }
  delay(1);
}
