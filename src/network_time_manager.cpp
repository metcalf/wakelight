#include "network_time_manager.h"

#include <cstring>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "time.h"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
// #define WIFI_CONNECTED_BIT BIT0
// #define WIFI_FAIL_BIT BIT1

#define MAX_RETRIES 5

const char *TAG = "ntm";

static const char *s_tz = "PST8PDT,M3.2.0,M11.1.0";

static wifi_config_t s_wifi_config = {
    .sta =
        {
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold =
                {
                    .authmode = WIFI_AUTH_WPA2_PSK,
                },

            .pmf_cfg = {.capable = true, .required = false},
        },
};

static int s_retry_num = 0;
static bool is_connected = false;
static bool is_active = false;

/* FreeRTOS event group to signal when we are connected*/
// static EventGroupHandle_t s_wifi_event_group;

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
}

// TODO: Implement auto-reconnection, periodic retries, etc...
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    is_connected = false;
    if (s_retry_num < MAX_RETRIES) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      // TODO: Add periodic retries
      //xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    is_connected = true;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    //xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    is_connected = false;
    // TODO: Do we try a reconnect here?
  }
}

void ntm_connect() {
  is_active = true;

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
  // EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
  //                                        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
  //                                        pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
  // if (bits & WIFI_CONNECTED_BIT) {
  //   ESP_LOGI(TAG, "connected to wifi");
  // } else if (bits & WIFI_FAIL_BIT) {
  //   ESP_LOGI(TAG, "Failed to connect to wifi");
  // } else {
  //   ESP_LOGE(TAG, "UNEXPECTED EVENT");
  // }
}

void ntm_init(const char *network_name, const char *network_pswd) {
  memcpy(s_wifi_config.sta.ssid, network_name, sizeof(s_wifi_config.sta.ssid));
  memcpy(s_wifi_config.sta.password, network_pswd,
         sizeof(s_wifi_config.sta.ssid));

  // s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_init();
  setenv("TZ", s_tz, 1);
  tzset();
}

void ntm_disconnect() {
  ESP_ERROR_CHECK(esp_wifi_stop());
  is_active = false;
}

bool ntm_is_connected() { return is_connected; }

bool ntm_is_active() { return is_active; }

bool ntm_get_local_time(struct tm *info) {
  time_t now;
  time(&now);
  localtime_r(&now, info);
  return info->tm_year > (2016 - 1900);
}
