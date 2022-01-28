#include "Button.h"

#include "helpers.h"

static void globalOnInterrupt(void *arg) { ((Button *)arg)->onInterrupt(); }

void Button::setup(bool start_pressed) {
  gpio_config_t conf = {.pin_bit_mask = (1ULL << pin_),
                        .mode = GPIO_MODE_INPUT,
                        .pull_up_en = GPIO_PULLUP_ENABLE,
                        .pull_down_en = GPIO_PULLDOWN_DISABLE,
                        .intr_type = GPIO_INTR_DISABLE};
  ESP_ERROR_CHECK(gpio_config(&conf));
  ESP_ERROR_CHECK(gpio_isr_register(globalOnInterrupt, this, 0, NULL));

  // TODO: Verify behavior if we start up with the button pressed
  setState(start_pressed ? State::PRESS_DEBOUNCE : State::RELEASED);
};

// NB: This currently assumes it will be called quite frequently
Button::CallbackReason Button::poll() {
  CallbackReason reason = CallbackReason::NONE;

  if (release_start_) {
    release_start_ = false;
    return setState(State::RELEASE_DEBOUNCE);
  }

  if (millis64() < next_update_time_ms_) {
    return reason;
  }

  int is_pressed = (gpio_get_level(pin_) == 0);

  switch (state_) {
  case State::RELEASED:
    // If the switch is in the release state the timer should be stopped so
    // we shouldn't even reach this point.
    // Log.error("Unexpected RELEASED state in onTimer");
    break;
  case State::PRESS_DEBOUNCE:
    if (is_pressed) {
      reason = setState(State::PRESSED);
    } else {
      // Assume this was a transient and ignore it.
      // TODO: This might miss short presses so we might want to
      // check after a shorter time to detect transients.
      reason = setState(State::RELEASED);
    }
    break;
  case State::PRESSED:
    if (is_pressed) {
      reason = setState(State::HELD);
    } else {
      // We've released before triggering a "hold"
      reason = setState(State::RELEASE_DEBOUNCE);
    }
    break;
  case State::HELD:
    if (is_pressed) {
      reason = CallbackReason::HOLD_REPEAT;
      next_update_time_ms_ += hold_repeat_ms_;
    } else {
      reason = setState(State::RELEASE_DEBOUNCE);
    }
    break;
  case State::RELEASE_DEBOUNCE:
    if (is_pressed) {
      // Assume we released and re-pressed and now need to debounce the press.
      reason = setState(State::PRESS_DEBOUNCE);
    } else {
      reason = setState(State::RELEASED);
    }
    break;
  }

  return reason;
}

Button::CallbackReason Button::setState(Button::State state) {
  // TODO: Better error reporting that doesn't crash things

  // The only valid transition from RELEASED is to PRESS_DEBOUNCE since it
  // starts the timer.
  // assert(!(state_ == State::RELEASED && state != State::PRESS_DEBOUNCE &&
  //          state != State::RELEASED));

  // The only valid transition into HELD is from PRESSED
  // assert(!(state == State::HELD && state_ != State::PRESSED));
  CallbackReason reason = CallbackReason::NONE;

  switch (state) {
  case State::RELEASED:
    ESP_ERROR_CHECK(gpio_set_intr_type(pin_, GPIO_INTR_NEGEDGE));
    ESP_ERROR_CHECK(gpio_intr_enable(pin_));
    next_update_time_ms_ = -1;
    break;
  case State::RELEASE_DEBOUNCE:
    ESP_ERROR_CHECK(gpio_intr_disable(pin_));
    next_update_time_ms_ = millis64() + debounce_interval_ms_;

    if (state_ == State::HELD) {
      reason = CallbackReason::HOLD_RELEASE;
    } else {
      reason = CallbackReason::PRESS_RELEASE;
    }
    break;
  case State::PRESS_DEBOUNCE:
    ESP_ERROR_CHECK(gpio_intr_disable(pin_));
    next_update_time_ms_ = millis64() + debounce_interval_ms_;
    break;
  case State::PRESSED:
    if (hold_time_ms_ > 0) {
      next_update_time_ms_ = millis64() + hold_time_ms_ - debounce_interval_ms_;
    } else {
      next_update_time_ms_ = -1;
    }
    ESP_ERROR_CHECK(gpio_set_intr_type(pin_, GPIO_INTR_POSEDGE));
    ESP_ERROR_CHECK(gpio_intr_enable(pin_));
    break;
  case State::HELD:
    if (hold_repeat_ms_ > 0) {
      next_update_time_ms_ = millis64() + hold_repeat_ms_;
    } else {
      next_update_time_ms_ = -1;
    }

    reason = CallbackReason::HOLD_START;

    break;
  };

  state_ = state;

  return reason;
}

void Button::onInterrupt() {
  if (state_ == State::RELEASED) {
    setState(State::PRESS_DEBOUNCE);
  } else {
    ESP_ERROR_CHECK(gpio_intr_disable(pin_));
    release_start_ = true;
  }
}
