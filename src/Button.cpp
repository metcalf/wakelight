#include "Button.h"

#include "helpers.h"

// TODO: This should be some proper per-channel registration
static Button *inst;
static void onInstPressInterrupt() { inst->onPressInterrupt(); }
static void onInstReleaseInterrupt() { inst->onReleaseInterrupt(); }

void Button::setup(bool start_pressed) {
  inst = this;
  pinMode(pin_, INPUT_PULLUP);
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

  int is_pressed = (digitalRead(pin_) == LOW);

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
    attachInterrupt(pin_, onInstPressInterrupt, FALLING);
    next_update_time_ms_ = -1;
    break;
  case State::RELEASE_DEBOUNCE:
    detachInterrupt(pin_);
    next_update_time_ms_ = millis64() + debounce_interval_ms_;

    if (state_ == State::HELD) {
      reason = CallbackReason::HOLD_RELEASE;
    } else {
      reason = CallbackReason::PRESS_RELEASE;
    }
    break;
  case State::PRESS_DEBOUNCE:
    detachInterrupt(pin_);
    next_update_time_ms_ = millis64() + debounce_interval_ms_;
    break;
  case State::PRESSED:
    if (hold_time_ms_ > 0) {
      next_update_time_ms_ = millis64() + hold_time_ms_ - debounce_interval_ms_;
    } else {
      next_update_time_ms_ = -1;
    }
    attachInterrupt(pin_, onInstReleaseInterrupt, RISING);
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
