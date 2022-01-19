#pragma once

#include "stdint.h"
#include "time.h"
#include <vector>

class LightManager {
public:
  struct HrMin {
    uint8_t hour, minute;
  };

  struct Action {
    HrMin time;
    uint8_t color[3];
  };

  struct Next {
    uint8_t color[3];
    uint32_t nextUpdateSecs;
  };

  // actions must be in ascending order by time
  LightManager(std::vector<Action> actions) : actions_(actions){};

  Next update(tm timeinfo);

private:
  std::vector<Action> actions_;
};
