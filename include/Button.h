#pragma once

#include "driver/gpio.h"
#include "stdint.h"

class Button {
public:
  enum class CallbackReason {
    NONE,
    PRESS_RELEASE,
    HOLD_START,
    HOLD_REPEAT,
    HOLD_RELEASE,
  };

  Button(const gpio_num_t pin, uint hold_time_ms = 0, uint hold_repeat_ms = 100,
         uint debounce_interval_ms = 20)
      : pin_(pin), debounce_interval_ms_(debounce_interval_ms),
        hold_time_ms_(hold_time_ms), hold_repeat_ms_(hold_time_ms){};

  void setup(bool start_pressed);
  CallbackReason poll();

  void onInterrupt();

  bool isActive() { return state_ != State::RELEASED; }

private:
  gpio_num_t pin_;
  volatile uint64_t next_update_time_ms_ = -1;
  volatile bool release_start_ = false;

  uint debounce_interval_ms_;
  uint hold_time_ms_;
  uint hold_repeat_ms_;

  enum class State {
    RELEASED,
    PRESS_DEBOUNCE,
    PRESSED,
    HELD,
    RELEASE_DEBOUNCE,
  };
  volatile State state_ = State::RELEASED;

  CallbackReason setState(Button::State state);
};
