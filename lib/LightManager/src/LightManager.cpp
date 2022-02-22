#include "LightManager.h"

// Return 0 if equal, 1 if t1 > t2, else -1
int cmpHrMin(LightManager::HrMin t1, LightManager::HrMin t2) {
  if (t1.hour == t2.hour && t1.minute == t2.minute) {
    return 0;
  } else if (t1.hour > t2.hour || (t1.hour == t2.hour && t1.minute > t2.minute)) {
    return 1;
  } else {
    return -1;
  }
}

LightManager::Next LightManager::update(tm timeinfo) {
  HrMin now{(uint8_t)timeinfo.tm_hour, (uint8_t)timeinfo.tm_min};
  Action before = actions_.back();
  Action after = actions_.front();
  Action curr;
  for (size_t i = 0; i < actions_.size(); i++) {
    curr = actions_.at(i);

    if (cmpHrMin(curr.time, now) == 1) {
      // If this is the first position where `curr` is after the current time,
      // update `after` and break.
      after = curr;
      break;
    } else {
      // If `curr` is still before the current time, update `before` and
      before = curr;
    }
  }

  int next_update_hrs = after.time.hour - now.hour;
  if (cmpHrMin(after.time, now) < 0) {
    // If after is before now, it happens tomorrow so wrap around
    next_update_hrs += 24;
  }

  return Next{.color = {before.color[0], before.color[1], before.color[2]},
              .nextUpdateSecs =
                  (uint32_t)((next_update_hrs * 60 + (after.time.minute - now.minute)) * 60 -
                             timeinfo.tm_sec)};
}
