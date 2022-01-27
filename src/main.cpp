#include <Arduino.h>

#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "esp_bt_main.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "soc/rtc.h"
#include <WiFi.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "Button.h"
#include "Dotstar.h"
#include "LightManager.h"
#include "TimeManager.h"
#include "wifi_credentials.h"

#define ACTION_FADE_TIME_SECS 5
#define BUTTON_FADE_TIME_SECS 1
#define BUTTON_HOLD_MS 5 * 1000
#define PWR_SENSE_LOW_DELAY_MS 1000

#define BUTTON_GPIO GPIO_NUM_4
#define LED_R_GPIO GPIO_NUM_27
#define LED_G_GPIO GPIO_NUM_14
#define LED_B_GPIO GPIO_NUM_15
#define BAT_VOLTAGE_GPIO GPIO_NUM_35
#define BAT_CHARGE_GPIO GPIO_NUM_34
#define PWR_SENSE_GPIO GPIO_NUM_26

std::vector<LightManager::Action> actions = {
    // Prewake
    // LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 25},
    //                      {255, 0, 0}},
    // Wake
    LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 30},
                         {255, 255, 0}},
    // Wake off
    LightManager::Action{LightManager::HrMin{.hour = 7, .minute = 30},
                         {0, 0, 0}},
    // Nightlight
    LightManager::Action{LightManager::HrMin{.hour = 18, .minute = 45},
                         {40, 40, 40}},
};
TimeManager timeManager(hardcoded_network_name, hardcoded_network_pswd);
LightManager lightManager(actions);
Button button(BUTTON_GPIO, BUTTON_HOLD_MS);
Dotstar dotstar;

unsigned long pwrSenseLowDeadline;
unsigned long nextLightUpdate;
RTC_DATA_ATTR uint8_t lastUpdateColor[3];
RTC_DATA_ATTR uint8_t currColor[3];
RTC_DATA_ATTR bool lightOn;
volatile uint8_t activeFades;
bool fadePending;

bool isPowered() {
  // We wait for the power sense pin to read LOW for at least 1s before
  // transitioning to an unpowered state but transition immediately to
  // a powered state on HIGH. This debounces plugging in the charge
  // cable. It also handles some instability that's probably caused
  // by choosing too high valued a resistor on the high side of the
  // voltage divider.
  if (digitalRead(PWR_SENSE_GPIO) == LOW) {
    if (pwrSenseLowDeadline == 0) {
      pwrSenseLowDeadline = millis() + PWR_SENSE_LOW_DELAY_MS;
    } else if (pwrSenseLowDeadline < millis()) {
      return false;
    }
  } else {
    pwrSenseLowDeadline = 0;
  }
  return true;
}

bool getLocalTimeNoDelay(struct tm *info) {
  time_t now;
  time(&now);
  localtime_r(&now, info);
  return info->tm_year > (2016 - 1900);
}

bool IRAM_ATTR onFadeEnd(const ledc_cb_param_t *param, void *user_arg) {
  if (param->event == LEDC_FADE_END_EVT) {
    activeFades &= ~(0x01 << param->channel); // Clear active channel bit
  }
  return false; // Callback does not request a yield
}

ledc_cbs_t cbs{.fade_cb = onFadeEnd};
void setupLEDs() {
  ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                                    .duty_resolution = LEDC_TIMER_8_BIT,
                                    .timer_num = LEDC_TIMER_0,
                                    .freq_hz = 1000,
                                    .clk_cfg = LEDC_USE_RTC8M_CLK};
  ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

  ESP_ERROR_CHECK(ledc_fade_func_install(0));

  gpio_num_t pins[3]{LED_R_GPIO, LED_G_GPIO, LED_B_GPIO};
  for (int i = 0; i < 3; i++) {
    ledc_channel_config_t ledc_channel = {.gpio_num = pins[i],
                                          .speed_mode = LEDC_LOW_SPEED_MODE,
                                          .channel = (ledc_channel_t)i,
                                          .intr_type = LEDC_INTR_DISABLE,
                                          .timer_sel = LEDC_TIMER_0,
                                          .duty = currColor[i],
                                          .hpoint = 0};
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_ERROR_CHECK(
        ledc_cb_register(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, &cbs, NULL));
    pinMode(pins[i], OUTPUT);
  }
}

void setup() {
  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  // NB: I don't know if this is necessary/does anything
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_8MD256);

  Serial.printf("wakeup reason: %d\n", wakeup_reason);

  if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
    rtc_gpio_deinit(BUTTON_GPIO);
    rtc_gpio_deinit(PWR_SENSE_GPIO);
  }

  pinMode(BUTTON_GPIO, INPUT_PULLUP);
  pinMode(PWR_SENSE_GPIO, INPUT);

  // If wake was triggered by the button going low, the button should start its
  // press debounce routine.
  button.setup(wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  Serial.println("Configuring LEDs");
  setupLEDs();

  if (isPowered()) {
    Serial.println("Initializing time manager");
    timeManager.init();
  }
}

void setColor(uint8_t color[3], size_t fade_time_secs) {
  bool isOn = false;
  for (size_t i = 0; i < 3; i++) {
    isOn = isOn || (color[i] != 0);
    currColor[i] = color[i];
  }
  lightOn = isOn;

  // The ESP32 misbehaves if you try to trigger a fade while another is running
  if (activeFades > 0) {
    Serial.println("Deferring setColor");
    fadePending = true;
    return;
  }

  Serial.printf("setColor: R%03d|G%03d|B%03d\n", color[0], color[1], color[2]);

  for (size_t i = 0; i < 3; i++) {
    activeFades |= 0x01 << i;
    ESP_ERROR_CHECK(ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
                                            (ledc_channel_t)i, color[i],
                                            fade_time_secs * 1000));
    ESP_ERROR_CHECK(ledc_fade_start(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i,
                                    LEDC_FADE_NO_WAIT));
  }
}

// TODO: Handle race between this and button press
void enterSleep(unsigned long sleep_time_ms) {
  Serial.println("Going to sleep");
  Serial.flush();

  dotstar.setPower(false);
  if (btStarted()) {
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    btStop();
  }
  WiFi.mode(WIFI_MODE_NULL);

  esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, LOW);
  rtc_gpio_pullup_en(BUTTON_GPIO);
  // EXT1 uses a GPIO bitmask instead of the raw GPIO number
  esp_sleep_enable_ext1_wakeup(0x01 << PWR_SENSE_GPIO,
                               ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup(sleep_time_ms * 1000);

  // TODO: Just using light sleep for now because for some reason
  // deep sleep isn't waking up.
  // if (lightOn) {
  // Only light sleep appears to support running ledc
  // For some reason we need to explicitly tell the ESP32 to keep the 8mhz clock on for ledc.
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC8M, ESP_PD_OPTION_ON);
  ESP_ERROR_CHECK(esp_light_sleep_start());
  // } else {
  //   esp_deep_sleep_start();
  // }
}

void loop() {
  struct tm timeinfo;
  LightManager::Next update;

  timeManager.poll(); // TODO: This doesn't do anything now

  if (nextLightUpdate < millis()) {
    if (getLocalTimeNoDelay(&timeinfo)) {
      update = lightManager.update(timeinfo);
      Serial.printf("%02d:%02d R%03d|G%03d|B%03d next: %d\r\n",
                    timeinfo.tm_hour, timeinfo.tm_min, update.color[0],
                    update.color[1], update.color[2], update.nextUpdateSecs);

      if (!std::equal(std::begin(lastUpdateColor), std::end(lastUpdateColor),
                      std::begin(update.color))) {
        for (size_t i = 0; i < 3; i++) {
          lastUpdateColor[i] = update.color[i];
        }
        setColor(update.color, ACTION_FADE_TIME_SECS);
      }

      nextLightUpdate = millis() + update.nextUpdateSecs * 1000;
    }
  }

  Button::CallbackReason buttonReason = button.poll();
  if (!fadePending) { // Don't keep enqueuing actions if we already have a fade pending
    switch (buttonReason) {
    case Button::CallbackReason::PRESS_RELEASE:
      Serial.println("Press!");
      if (lightOn) {
        uint8_t color[3]{0, 0, 0};
        setColor(color, BUTTON_FADE_TIME_SECS);
      } else {
        uint8_t color[3]{255, 255, 255};
        setColor(color, BUTTON_FADE_TIME_SECS);
      }
      break;
    case Button::CallbackReason::HOLD_START:
      Serial.println("Button held!");
      break;
    default:
      break;
    }
  }

  if (fadePending && activeFades == 0) {
    Serial.println("Applying pending fade");
    fadePending = false;
    setColor(currColor, BUTTON_FADE_TIME_SECS);
  }

  if (isPowered()) {
    uint8_t color[3]{0, 0, 0};
    if (WiFi.isConnected()) {
      color[1] = 10;
    } else {
      color[2] = 10;
    }
    dotstar.setColor(color);

    // If WiFi is disabled (e.g. after sleep), re-enable it so we can fetch time
    if (WiFi.getMode() != WIFI_MODE_STA) {
      timeManager.init();
    }
  } else {
    uint8_t color[3]{10, 0, 0};
    dotstar.setColor(color);

    if (!button.isActive() && !fadePending && activeFades == 0 &&
        nextLightUpdate > 0) {
      // TODO: Add some logic for periodic clock updates (may not be necessary)
      enterSleep((nextLightUpdate - millis()) * 1000);
    }
  }
}
