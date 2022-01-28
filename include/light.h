#pragma once

#include "stddef.h"
#include "stdint.h"

void light_setup();

void light_set_color(uint8_t color[3], size_t fade_time_secs);

void light_toggle(size_t fade_time_secs);

bool light_is_fading();

void light_handle_pending();
