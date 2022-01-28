

#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "soc/rtc.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include "Button.h"
#include "Dotstar.h"
#include "LightManager.h"
#include "Power.h"
#include "TimeManager.h"
#include "helpers.h"
#include "light.h"
#include "wifi_credentials.h"

#define ACTION_FADE_TIME_SECS 5
#define BUTTON_FADE_TIME_SECS 1
#define BUTTON_HOLD_MS 5 * 1000

#define BUTTON_GPIO GPIO_NUM_4

std::vector<LightManager::Action> actions = {
    // Prewake
    // LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 25},
    //                      {255, 0, 0}},
    // Wake
    LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 30},
                         {255, 200, 0}},
    // Wake off
    LightManager::Action{LightManager::HrMin{.hour = 7, .minute = 30},
                         {0, 0, 0}},
    // Nightlight
    LightManager::Action{LightManager::HrMin{.hour = 18, .minute = 45},
                         {40, 35, 30}},
};
TimeManager timeManager(hardcoded_network_name, hardcoded_network_pswd);
LightManager lightManager(actions);
Button button(BUTTON_GPIO, BUTTON_HOLD_MS);
Dotstar dotstar;
Power power;

uint64_t nextLightUpdate;
RTC_DATA_ATTR uint8_t lastUpdateColor[3];

void setup() {
  esp_log_level_set("*", ESP_LOG_INFO);

  Serial.begin(115200);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  power.setup();

  // NB: I don't know if this is necessary/does anything
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_8MD256);

  ESP_LOGI("APP", "wakeup reason: %d\n", wakeup_reason);

  if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
    rtc_gpio_deinit(BUTTON_GPIO);
  }

  pinMode(BUTTON_GPIO, INPUT_PULLUP);

  // If wake was triggered by the button going low, the button should start its
  // press debounce routine.
  button.setup(wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  ESP_LOGI("APP", "Configuring LEDs");
  light_setup();

  if (power.isPowered()) {
    ESP_LOGI("APP", "Initializing time manager");
    timeManager.init();
  }
}

// TODO: Handle race between this and button press
void enterSleep(uint64_t sleep_time_ms) {
  ESP_LOGI("APP", "Going to sleep");
  Serial.flush();

  dotstar.setPower(false);
  // TODO: I suspect these need to change for Nimble based BLE
  if (btStarted()) {
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

  power.printState();

  if (nextLightUpdate < millis64()) {
    if (timeManager.getLocalTime(&timeinfo)) {
      update = lightManager.update(timeinfo);
      ESP_LOGI("APP", "%02d:%02d R%03d|G%03d|B%03d next: %d\r\n",
               timeinfo.tm_hour, timeinfo.tm_min, update.color[0],
               update.color[1], update.color[2], update.nextUpdateSecs);

      if (!std::equal(std::begin(lastUpdateColor), std::end(lastUpdateColor),
                      std::begin(update.color))) {
        for (size_t i = 0; i < 3; i++) {
          lastUpdateColor[i] = update.color[i];
        }
        light_set_color(update.color, ACTION_FADE_TIME_SECS);
      }

      // TODO: recheck this after clock updates
      nextLightUpdate = millis64() + update.nextUpdateSecs * 1000;
    } else {
      nextLightUpdate = millis64() + 1000; // Check the time again in a second
    }
  }

  Button::CallbackReason buttonReason = button.poll();
  switch (buttonReason) {
  case Button::CallbackReason::PRESS_RELEASE:
    ESP_LOGI("APP", "Button: PRESS_RELEASE");
    light_toggle(BUTTON_FADE_TIME_SECS);
    break;
  case Button::CallbackReason::HOLD_START:
    ESP_LOGI("APP", "Button: HOLD_START");
    break;
  case Button::CallbackReason::HOLD_REPEAT:
    ESP_LOGI("APP", "Button: HOLD_REPEAT");
    break;
  case Button::CallbackReason::HOLD_RELEASE:
    ESP_LOGI("APP", "Button: HOLD_RELEASE");
    break;
  case Button::CallbackReason::NONE:
    break;
  }

  if (power.isPowered()) {
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

    if (!button.isActive() && !light_is_fading() && nextLightUpdate > 0) {
      // TODO: Add some logic for periodic clock updates (may not be necessary)
      enterSleep((nextLightUpdate - millis64()) * 1000);
    }
  }
}
