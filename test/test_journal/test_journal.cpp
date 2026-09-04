#include <unity.h>
#include "journal.h"

void test_encode_decode_roundtrip() {
  String line = journalEncode(1725283200, false, "activity_log",
      "{\"device\":\"sf500_107888\",\"category\":\"dosing\",\"action\":\"Dose 45s\"}");
  TEST_ASSERT_EQUAL(-1, (int)line.find('\n'));

  JournalRec r;
  TEST_ASSERT_TRUE(journalDecode(line, r));
  TEST_ASSERT_EQUAL(1725283200, r.t);
  TEST_ASSERT_FALSE(r.approx);
  TEST_ASSERT_EQUAL_STRING("activity_log", r.tbl.c_str());
  TEST_ASSERT_TRUE(r.row.find("Dose 45s") != String::npos);
  TEST_ASSERT_TRUE(r.row.find("sf500_107888") != String::npos);
}

void test_encode_marks_approx() {
  String line = journalEncode(1, true, "sensor_metrics", "{\"value\":1.2}");
  JournalRec r;
  TEST_ASSERT_TRUE(journalDecode(line, r));
  TEST_ASSERT_TRUE(r.approx);
}

void test_decode_rejects_unknown_table() {
  String line = journalEncode(1, false, "activity_log", "{}");
  size_t p = line.find("activity_log");
  line.replace(p, sizeof("activity_log") - 1, "evil_table");
  JournalRec r;
  TEST_ASSERT_FALSE(journalDecode(line, r));
}

void test_decode_rejects_torn_line() {
  JournalRec r;
  TEST_ASSERT_FALSE(journalDecode("{\"t\":1725283200,\"appro", r));
}

void test_decode_rejects_missing_row() {
  JournalRec r;
  TEST_ASSERT_FALSE(journalDecode("{\"t\":1,\"approx\":0,\"tbl\":\"relay_metrics\"}", r));
}

void test_compact_drops_consumed_prefix() {
  String all = "line1\nline2\nline3\n";
  String out = journalDropPrefix(all, 6);          // offset at the start of "line2"
  TEST_ASSERT_EQUAL_STRING("line2\nline3\n", out.c_str());
}

void test_compact_offset_mid_line_snaps_forward() {
  String all = "line1\nline2\nline3\n";
  String out = journalDropPrefix(all, 8);          // mid "line2"
  TEST_ASSERT_EQUAL_STRING("line3\n", out.c_str());
}

void test_compact_offset_zero_is_identity() {
  String all = "a\nb\n";
  TEST_ASSERT_EQUAL_STRING("a\nb\n", journalDropPrefix(all, 0).c_str());
}

void test_compact_offset_past_end_is_empty() {
  String all = "a\nb\n";
  TEST_ASSERT_EQUAL_STRING("", journalDropPrefix(all, 99).c_str());
}

void test_eviction_keeps_audit_drops_oldest_sensor() {
  String all =
    "{\"t\":1,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n"
    "{\"t\":2,\"approx\":0,\"tbl\":\"activity_log\",\"row\":{}}\n"
    "{\"t\":3,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n"
    "{\"t\":4,\"approx\":0,\"tbl\":\"relay_metrics\",\"row\":{}}\n"
    "{\"t\":5,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n";
  // Each line is 51 bytes. audit (t2,t4) = 102. Budget 155 -> room for ~1 sensor line.
  String out = journalEvictSensorMetrics(all, 155);
  TEST_ASSERT_TRUE(out.find("\"t\":2") != String::npos);   // activity kept
  TEST_ASSERT_TRUE(out.find("\"t\":4") != String::npos);   // relay kept
  TEST_ASSERT_TRUE(out.find("\"t\":5") != String::npos);   // newest sensor kept
  TEST_ASSERT_TRUE(out.find("\"t\":1") == String::npos);   // oldest sensor dropped
  TEST_ASSERT_TRUE(out.find("\"t\":3") == String::npos);   // 2nd-oldest sensor dropped
}

void test_eviction_keeps_all_audit_when_no_budget() {
  String all =
    "{\"t\":1,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{}}\n"
    "{\"t\":2,\"approx\":0,\"tbl\":\"activity_log\",\"row\":{}}\n";
  String out = journalEvictSensorMetrics(all, 1);          // no room for any sensor
  TEST_ASSERT_TRUE(out.find("\"t\":2") != String::npos);
  TEST_ASSERT_TRUE(out.find("\"t\":1") == String::npos);
}

void test_line_classifier_sensor_vs_audit() {
  // The classifier shared by the in-RAM and the streaming (device) retention
  // passes — it must agree with journalDecode's table match.
  String s = "{\"t\":1,\"approx\":0,\"tbl\":\"sensor_metrics\",\"row\":{\"value\":1.2}}\n";
  String a = "{\"t\":2,\"approx\":0,\"tbl\":\"activity_log\",\"row\":{\"action\":\"x\"}}\n";
  String r = "{\"t\":3,\"approx\":0,\"tbl\":\"relay_metrics\",\"row\":{\"relay_id\":\"relay01\"}}\n";
  TEST_ASSERT_TRUE (journalLineIsSensorMetrics(s.c_str(), s.length()));
  TEST_ASSERT_FALSE(journalLineIsSensorMetrics(a.c_str(), a.length()));
  TEST_ASSERT_FALSE(journalLineIsSensorMetrics(r.c_str(), r.length()));
  TEST_ASSERT_FALSE(journalLineIsSensorMetrics("", 0));
  TEST_ASSERT_FALSE(journalLineIsSensorMetrics("sensor_metri", 12));   // truncated, no match
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_roundtrip);
  RUN_TEST(test_encode_marks_approx);
  RUN_TEST(test_decode_rejects_unknown_table);
  RUN_TEST(test_decode_rejects_torn_line);
  RUN_TEST(test_decode_rejects_missing_row);
  RUN_TEST(test_compact_drops_consumed_prefix);
  RUN_TEST(test_compact_offset_mid_line_snaps_forward);
  RUN_TEST(test_compact_offset_zero_is_identity);
  RUN_TEST(test_compact_offset_past_end_is_empty);
  RUN_TEST(test_eviction_keeps_audit_drops_oldest_sensor);
  RUN_TEST(test_eviction_keeps_all_audit_when_no_budget);
  RUN_TEST(test_line_classifier_sensor_vs_audit);
  return UNITY_END();
}
