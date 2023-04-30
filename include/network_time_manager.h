#pragma once

#include "time.h"

void ntm_init();
void ntm_connect(const char *network_name, const char *network_pswd);
void ntm_disconnect();
void ntm_retry();
void ntm_set_offline_time(time_t hour, time_t min);
bool ntm_has_error();
bool ntm_is_connected();
bool ntm_is_active();
bool ntm_poll_clock_updated();
bool ntm_get_local_time(struct tm *info);
