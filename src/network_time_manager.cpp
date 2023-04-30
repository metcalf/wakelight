#include "network_time_manager.h"

#include <cstring>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "time.h"

#include "zones.h"

#define MAX_HTTP_OUTPUT_BUFFER 128
#define MAX_RETRIES 5
#define WIFI_ACTIVE_BIT BIT0
#define WIFI_CONNECTED_BIT BIT1
#define WIFI_FAIL_BIT BIT2
#define TZ_READY_BIT BIT3
#define TZ_FAIL_BIT BIT4
#define CLOCK_UPDATED_BIT BIT5
#define JAN_1_2020_EPOCH 1577836800

const static char *TAG = "ntm";

static wifi_config_t s_wifi_config = {
    .sta =
        {
            .pmf_cfg = {.capable = true, .required = false},
        },
};

static int s_retry_num = 0;
static size_t s_http_output_len;
static char s_response_buffer[MAX_HTTP_OUTPUT_BUFFER]{};

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_ntm_event_group;
static TaskHandle_t s_tz_fetch_task_handle;

void sntp_sync_time(struct timeval *tv) {
  struct timeval old;
  gettimeofday(&old, NULL);

  settimeofday(tv, NULL);
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);

  if (old.tv_sec < 16e8) { // Before ~2020
    ESP_LOGI(TAG, "time initialized");
  } else {
    ESP_LOGI(TAG, "time updated, offset: %ld", (long)old.tv_sec - tv->tv_sec);
  }
  xEventGroupSetBits(s_ntm_event_group, CLOCK_UPDATED_BIT);
}

esp_err_t ntm_http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA: {
    ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    size_t n = MAX_HTTP_OUTPUT_BUFFER - s_http_output_len - 1; // -1 to ensure terminating null byte
    if (evt->data_len > n) {
      ESP_LOGE(TAG, "HTTP data size exceeded receive buffer");
    } else {
      n = evt->data_len;
    }

    memcpy(evt->user_data + s_http_output_len, evt->data, evt->data_len);
    s_http_output_len += evt->data_len;
    break;
  }
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    break;
  }
  return ESP_OK;
}

void ntm_set_posix_tz(const char *posix_str) {
  setenv("TZ", posix_str, 1);
  tzset();

  xEventGroupSetBits(s_ntm_event_group, TZ_READY_BIT);
  xEventGroupSetBits(s_ntm_event_group, CLOCK_UPDATED_BIT);
  xEventGroupClearBits(s_ntm_event_group, TZ_FAIL_BIT);
}

void ntm_tz_fetch_task(void *pvParameters) {
  while (1) {
    xTaskNotifyWaitIndexed(0, 0, ULONG_MAX, NULL, portMAX_DELAY);
    s_http_output_len = 0;
    /**
     * NOTE: All the configuration parameters for http_client must be spefied either in URL or as host and path parameters.
     * If host and path parameters are not set, query parameter will be ignored. In such cases,
     * query parameter should be specified in URL.
     *
     * If URL as well as host and path parameters are specified, values of host and path will be considered.
     */
    esp_http_client_config_t config = {
        .host = "ip-api.com",
        .path = "/csv",
        .query = "fields=status,message,timezone",
        .event_handler = ntm_http_event_handler,
        .user_data = s_response_buffer, // Pass address of local buffer to get response
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
               esp_http_client_get_status_code(client), esp_http_client_get_content_length(client));
    } else {
      ESP_LOGW(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);

    s_response_buffer[s_http_output_len] = 0; // Null-terminate string

    if (strncmp("success,", s_response_buffer, 8) == 0) {
      const char *response_tz = (char *)s_response_buffer + 8; // Advance past 'success,'
      // Replace newline with terminating null byte
      *strchr(response_tz, '\n') = 0;
      const char *posix_str = micro_tz_db_get_posix_str(response_tz);

      if (posix_str == NULL) {
        ESP_LOGE(TAG, "Unable to find POSIX string for zone %s", response_tz);
        xEventGroupSetBits(s_ntm_event_group, TZ_FAIL_BIT);
      } else {
        ESP_LOGI(TAG, "Setting TZ=%s for zone %s", posix_str, response_tz);
        ntm_set_posix_tz(posix_str);
      }
    } else {
      ESP_LOGE(TAG, "Error fetching timezone from IP: %s", s_response_buffer);
      xEventGroupSetBits(s_ntm_event_group, TZ_FAIL_BIT);
    }
  }

  vTaskDelete(NULL);
}

void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_ntm_event_group, WIFI_CONNECTED_BIT);

    uint8_t reason = ((wifi_event_sta_disconnected_t *)event_data)->reason;
    ESP_LOGI(TAG, "reason: %d", reason);
    // TODO: Arduino doesn't retry on AUTH_FAIL but sometime this seems necessary...
    // if (reason == WIFI_REASON_AUTH_FAIL) {

    //   esp_wifi_connect();
    //   ESP_LOGW(TAG, "wifi auth failed, not retrying");
    // } else
    if (s_retry_num < MAX_RETRIES) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retrying connection to AP");
    } else {
      ESP_LOGW(TAG, "failed to connect to the AP");
      xEventGroupSetBits(s_ntm_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_ntm_event_group, WIFI_CONNECTED_BIT);
    xEventGroupClearBits(s_ntm_event_group, WIFI_FAIL_BIT);

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;

    xTaskNotify(s_tz_fetch_task_handle, 0, eNoAction);

    // Restart sntp every time we reconnect to reset the polling timeout
    if (sntp_enabled()) {
      sntp_stop();
    }
    sntp_init();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    xEventGroupClearBits(s_ntm_event_group, WIFI_CONNECTED_BIT);
    ESP_LOGW(TAG, "lost IP");
    // TODO: Do we try a reconnect here?
  }
}

void ntm_connect(const char *network_name, const char *network_pswd) {
  memcpy(s_wifi_config.sta.ssid, network_name, sizeof(s_wifi_config.sta.ssid));
  memcpy(s_wifi_config.sta.password, network_pswd, sizeof(s_wifi_config.sta.password));

  xEventGroupSetBits(s_ntm_event_group, WIFI_ACTIVE_BIT);

  // Zero-length password
  s_wifi_config.sta.threshold.authmode =
      (network_pswd[0] == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");
}

void ntm_init() {
  s_ntm_event_group = xEventGroupCreate();

  xTaskCreate(&ntm_tz_fetch_task, "tz_fetch_task", 8192, NULL, tskIDLE_PRIORITY + 1,
              &s_tz_fetch_task_handle);

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler,
                                                      NULL, NULL));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));

  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
}

void ntm_disconnect() {
  ESP_ERROR_CHECK(esp_wifi_stop());
  xEventGroupClearBits(s_ntm_event_group, WIFI_ACTIVE_BIT);
}

void ntm_set_offline_time(time_t hour, time_t min) {
  ntm_set_posix_tz("UTC");

  // We add the epoch time for Jan 1, 2020 to our manual time since we treat very old
  // times as likely invalid in ntm_get_local_time.
  const timeval tv = timeval{.tv_sec = JAN_1_2020_EPOCH + (hour * 60 + min) * 60};

  settimeofday(&tv, NULL);
  ESP_LOGI(TAG, "time set manually");

  xEventGroupSetBits(s_ntm_event_group, CLOCK_UPDATED_BIT);
}

bool ntm_has_error() {
  return xEventGroupGetBits(s_ntm_event_group) & (WIFI_FAIL_BIT | TZ_FAIL_BIT);
}

bool ntm_is_connected() { return xEventGroupGetBits(s_ntm_event_group) & WIFI_CONNECTED_BIT; }

bool ntm_is_active() { return xEventGroupGetBits(s_ntm_event_group) & WIFI_ACTIVE_BIT; }

bool ntm_poll_clock_updated() {
  return xEventGroupClearBits(s_ntm_event_group, CLOCK_UPDATED_BIT) & CLOCK_UPDATED_BIT;
}

bool ntm_get_local_time(struct tm *info) {
  if (!(xEventGroupGetBits(s_ntm_event_group) & TZ_READY_BIT)) {
    return false;
  }

  time_t now;
  time(&now);
  localtime_r(&now, info);
  return info->tm_year > (2016 - 1900);
}
