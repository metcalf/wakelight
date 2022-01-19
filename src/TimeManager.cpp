#include "TimeManager.h"

#include "Wifi.h"
#include "time.h"

const char *ntpServer = "pool.ntp.org";
const char *tz = "PST8PDT,M3.2.0,M11.1.0";

void TimeManager::init() {
  WiFi.setAutoReconnect(true);
  WiFi.begin(network_name_, network_pswd_);
  // This will start polling every 60m as long as we're connected to Wifi
  configTzTime(tz, "pool.ntp.org");
}

void TimeManager::poll() {
  // TODO: Implement polling that turns Wifi on and off based on charge state
  // Probably use sntp_set_time_sync_notification_cb to keep track of updates
}
