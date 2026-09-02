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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_encode_decode_roundtrip);
  RUN_TEST(test_encode_marks_approx);
  RUN_TEST(test_decode_rejects_unknown_table);
  RUN_TEST(test_decode_rejects_torn_line);
  RUN_TEST(test_decode_rejects_missing_row);
  return UNITY_END();
}
