#include "Dotstar.h"

#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DOTSTAR_CLK GPIO_NUM_12
#define DOTSTAR_DATA GPIO_NUM_2
#define DOTSTAR_PWR GPIO_NUM_13

// Copied from Arduino HAL code
#define NOP() asm volatile("nop")
void delay_microseconds(uint32_t us) {
  uint64_t m = (uint64_t)esp_timer_get_time();
  if (us) {
    uint64_t e = (m + us);
    if (m > e) { //overflow
      while ((uint64_t)esp_timer_get_time() > e) {
        NOP();
      }
    }
    while ((uint64_t)esp_timer_get_time() < e) {
      NOP();
    }
  }
}

void Dotstar::setPower(bool state) {
  if (state == state_) {
    return;
  }

  ESP_ERROR_CHECK(rtc_gpio_hold_dis(DOTSTAR_PWR));

  gpio_config_t gpio_pwr_cfg{};
  gpio_pwr_cfg.mode = state ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
  gpio_pwr_cfg.pin_bit_mask = (1ULL << DOTSTAR_PWR);

  gpio_config_t gpio_spi_cfg = gpio_pwr_cfg;
  gpio_spi_cfg.pin_bit_mask = ((1ULL << DOTSTAR_DATA) | (1ULL << DOTSTAR_CLK));

  if (!state) {
    gpio_spi_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
  }

  ESP_ERROR_CHECK(gpio_config(&gpio_pwr_cfg));
  ESP_ERROR_CHECK(gpio_config(&gpio_spi_cfg));

  if (state) {
    ESP_ERROR_CHECK(gpio_set_level(DOTSTAR_PWR, 0));
    ESP_ERROR_CHECK(gpio_set_level(DOTSTAR_DATA, 0));
    ESP_ERROR_CHECK(gpio_set_level(DOTSTAR_CLK, 0));
  } else {
    ESP_ERROR_CHECK(rtc_gpio_isolate(DOTSTAR_PWR));
  }

  state_ = state;
}

void Dotstar::setColor(uint8_t color[3]) {
  bool color_changed = false;

  if (!state_) {
    color_changed = true;
    setPower(true);
    vTaskDelay(pdMS_TO_TICKS(10)); // TODO: Avoid blocking the main task here
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
  // TODO: Replace this bit banging with hardware SPI
  delay_microseconds(1);
}
