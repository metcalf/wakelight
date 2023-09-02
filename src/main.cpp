

#include "freertos/FreeRTOS.h"

#include "driver/rtc_io.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "soc/rtc.h"

#include <algorithm>
#include <iterator>
#include <string.h>
#include <vector>

#include "Button.h"
#include "Dotstar.h"
#include "LightManager.h"
#include "Power.h"
#include "app_config.h"
#include "bt.h"
#include "helpers.h"
#include "light.h"
#include "network_time_manager.h"
#include "wifi_credentials.h"

#define ACTION_FADE_MS_PER_STEP 118 // ~30s (1000 * 30 / 255)
#define BUTTON_FADE_MS_PER_STEP 4   // ~1 second
#define BUTTON_HOLD_MS 5 * 1000
#define ERROR_SLEEP_DURATION_S 60 * 30
#define WAKE_ON_MINS 60
// #define NAP_MINS 90
#define PRESLEEP_MINS 60

#define WAKE_IDX __COUNTER__
#define WAKE_OFF_IDX __COUNTER__
// #define NAP_IDX __COUNTER__
#define NAP_WAKE_IDX __COUNTER__
#define NAP_OFF_IDX __COUNTER__
#define PRESLEEP_IDX __COUNTER__
#define SLEEP_IDX __COUNTER__

#define BUTTON_GPIO GPIO_NUM_4

// Config
std::vector<LightManager::Action> actions;
char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE];
char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE];

LightManager lightManager(actions);
Button button(BUTTON_GPIO, BUTTON_HOLD_MS, BUTTON_HOLD_MS);
Dotstar dotstar;
Power power;

uint64_t nextLightUpdateMillis;
uint8_t lastUpdateColor[3];
bool btWroteColor;

char color_access_buf[12];
char time_access_buf[6];

// TODO: Write tests for this
size_t strSplitToUL(const char *str, size_t strN, uint8_t *dest, size_t destN, char delim) {
  size_t resultN = 0;

  char *end = (char *)str + strN;
  char *start = (char *)str;
  for (size_t i = 0; i < destN; i++) {
    char *resultEnd = NULL;
    char *nextStart = std::find(start, end, delim);

    // TODO: Should probably handle errno here
    unsigned long result = strtoul(start, &resultEnd, 10);
    if ((resultEnd == NULL ||    // Consumed everything up to EOS
         resultEnd == nextStart) // or to the next delimeter
        && result <= 255         // Result is in range
    ) {
      dest[i] = result;
      resultN++;
    }

    if (nextStart == end) {
      break; // Searched to the end of the string
    }
    start = nextStart + 1;
  }

  return resultN;
}

int strAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  switch (op) {
  case BtOp::REQUEST_READ:
    *bytes = strnlen(chr->buffer, chr->bufferSize + 1);
    break;
  case BtOp::WRITTEN:
    chr->buffer[*bytes] = 0;
    break;
  }

  return 0;
}

int wifiSsidAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  int ret = strAccessCb(bytes, chr, op);
  if (op == BtOp::WRITTEN) {
    config_set_ssid(wifi_ssid);
  }

  return ret;
}

int wifiPswdAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  int ret = strAccessCb(bytes, chr, op);
  if (op == BtOp::WRITTEN) {
    config_set_pswd(wifi_pswd);
  }

  return ret;
}

int colorAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  uint8_t color[3];
  switch (op) {
  case BtOp::REQUEST_READ:
    light_get_color(color);
    *bytes = snprintf(chr->buffer, chr->bufferSize, "%03d:%03d:%03d", color[0], color[1], color[2]);
    break;
  case BtOp::WRITTEN:
    chr->buffer[*bytes] = 0;
    size_t resultN = strSplitToUL(chr->buffer, *bytes, color, sizeof(color), ':');
    if (resultN != sizeof(color)) {
      ESP_LOGE("APP", "Invalid color string: %s", chr->buffer);
      return 1;
    }

    light_set_color(color, BUTTON_FADE_MS_PER_STEP);
    for (size_t i = 0; i < sizeof(color); i++) {
      (*actions.at(PRESLEEP_IDX).color)[i] = color[i];
    }

    btWroteColor = true;
    config_set_actions(actions);

    break;
  }

  return 0;
}

int timeAccessCb(size_t *bytes, const bt_chr *chr, BtOp op, LightManager::HrMin *target) {
  switch (op) {
  case BtOp::REQUEST_READ:
    *bytes = snprintf(chr->buffer, chr->bufferSize, "%02d:%02d", target->hour, target->minute);
    break;
  case BtOp::WRITTEN:
    chr->buffer[*bytes] = 0;
    uint8_t result[2];

    size_t resultN = strSplitToUL(chr->buffer, *bytes, result, sizeof(result), ':');
    if (resultN != sizeof(result) || result[0] >= 24 || result[1] >= 60) {
      ESP_LOGE("APP", "Invalid time string: %s", chr->buffer);
      return 1;
    }

    target->hour = result[0];
    target->minute = result[1];
  }

  return 0;
}

void setNextTime(LightManager::HrMin *time, LightManager::HrMin *next, int incr_mins) {
  next->hour = time->hour;
  next->minute = time->minute + incr_mins;
  if (next->minute >= 60) {
    next->hour++;
    next->minute -= 60;
  }
}

int presleepTimeAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  LightManager::HrMin *presleep = &actions[PRESLEEP_IDX].time;

  if (timeAccessCb(bytes, chr, op, presleep) != 0) {
    return 1;
  }
  if (op == BtOp::WRITTEN) {
    LightManager::HrMin *sleep = &actions[SLEEP_IDX].time;
    setNextTime(presleep, sleep, PRESLEEP_MINS);

    config_set_actions(actions);
    ESP_LOGI("APP", "Set presleep %02d:%02d, sleep %02d:%02d", presleep->hour, presleep->minute,
             sleep->hour, sleep->minute);
  }

  return 0;
}

int napWakeTimeAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  LightManager::HrMin *wake = &actions[NAP_WAKE_IDX].time;

  if (timeAccessCb(bytes, chr, op, wake) != 0) {
    return 1;
  }
  if (op == BtOp::WRITTEN) {
    LightManager::HrMin *off = &actions[NAP_OFF_IDX].time;
    setNextTime(wake, off, WAKE_ON_MINS);

    config_set_actions(actions);
    ESP_LOGI("APP", "Set nap wake %02d:%02d, off %02d:%02d", wake->hour, wake->minute, off->hour,
             off->minute);
  }

  return 0;
}

int wakeTimeAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  LightManager::HrMin *wake = &actions[WAKE_IDX].time;

  if (timeAccessCb(bytes, chr, op, wake) != 0) {
    return 1;
  }
  if (op == BtOp::WRITTEN) {
    LightManager::HrMin *off = &actions[WAKE_OFF_IDX].time;
    setNextTime(wake, off, WAKE_ON_MINS);

    config_set_actions(actions);
    ESP_LOGI("APP", "Set wake %02d:%02d, off %02d:%02d", wake->hour, wake->minute, off->hour,
             off->minute);
  }

  return 0;
}

int currentTimeAccessCb(size_t *bytes, const bt_chr *chr, BtOp op) {
  struct tm timeinfo;
  LightManager::HrMin curr = {0, 0};
  if (ntm_get_local_time(&timeinfo)) {
    curr.hour = timeinfo.tm_hour;
    curr.minute = timeinfo.tm_min;
  }

  if (timeAccessCb(bytes, chr, op, &curr) != 0) {
    return 1;
  }
  if (op == BtOp::WRITTEN) {
    ntm_set_offline_time(curr.hour, curr.minute);

    ESP_LOGI("APP", "Set current time %02d:%02d", curr.hour, curr.minute);
  }

  return 0;
}

// TODO: Handle race between this and button press
void enterSleep(uint64_t sleep_time_ms) {
  ESP_LOGI("APP", "Going to sleep");

  dotstar.setPower(false);

  bt_stop();
  ntm_disconnect();

  ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BUTTON_GPIO, 0));
  ESP_ERROR_CHECK(rtc_gpio_pullup_en(BUTTON_GPIO));
  // EXT1 uses a GPIO bitmask instead of the raw GPIO number
  ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << PWR_SENSE_GPIO, ESP_EXT1_WAKEUP_ANY_HIGH));
  ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(sleep_time_ms * 1000));

  // TODO: Just using light sleep for now because for some reason
  // deep sleep isn't waking up.
  // if (lightOn) {
  // Only light sleep appears to support running ledc
  // For some reason we need to explicitly tell the ESP32 to keep the 8mhz clock on for ledc.
  ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC8M, ESP_PD_OPTION_ON));
  ESP_ERROR_CHECK(esp_light_sleep_start());
  // } else {
  //   esp_deep_sleep_start();
  // }
}

uint64_t getNextSleepTime() {
  struct tm timeinfo;

  if (button.isActive() || light_is_fading() || bt_is_enabled()) {
    return 0;
  }
  if (ntm_get_local_time(&timeinfo)) {
    return nextLightUpdateMillis - millis64();
  }
  // If we haven't gotten the time for the first time, don't sleep unless we end
  // up in an error state. This effectively implements retries on the network logic
  // since it restarts on wake.
  if (ntm_has_error()) {
    return ERROR_SLEEP_DURATION_S * 1000;
  }
  return 0;
}

void loop() {
  struct tm timeinfo;
  LightManager::Next update;

  power.printState();

  if (nextLightUpdateMillis < millis64() || ntm_poll_clock_updated()) {
    if (ntm_get_local_time(&timeinfo)) {
      update = lightManager.update(timeinfo);
      ESP_LOGI("APP", "%02d:%02d R%03d|G%03d|B%03d next: %d\r\n", timeinfo.tm_hour, timeinfo.tm_min,
               (*update.color)[0], (*update.color)[1], (*update.color)[2], update.nextUpdateSecs);

      if (!std::equal(lastUpdateColor, std::end(lastUpdateColor), *update.color)) {
        for (size_t i = 0; i < 3; i++) {
          lastUpdateColor[i] = (*update.color)[i];
        }
        light_set_color(*update.color, ACTION_FADE_MS_PER_STEP);
      }

      nextLightUpdateMillis = millis64() + update.nextUpdateSecs * 1000;
    } else {
      ESP_LOGI("APP", "Awaiting time...");
      nextLightUpdateMillis = millis64() + 1000; // Check the time again in a second
    }
  }

  Button::CallbackReason buttonReason = button.poll();
  switch (buttonReason) {
  case Button::CallbackReason::PRESS_RELEASE:
    ESP_LOGI("APP", "Button: PRESS_RELEASE");
    if (bt_is_enabled()) {
      bt_stop();
      // TODO: There's probably a gentler way to reset this, plus we could only reset if
      // something changes.
      ntm_disconnect();
      ntm_connect(wifi_ssid, wifi_pswd);
      nextLightUpdateMillis = 0; // Force an update in case things have changed
      if (!btWroteColor) {
        light_set_color(lastUpdateColor, 0);
      }
    } else {
      light_toggle(BUTTON_FADE_MS_PER_STEP, lastUpdateColor);
    }
    break;
  case Button::CallbackReason::HOLD_START:
    ESP_LOGI("APP", "Button: HOLD_START");
    if (bt_is_enabled()) {
      // If we hold again after Bluetooth is enabled, restart.
      esp_restart();
    } else {
      // Set the color to blue to indicate the state but don't actually enable Bluetooth
      // until releasing the button since continuing to hold will trigger a restart
      // instead.
      light_set_color(LIGHT_COLOR_BLUE, 0);
    }

    break;
  case Button::CallbackReason::HOLD_REPEAT:
    // If we hold for double the bluetooth-enabled cycle, restart
    ESP_LOGI("APP", "Button: HOLD_REPEAT");
    esp_restart();
    break;
  case Button::CallbackReason::HOLD_RELEASE:
    btWroteColor = false;
    bt_start();
    ESP_LOGI("APP", "Button: HOLD_RELEASE");
    break;
  case Button::CallbackReason::NONE:
    break;
  }

  if (power.isPowered()) {
    // If WiFi is disabled (e.g. after sleep), re-enable it so we can fetch time
    if (!ntm_is_active()) {
      ntm_connect(wifi_ssid, wifi_pswd);
    }

    uint8_t color[3]{0, 0, 0};
    if (ntm_has_error()) {
      // Purple
      color[0] = 15;
      color[2] = 10;
    } else if (ntm_is_connected()) {
      color[1] = 10; // green
    } else {
      color[2] = 10; // blue
    }
    dotstar.setColor(color);
  } else {
    uint8_t color[3]{10, 0, 0};
    dotstar.setColor(color);

    uint64_t nextSleepTime = getNextSleepTime();
    if (nextSleepTime > 0) {
      enterSleep(nextSleepTime);
    }
  }
}

extern "C" void app_main() {
  esp_log_level_set("*", ESP_LOG_INFO);

  uart_set_baudrate(UART_NUM_0, 115200);

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  //Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // NB: I don't know if this is necessary/does anything
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_8MD256);

  ESP_LOGI("APP", "wakeup reason: %d\n", wakeup_reason);

  power.setup();

  if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
    ESP_ERROR_CHECK(rtc_gpio_deinit(BUTTON_GPIO));
  }

  // If wake was triggered by the button going low, the button should start its
  // press debounce routine.
  button.setup(wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  ESP_LOGI("APP", "Loading config");
  config_load(actions, wifi_ssid, wifi_pswd);
  ESP_LOGI("APP", "Loaded SSID: %s Pass: %s Wake time: %02d:%02d", wifi_ssid, wifi_pswd,
           actions[WAKE_IDX].time.hour, actions[WAKE_IDX].time.minute);

  ESP_LOGI("APP", "Configuring LEDs");
  light_setup();

  ESP_LOGI("APP", "Initializing network time manager");
  ntm_init();

  bt_register(bt_chr{.name = "wifi ssid",
                     .buffer = wifi_ssid,
                     .bufferSize = sizeof(wifi_ssid) - 1, // Ensure space for null termination
                     .readable = true,
                     .writable = true,
                     .access_cb = wifiSsidAccessCb});
  bt_register(bt_chr{.name = "wifi pass",
                     .buffer = wifi_pswd,
                     .bufferSize = sizeof(wifi_pswd) - 1, // Ensure space for null termination
                     .readable = false,
                     .writable = true,
                     .access_cb = wifiPswdAccessCb});
  bt_register(bt_chr{.name = "current light",
                     .buffer = color_access_buf,
                     .bufferSize = sizeof(color_access_buf),
                     .readable = true,
                     .writable = true,
                     .access_cb = colorAccessCb});
  bt_register(bt_chr{.name = "wake time",
                     .buffer = time_access_buf,
                     .bufferSize = sizeof(time_access_buf),
                     .readable = true,
                     .writable = true,
                     .access_cb = wakeTimeAccessCb});
  bt_register(bt_chr{.name = "nap wake time",
                     .buffer = time_access_buf,
                     .bufferSize = sizeof(time_access_buf),
                     .readable = true,
                     .writable = true,
                     .access_cb = napWakeTimeAccessCb});
  bt_register(bt_chr{.name = "sleep time",
                     .buffer = time_access_buf,
                     .bufferSize = sizeof(time_access_buf),
                     .readable = true,
                     .writable = true,
                     .access_cb = presleepTimeAccessCb});
  bt_register(bt_chr{.name = "current time",
                     .buffer = time_access_buf,
                     .bufferSize = sizeof(time_access_buf),
                     .readable = true,
                     .writable = true,
                     .access_cb = currentTimeAccessCb});

  while (1) {
    esp_task_wdt_reset();
    loop();
    // TODO: Consider switching tick rate back to 100hz and doing less with a loop
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
