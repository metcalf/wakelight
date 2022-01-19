#include "time.h"
#include <unity.h>
#include <vector>

#include "LightManager.h"

using Action = LightManager::Action;
using HrMin = LightManager::HrMin;
using Next = LightManager::Next;

struct TestCase {
  HrMin now;
  Next expect;
};

void test_actions() {
  char msg[9];

  std::vector<TestCase> testCases{
      {HrMin{0, 0}, Next{{0, 0, 0}, 80 * 60}},
      {HrMin{1, 20}, Next{{255, 255, 255}, 70 * 60}},
      {HrMin{1, 21}, Next{{255, 255, 255}, 69 * 60}},
      {HrMin{2, 30}, Next{{0, 0, 0}, (22 * 60 + 50) * 60}},
      {HrMin{2, 31}, Next{{0, 0, 0}, (22 * 60 + 49) * 60}}};

  std::vector<Action> actions{
      Action{HrMin{1, 20}, {255, 255, 255}},
      Action{HrMin{2, 30}, {0, 0, 0}},
  };

  LightManager lightManager(actions);

  for (TestCase &testCase : testCases) {
    tm now{.tm_hour = testCase.now.hour, .tm_min = testCase.now.minute};
    Next actual = lightManager.update(now);
    snprintf(msg, sizeof(msg), "At %02d:%02d", testCase.now.hour,
             testCase.now.minute);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(testCase.expect.color, actual.color,
                                          3, msg);
    TEST_ASSERT_EQUAL_MESSAGE(testCase.expect.nextUpdateSecs,
                              actual.nextUpdateSecs, msg);
  }
}

int main(int argc, char **argv) {
  UNITY_BEGIN();
  RUN_TEST(test_actions);
  UNITY_END();

  return 0;
}
