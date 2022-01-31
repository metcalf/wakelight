#include "Power.h"

#include "driver/rtc_io.h"
#include "esp_adc_cal.h"
#include "esp_log.h"

#include "helpers.h"

#define UPPER_DIVIDER 442
#define LOWER_DIVIDER 160
#define DEFAULT_VREF 1100                   // Default referance voltage in mv
#define BATT_VOLTAGE_CHANNEL ADC1_CHANNEL_7 // Battery voltage ADC input
#define PWR_SENSE_LOW_DELAY_MS 1000
#define STATE_REPORT_INTERVAL_SECS 60

static esp_adc_cal_characteristics_t adc_chars_;

void Power::setup() {
  gpio_config_t gpio_cfg{};
  gpio_cfg.mode = GPIO_MODE_INPUT;
  gpio_cfg.pin_bit_mask = (1ULL << PWR_SENSE_GPIO);
  ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
  rtc_gpio_hold_en(PWR_SENSE_GPIO);
}

void Power::printState() {
  bool powered = isPowered();
  if (powered == lastReportPoweredState_ &&
      nextStateReportMillis_ > millis64()) {
    return;
  }

  ESP_LOGI("APP", "Powered: %s Bat Voltage: %0.2f\n", powered ? "Y" : "N",
           getBatteryVoltage());

  lastReportPoweredState_ = powered;
  nextStateReportMillis_ = millis64() + STATE_REPORT_INTERVAL_SECS * 1000;
}

float Power::getBatteryVoltage() {
  uint32_t raw, mv;

  if (adc_chars_.adc_num == 0) {
    // Get ADC calibration values once
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_11db, ADC_WIDTH_BIT_12,
                             DEFAULT_VREF, &adc_chars_);
  }

  // only check voltage every 1 second
  if (nextVoltageTimeMillis_ - millis64() > 0) {
    nextVoltageTimeMillis_ = millis64() + 1000;

    // Configure ADC and grab voltage
    // TODO: We probably only have to do this once, unclear how that interacts with light sleep
    ESP_ERROR_CHECK(adc1_config_width((adc_bits_width_t)ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(
        adc1_config_channel_atten(BATT_VOLTAGE_CHANNEL, ADC_ATTEN_11db));
    raw = adc1_get_raw(BATT_VOLTAGE_CHANNEL); // Read of raw ADC value

    // Convert to calibrated mv then volts
    mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars_) *
         (LOWER_DIVIDER + UPPER_DIVIDER) / LOWER_DIVIDER;
    lastMeasuredVoltage_ = (float)mv / 1000.0;
  }

  return (lastMeasuredVoltage_);
};

bool Power::isPowered() {
  // We wait for the power sense pin to read LOW for at least 1s before
  // transitioning to an unpowered state but transition immediately to
  // a powered state on HIGH. This debounces plugging in the charge
  // cable. It also handles some instability that's probably caused
  // by choosing too high valued a resistor on the high side of the
  // voltage divider.
  if (gpio_get_level(PWR_SENSE_GPIO) == 0) {
    if (pwrSenseLowDeadline_ == 0) {
      pwrSenseLowDeadline_ = millis64() + PWR_SENSE_LOW_DELAY_MS;
    } else if (pwrSenseLowDeadline_ < millis64()) {
      return false;
    }
  } else {
    pwrSenseLowDeadline_ = 0;
  }
  return true;
}
