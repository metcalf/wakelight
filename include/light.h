#pragma once

#include "stddef.h"
#include "stdint.h"

static uint8_t LIGHT_COLOR_BLUE[3]{0, 0, 255};
static uint8_t LIGHT_COLOR_OFF[3]{0, 0, 0};
static uint8_t LIGHT_COLOR_ON[3]{60, 48, 38};

void light_setup();

void light_set_color(uint8_t color[3], size_t fade_ms_per_step);
void light_get_color(uint8_t *color);

void light_toggle(size_t fade_ms_per_step);

bool light_is_fading();
