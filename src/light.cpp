#include "Light.h"

#include <algorithm>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "helpers.h"

#define LED_R_GPIO GPIO_NUM_27
#define LED_G_GPIO GPIO_NUM_14
#define LED_B_GPIO GPIO_NUM_15

static uint64_t s_end_ms;
static uint8_t s_target_color[3];
static double s_delta_per_ms[3];

static StaticTimer_t s_fade_timer;
static TimerHandle_t s_fade_timer_handle;

void fade_timer_cb(void *pvParameters) {
  uint64_t now = millis64();
  bool done = now >= s_end_ms;

  for (size_t i = 0; i < 3; i++) {
    uint8_t duty;
    if (done) {
      duty = s_target_color[i];
    } else {
      duty = s_target_color[i] - s_delta_per_ms[i] * (s_end_ms - now);
    }
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i));
  }

  if (done) {
    s_end_ms = 0;
    xTimerStop(s_fade_timer_handle, 0);
  }
}

void light_setup() {
  ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                    .duty_resolution = LEDC_TIMER_8_BIT,
                                    .timer_num = LEDC_TIMER_0,
                                    .freq_hz = 1000,
                                    .clk_cfg = LEDC_USE_RTC8M_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  gpio_num_t pins[3] = {LED_R_GPIO, LED_G_GPIO, LED_B_GPIO};
  for (int i = 0; i < 3; i++) {
    ledc_channel_config_t ledc_channel = {.gpio_num = pins[i],
                                          .speed_mode = LEDC_LOW_SPEED_MODE,
                                          .channel = (ledc_channel_t)i,
                                          .intr_type = LEDC_INTR_DISABLE,
                                          .timer_sel = LEDC_TIMER_0,
                                          .duty = s_target_color[i],
                                          .hpoint = 0,
                                          .flags = {}};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
  }

  s_fade_timer_handle = xTimerCreateStatic("light_fade", pdMS_TO_TICKS(10), pdTRUE, NULL,
                                           fade_timer_cb, &s_fade_timer);
}

void light_set_color(uint8_t color[3], size_t fade_ms_per_step) {
  xTimerStop(s_fade_timer_handle, portMAX_DELAY);

  uint64_t now = millis64();
  uint8_t max_delta = 0;
  for (size_t i = 0; i < 3; i++) {
    uint8_t start_color = ledc_get_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    s_target_color[i] = color[i];
    s_delta_per_ms[i] = (double)((int64_t)s_target_color[i] - start_color);

    max_delta = std::max(max_delta, (uint8_t)abs((int)s_target_color[i] - start_color));
  }

  s_end_ms = now + max_delta * fade_ms_per_step;
  ESP_LOGI("APP", "setColor: R%03d|G%03d|B%03d now:%llu end:%llu\n", color[0], color[1], color[2],
           now, s_end_ms);

  if (fade_ms_per_step == 0) {
    s_end_ms = 0;
    fade_timer_cb(NULL);
  } else {
    for (size_t i = 0; i < 3; i++) {
      s_delta_per_ms[i] /= (s_end_ms - now);
    }

    xTimerStart(s_fade_timer_handle, 0);
  }
}

void light_get_color(uint8_t *color) {
  for (size_t i = 0; i < 3; i++) {
    color[i] = s_target_color[i];
  }
}

void light_toggle(uint fade_ms_per_step) {
  bool is_on = false;
  for (size_t i = 0; i < 3; i++) {
    is_on = is_on || (s_target_color[i] != 0);
  }

  if (is_on) {
    light_set_color(LIGHT_COLOR_OFF, fade_ms_per_step);
  } else {
    light_set_color(LIGHT_COLOR_WHITE, fade_ms_per_step);
  }
}

bool light_is_fading() { return s_end_ms > 0; }
