#include <unity.h>
#include "persist.h"

void test_config_roundtrip() {
  LocalConfig a{};
  a.autoDosing = true; a.ecTarget = 1.85f; a.dosingTime = 45;
  a.autoMixing = true; a.smartDosing = false; a.minWlDosing = 120;
  strcpy(a.plugMode, "refill"); a.refillCutoffMm = 380.0f;
  a.plugEnabled = true; strcpy(a.plugTopic, "sf500_107888_plug");
  a.ecSensorId = 3; a.wlSensorId = 13; a.ecFound = true; a.wlFound = true;
  a.lastRainResetDay = 2;

  LocalConfig b{};
  TEST_ASSERT_TRUE(configFromJson(configToJson(a), b));
  TEST_ASSERT_EQUAL(a.autoDosing, b.autoDosing);
  TEST_ASSERT_FLOAT_WITHIN(0.001, a.ecTarget, b.ecTarget);
  TEST_ASSERT_EQUAL(a.dosingTime, b.dosingTime);
  TEST_ASSERT_EQUAL(a.autoMixing, b.autoMixing);
  TEST_ASSERT_EQUAL(a.minWlDosing, b.minWlDosing);
  TEST_ASSERT_EQUAL_STRING(a.plugMode, b.plugMode);
  TEST_ASSERT_FLOAT_WITHIN(0.01, a.refillCutoffMm, b.refillCutoffMm);
  TEST_ASSERT_EQUAL(a.plugEnabled, b.plugEnabled);
  TEST_ASSERT_EQUAL_STRING(a.plugTopic, b.plugTopic);
  TEST_ASSERT_EQUAL(a.ecSensorId, b.ecSensorId);
  TEST_ASSERT_EQUAL(a.wlSensorId, b.wlSensorId);
  TEST_ASSERT_EQUAL(a.ecFound, b.ecFound);
  TEST_ASSERT_EQUAL(a.wlFound, b.wlFound);
  TEST_ASSERT_EQUAL(a.lastRainResetDay, b.lastRainResetDay);
}

void test_config_rejects_missing_required() {
  LocalConfig b{};
  TEST_ASSERT_FALSE(configFromJson("{\"autoDosing\":true}", b));
}

void test_config_rejects_garbage() {
  LocalConfig b{};
  TEST_ASSERT_FALSE(configFromJson("not json", b));
}

void test_config_defaults_for_absent_optionals() {
  LocalConfig b{};
  // Only the required keys present — optionals must take safe defaults.
  TEST_ASSERT_TRUE(configFromJson("{\"ecTarget\":1.5,\"dosingTime\":30}", b));
  TEST_ASSERT_FALSE(b.autoDosing);
  TEST_ASSERT_EQUAL(0, b.minWlDosing);
  TEST_ASSERT_EQUAL_STRING("custom", b.plugMode);
  TEST_ASSERT_EQUAL(-1, b.lastRainResetDay);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_config_roundtrip);
  RUN_TEST(test_config_rejects_missing_required);
  RUN_TEST(test_config_rejects_garbage);
  RUN_TEST(test_config_defaults_for_absent_optionals);
  return UNITY_END();
}
