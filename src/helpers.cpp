#include "helpers.h"

#include "esp_timer.h"

uint64_t millis64() { return esp_timer_get_time() / 1000; };
