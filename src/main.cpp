#include <Arduino.h>

#include "driver/ledc.h"
#include "esp_sleep.h"
#include <WiFi.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "Button.h"
#include "LightManager.h"
#include "TimeManager.h"
#include "wifi_credentials.h"

#define ACTION_FADE_TIME_SECS 5
#define BUTTON_FADE_TIME_SECS 1
#define BUTTON_HOLD_MS 5 * 1000

#define DOTSTAR_CLK 12
#define DOTSTAR_DATA 2
#define DOTSTAR_PWR GPIO_NUM_13

#define BUTTON_GPIO 4
#define LED_R_GPIO 27
#define LED_G_GPIO 14
#define LED_B_GPIO 15

// Let's start with a version that's purely hardcoded and works here on Pacific time
// No bluetooth

std::vector<LightManager::Action> actions = {
    // Prewake
    LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 25},
                         {255, 0, 0}},
    // Wake
    LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 30},
                         {255, 255, 0}},
    // Wake off
    LightManager::Action{LightManager::HrMin{.hour = 7, .minute = 15},
                         {0, 0, 0}},
    // Nightlight
    LightManager::Action{LightManager::HrMin{.hour = 18, .minute = 45},
                         {255, 255, 255}},
};
TimeManager timeManager(hardcoded_network_name, hardcoded_network_pswd);
LightManager lightManager(actions);
Button button(BUTTON_GPIO, BUTTON_HOLD_MS);

unsigned long nextLightUpdate;
uint8_t lastUpdateColor[3];
uint8_t currColor[3];
bool lightOn;
unsigned long fadeUntilMs;
bool fadePending;

void setup() {
  Serial.begin(115200);

  Serial.println("Disabling dotstar");

  // Configure the dotstar never to draw power (even in sleep)
  pinMode(DOTSTAR_PWR, INPUT_PULLUP);
  gpio_hold_en(DOTSTAR_PWR);
  gpio_deep_sleep_hold_en();
  pinMode(DOTSTAR_CLK, INPUT);
  pinMode(DOTSTAR_DATA, INPUT);

  pinMode(BUTTON_GPIO, INPUT_PULLUP);

  Serial.println("Configuring LEDs");
  for (size_t i = 0; i < 3; i++) {
    ledcSetup(i, 1000, 8);
  }

  ledcAttachPin(LED_R_GPIO, 0);
  ledcAttachPin(LED_G_GPIO, 1);
  ledcAttachPin(LED_B_GPIO, 2);

  ledc_fade_func_install(0);

  timeManager.init();

  button.setup();
}

void setColor(uint8_t color[3], size_t fade_time_secs) {
  bool isOn = false;
  for (size_t i = 0; i < 3; i++) {
    isOn = isOn || (color[i] != 0);
    currColor[i] = color[i];
  }
  lightOn = isOn;

  // The ESP32 misbehaves if you try to trigger a fade while another is running
  if (fadeUntilMs > millis()) {
    Serial.println("Deferring setColor");
    fadePending = true;
    return;
  }

  Serial.printf("setColor: R%03d|G%03d|B%03d\n", color[0], color[1], color[2]);

  for (size_t i = 0; i < 3; i++) {
    ledc_set_fade_with_time(LEDC_HIGH_SPEED_MODE, (ledc_channel_t)i, color[i],
                            fade_time_secs * 1000);
    ledc_fade_start(LEDC_HIGH_SPEED_MODE, (ledc_channel_t)i, LEDC_FADE_NO_WAIT);
  }

  fadeUntilMs = millis() + fade_time_secs * 1000 + 1;
}

void loop() {
  struct tm timeinfo;
  LightManager::Next update;

  timeManager.poll(); // TODO: This doesn't do anything now

  if (nextLightUpdate < millis()) {
    if (!getLocalTime(&timeinfo)) {
      Serial.println("No time available yet");
    } else {
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

  if (fadePending && fadeUntilMs < millis()) {
    Serial.println("Applying pending fade");
    fadePending = false;
    setColor(currColor, BUTTON_FADE_TIME_SECS);
  }
}
