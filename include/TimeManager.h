#pragma once

// TimeManager owns the Wifi connection since that's all we use Wifi for right now
class TimeManager {
public:
  TimeManager(const char *network_name, const char *network_pswd)
      : network_name_(network_name), network_pswd_(network_pswd){};

  void init();
  void poll();

private:
  const char *network_name_;
  const char *network_pswd_;
};
