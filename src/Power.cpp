#include "Power.h"

#include "driver/rtc_io.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

#include "helpers.h"

#define UPPER_DIVIDER 442
#define LOWER_DIVIDER 160

#define PWR_SENSE_LOW_DELAY_MS 1000
#define STATE_REPORT_INTERVAL_SECS 60

#define BATT_VOLTAGE_CHANNEL ADC_CHANNEL_7 // Battery voltage ADC input
#define BATT_VOLTAGE_UNIT ADC_UNIT_1
#define BATT_VOLTAGE_ATTEN ADC_ATTEN_DB_11

const static char *TAG = "PWR";

static bool adc_calibration_init(adc_unit_t unit, adc_atten_t atten,
                                 adc_cali_handle_t *out_handle) {
  adc_cali_handle_t handle = NULL;
  esp_err_t ret = ESP_FAIL;
  bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  if (!calibrated) {
    ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
    if (ret == ESP_OK) {
      calibrated = true;
    }
  }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
  if (!calibrated) {
    ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
    if (ret == ESP_OK) {
      calibrated = true;
    }
  }
#endif

  if (ret == ESP_OK) {
    *out_handle = handle;
    ESP_LOGI(TAG, "ADC Calibration Success");
  } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
    ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
  } else {
    ESP_LOGE(TAG, "Invalid arg or no memory");
  }

  return calibrated;
}

void Power::setup() {
  // ADC init
  adc_oneshot_unit_init_cfg_t init_config1 = {
      .unit_id = BATT_VOLTAGE_UNIT,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1Handle_));

  // ADC config
  adc_oneshot_chan_cfg_t config = {
      .atten = BATT_VOLTAGE_ATTEN,
      .bitwidth = ADC_BITWIDTH_12,
  };

  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1Handle_, BATT_VOLTAGE_CHANNEL, &config));
  //ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1Handle_, EXAMPLE_ADC1_CHAN1, &config));

  // Calibration init
  adc_calibration_init(BATT_VOLTAGE_UNIT, BATT_VOLTAGE_ATTEN, &adcCaliHandle_);

  gpio_config_t gpio_cfg{};
  gpio_cfg.mode = GPIO_MODE_INPUT;
  gpio_cfg.pin_bit_mask = (1ULL << PWR_SENSE_GPIO);
  ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
  rtc_gpio_hold_en(PWR_SENSE_GPIO);
}

void Power::printState() {
  bool powered = isPowered();
  if (powered == lastReportPoweredState_ && nextStateReportMillis_ > millis64()) {
    return;
  }

  ESP_LOGI("APP", "Powered: %s Bat Voltage: %0.2f\n", powered ? "Y" : "N", getBatteryVoltage());

  lastReportPoweredState_ = powered;
  nextStateReportMillis_ = millis64() + STATE_REPORT_INTERVAL_SECS * 1000;
}

float Power::getBatteryVoltage() {
  int raw, mv;

  // only check voltage every 1 second
  if (nextVoltageTimeMillis_ - millis64() > 0) {
    nextVoltageTimeMillis_ = millis64() + 1000;

    ESP_ERROR_CHECK(adc_oneshot_read(adc1Handle_, BATT_VOLTAGE_CHANNEL, &raw));

    if (adcCaliHandle_ != NULL) {
      ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adcCaliHandle_, raw, &mv));
    }

    // Adjust for voltage divider and convert mv to volts
    mv = mv * (LOWER_DIVIDER + UPPER_DIVIDER) / LOWER_DIVIDER;
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
