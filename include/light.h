#pragma once

#include "stddef.h"
#include "stdint.h"

static uint8_t LIGHT_COLOR_BLUE[3]{0, 0, 255};
static uint8_t LIGHT_COLOR_OFF[3]{0, 0, 0};
static uint8_t LIGHT_COLOR_WHITE[3]{60, 48, 38};
static uint8_t LIGHT_COLOR_RED[3]{255, 25, 20};
static uint8_t LIGHT_COLOR_GREEN[3]{30, 90, 0};

void light_setup();

void light_set_color(uint8_t color[3], size_t fade_ms_per_step);
void light_get_color(uint8_t *color);

void light_toggle(size_t fade_ms_per_step, uint8_t last_update_color[3]);

bool light_is_fading();
