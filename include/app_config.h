#pragma once

#include "LightManager.h"

#define APP_CONFIG_WIFI_SSID_SIZE 32
#define APP_CONFIG_WIFI_PSWD_SIZE 64

void config_load(std::vector<LightManager::Action> &actions,
                 char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE],
                 char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE]);
void config_set_ssid(const char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE]);
void config_set_pswd(const char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE]);
void config_set_actions(std::vector<LightManager::Action> &actions);
