#include "app_config.h"

#include <cstring>

#include "esp_log.h"
#include "nvs_flash.h"

#include "light.h"
#include "wifi_credentials.h"

#define NVS_CONFIG_VERSION 3
#define STORAGE_NAMESPACE "config"

const static char *TAG = "ntm";

static std::vector<LightManager::Action> default_actions = {
    // Prewake
    // LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 25},
    //                      {255, 0, 0}},
    // Wake
    LightManager::Action{LightManager::HrMin{.hour = 6, .minute = 30}, {30, 90, 0}},
    // Wake off
    LightManager::Action{LightManager::HrMin{.hour = 7, .minute = 30}, {0, 0, 0}},
    // Nightlight
    LightManager::Action{LightManager::HrMin{.hour = 18, .minute = 45},
                         {LIGHT_COLOR_ON[0], LIGHT_COLOR_ON[1], LIGHT_COLOR_ON[2]}},
};

bool config_load_internal(nvs_handle_t handle, std::vector<LightManager::Action> &actions,
                          char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE],
                          char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE]) {

  uint16_t version = 0;
  esp_err_t err = nvs_get_u16(handle, "version", &version);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "No config stored");
    return false;
  }
  ESP_ERROR_CHECK(err);
  if (version != NVS_CONFIG_VERSION) {
    ESP_LOGI(TAG, "Stored config is v%d, want %d", version, NVS_CONFIG_VERSION);
    return false;
  }

  size_t length = APP_CONFIG_WIFI_SSID_SIZE;
  ESP_ERROR_CHECK(nvs_get_blob(handle, "ssid", wifi_ssid, &length));
  if (length == 0) {
    ESP_LOGI(TAG, "No SSID stored");
    return false;
  }

  length = APP_CONFIG_WIFI_PSWD_SIZE;
  ESP_ERROR_CHECK(nvs_get_blob(handle, "pswd", wifi_pswd, &length));
  if (length == 0) {
    ESP_LOGI(TAG, "No password stored");
    return false;
  }

  // Read the size of memory space required for blob
  size_t actions_size = 0; // value will default to 0, if not set yet in NVS
  ESP_ERROR_CHECK(nvs_get_blob(handle, "actions", NULL, &actions_size));

  if (actions_size == 0) {
    ESP_LOGI(TAG, "No actions stored");
    return false;
  }

  LightManager::Action *action_arr = (LightManager::Action *)malloc(actions_size);
  nvs_get_blob(handle, "actions", action_arr, &actions_size);

  actions.assign(action_arr, action_arr + actions_size / sizeof(LightManager::Action));

  return true;
}

void config_set_ssid_internal(nvs_handle_t handle,
                              const char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE]) {
  ESP_ERROR_CHECK(nvs_set_blob(handle, "ssid", wifi_ssid, APP_CONFIG_WIFI_SSID_SIZE));
}

void config_set_pswd_internal(nvs_handle_t handle,
                              const char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE]) {
  ESP_ERROR_CHECK(nvs_set_blob(handle, "pswd", wifi_pswd, APP_CONFIG_WIFI_PSWD_SIZE));
}

void config_set_actions_internal(nvs_handle_t handle, std::vector<LightManager::Action> &actions) {
  ESP_ERROR_CHECK(
      nvs_set_blob(handle, "actions", &actions[0], sizeof(LightManager::Action) * actions.size()));
}

void config_load(std::vector<LightManager::Action> &actions,
                 char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE],
                 char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE]) {
  nvs_handle_t handle;

  // Open
  ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle));

  bool success = config_load_internal(handle, actions, wifi_ssid, wifi_pswd);
  if (success) {
    return;
  }

  // Load defaults
  // TODO: Should we erase first?
  ESP_LOGI(TAG, "Using config defaults");
  config_set_ssid_internal(handle, default_wifi_ssid);
  config_set_pswd_internal(handle, default_wifi_pswd);
  config_set_actions_internal(handle, default_actions);

  ESP_ERROR_CHECK(nvs_set_u16(handle, "version", NVS_CONFIG_VERSION));

  ESP_ERROR_CHECK(nvs_commit(handle));
  nvs_close(handle);

  std::memcpy(wifi_ssid, default_wifi_ssid, sizeof(default_wifi_ssid));
  std::memcpy(wifi_pswd, default_wifi_pswd, sizeof(default_wifi_pswd));
  actions = default_actions;
}

void config_set_ssid(const char wifi_ssid[APP_CONFIG_WIFI_SSID_SIZE]) {
  nvs_handle_t handle;
  ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle));
  config_set_ssid_internal(handle, wifi_ssid);
  ESP_ERROR_CHECK(nvs_commit(handle));
  nvs_close(handle);
}

void config_set_pswd(const char wifi_pswd[APP_CONFIG_WIFI_PSWD_SIZE]) {
  nvs_handle_t handle;
  ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle));
  config_set_pswd_internal(handle, wifi_pswd);
  ESP_ERROR_CHECK(nvs_commit(handle));
  nvs_close(handle);
}

void config_set_actions(std::vector<LightManager::Action> &actions) {
  nvs_handle_t handle;
  ESP_ERROR_CHECK(nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle));
  config_set_actions_internal(handle, actions);
  ESP_ERROR_CHECK(nvs_commit(handle));
  nvs_close(handle);
}
