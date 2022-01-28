#include "TimeManager.h"

#include "Wifi.h"
#include "esp_sntp.h"
#include "time.h"

const char *TAG = "TimeManager";

static const char *ntpServer = "pool.ntp.org";
static const char *tz = "PST8PDT,M3.2.0,M11.1.0";

void sntp_sync_time(struct timeval *tv) {
  struct timeval old;
  gettimeofday(&old, NULL);

  settimeofday(tv, NULL);
  sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);

  if (old.tv_sec < 16e8) { // Before ~2020
    ESP_LOGI(TAG, "time initialized");
  } else {
    ESP_LOGI(TAG, "time updated, offset: %d", (long)old.tv_sec - tv->tv_sec);
  }
}

bool get_local_time_no_delay(struct tm *info) {
  time_t now;
  time(&now);
  localtime_r(&now, info);
  return info->tm_year > (2016 - 1900);
}

void TimeManager::init() {
  WiFi.setAutoReconnect(true);
  WiFi.begin(network_name_, network_pswd_);
  // This will start polling every 60m as long as we're connected to Wifi
  configTzTime(tz, "pool.ntp.org");
}

bool TimeManager::getLocalTime(struct tm *info) {
  return get_local_time_no_delay(info);
}
