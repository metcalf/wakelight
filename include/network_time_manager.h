#pragma once

void ntm_init(const char *network_name, const char *network_pswd);
void ntm_connect();
void ntm_disconnect();
bool ntm_is_connected();
bool ntm_is_active();
bool ntm_poll_clock_updated();
bool ntm_get_local_time(struct tm *info);
