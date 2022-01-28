#include "Light.h"

#include "driver/ledc.h"
#include "esp_log.h"

#define LED_R_GPIO GPIO_NUM_27
#define LED_G_GPIO GPIO_NUM_14
#define LED_B_GPIO GPIO_NUM_15

RTC_DATA_ATTR uint8_t curr_color[3];
RTC_DATA_ATTR bool light_on;
volatile uint8_t active_fades;
bool fade_pending;

bool IRAM_ATTR on_fade_end(const ledc_cb_param_t *param, void *user_arg) {
  if (param->event == LEDC_FADE_END_EVT) {
    active_fades &= ~(0x01 << param->channel); // Clear active channel bit
  }
  return false; // Callback does not request a yield
}

ledc_cbs_t cbs = {.fade_cb = on_fade_end};
void light_setup() {
  ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                    .duty_resolution = LEDC_TIMER_8_BIT,
                                    .timer_num = LEDC_TIMER_0,
                                    .freq_hz = 1000,
                                    .clk_cfg = LEDC_USE_RTC8M_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  ESP_ERROR_CHECK(ledc_fade_func_install(0));

  gpio_num_t pins[3] = {LED_R_GPIO, LED_G_GPIO, LED_B_GPIO};
  for (int i = 0; i < 3; i++) {
    ledc_channel_config_t ledc_channel = {.gpio_num = pins[i],
                                          .speed_mode = LEDC_LOW_SPEED_MODE,
                                          .channel = (ledc_channel_t)i,
                                          .intr_type = LEDC_INTR_DISABLE,
                                          .timer_sel = LEDC_TIMER_0,
                                          .duty = curr_color[i],
                                          .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_ERROR_CHECK(
        ledc_cb_register(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, &cbs, NULL));
    ESP_ERROR_CHECK(gpio_set_direction(pins[i], GPIO_MODE_OUTPUT));
  }
}

void light_set_color(uint8_t color[3], size_t fade_time_secs) {
  if (fade_pending) {
    if (active_fades == 0) {
      // If there's a fade pending but we're done with the previous fade, just
      // throw out the pending fade so we can use the new value.
      fade_pending = false;
    } else {
      // Otherwise, ignore the input
      // TODO: Is this the best behavior?
      ESP_LOGI("APP", "Ignoring light_set_color when one is already pending");
      return;
    }
  }

  bool isOn = false;
  for (size_t i = 0; i < 3; i++) {
    isOn = isOn || (color[i] != 0);
    curr_color[i] = color[i];
  }
  light_on = isOn;

  // The ESP32 misbehaves if you try to trigger a fade while another is running
  if (active_fades != 0) {
    ESP_LOGI("APP", "Deferring setColor");
    fade_pending = true;
    return;
  }

  ESP_LOGI("APP", "setColor: R%03d|G%03d|B%03d\n", color[0], color[1],
           color[2]);

  for (size_t i = 0; i < 3; i++) {
    active_fades |= 0x01 << i;
    ESP_ERROR_CHECK(ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
                                            (ledc_channel_t)i, color[i],
                                            fade_time_secs * 1000));
    ESP_ERROR_CHECK(ledc_fade_start(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i,
                                    LEDC_FADE_NO_WAIT));
  }
}

void light_toggle(uint fade_time_secs) {
  uint8_t c;
  if (light_on) {
    c = 0;
  } else {
    c = 255;
  }
  uint8_t color[3]{c, c, c};
  light_set_color(color, fade_time_secs);
}

bool light_is_fading() { return fade_pending || active_fades != 0; }

void light_handle_pending() {
  if (!fade_pending) {
    return; // Nothing to do
  }
  if (active_fades != 0) {
    return; // Still handling the last fade
  }

  ESP_LOGI("APP", "Applying pending fade");
  fade_pending = false;
  light_set_color(curr_color, 1);
}
