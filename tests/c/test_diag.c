#include "core/ms_diag.h"

#include <stdio.h>
#include <string.h>

#include "core/ms_memory.h"
#include "ms_test.h"

static void assertNoLeak(void) {
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, stats.liveBlocks);
  MS_ASSERT_EQ(0, stats.currentBytes);
}

MS_TEST(Diag, InitAndDestroy) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_EQ(0, msDiagListCount(&list));
  MS_ASSERT_TRUE(!msDiagListIsFull(&list));
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, InitFailureReturnsOom) {
  msMemResetStats();
  msMemSetFailAfter(0);
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_ERROR_OOM, msDiagListInit(&list, "test.ms"));
  msMemSetFailAfter(-1);
  assertNoLeak();
}

MS_TEST(Diag, ReportRecordsFields) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_TRUE(msDiagReport(&list, 10, 5, 103, "unexpected '%c'", 'x'));
  MS_ASSERT_EQ(1, msDiagListCount(&list));
  const struct MsDiag* diag = msDiagListAt(&list, 0);
  MS_ASSERT_TRUE(diag != NULL);
  MS_ASSERT_TRUE(diag->file == list.chunkName);
  MS_ASSERT_EQ(10, diag->line);
  MS_ASSERT_EQ(5, diag->column);
  MS_ASSERT_EQ(103, diag->code);
  MS_ASSERT_TRUE(strcmp(diag->message, "unexpected 'x'") == 0);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, LongMessageIsTruncated) {
  msMemResetStats();
  char longMessage[300];
  memset(longMessage, 'a', sizeof(longMessage) - 1);
  longMessage[sizeof(longMessage) - 1] = '\0';
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_TRUE(msDiagReport(&list, 1, 1, 1, "%s", longMessage));
  const struct MsDiag* diag = msDiagListAt(&list, 0);
  MS_ASSERT_EQ(MS_DIAG_MESSAGE_LEN - 1, strlen(diag->message));
  MS_ASSERT_EQ('\0', diag->message[MS_DIAG_MESSAGE_LEN - 1]);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, CapacityCapStopsAtTwenty) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  int accepted = 0;
  for (uint32_t i = 0; i < 25; ++i) {
    if (msDiagReport(&list, i + 1, 1, 100, "error %u", (unsigned)i)) {
      ++accepted;
    }
  }
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, accepted);
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, msDiagListCount(&list));
  MS_ASSERT_TRUE(msDiagListIsFull(&list));
  MS_ASSERT_TRUE(!msDiagReport(&list, 99, 1, 100, "too late"));
  MS_ASSERT_EQ(MS_DIAG_MAX_COUNT, msDiagListCount(&list));
  const struct MsDiag* first = msDiagListAt(&list, 0);
  MS_ASSERT_EQ(1, first->line);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, FormatProducesExpectedText) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  MS_ASSERT_TRUE(msDiagReport(&list, 1, 2, 103, "bad token"));
  char buffer[512];
  int written = msDiagFormat(msDiagListAt(&list, 0), buffer, sizeof(buffer));
  MS_ASSERT_TRUE(strcmp(buffer, "test.ms:1:2: error E103: bad token") == 0);
  MS_ASSERT_EQ(strlen(buffer), written);
  msDiagListDestroy(&list);
  assertNoLeak();
}

MS_TEST(Diag, PrintWritesOneLinePerEntry) {
  msMemResetStats();
  struct MsDiagList list;
  MS_ASSERT_EQ(MS_OK, msDiagListInit(&list, "test.ms"));
  msDiagReport(&list, 1, 1, 101, "first");
  msDiagReport(&list, 2, 3, 102, "second");
  // tmpfile() may fail in restricted environments; formatting itself is
  // covered by Diag.FormatProducesExpectedText, so a NULL FILE skips the
  // content check instead of failing.
  FILE* out = tmpfile();
  if (out != NULL) {
    msDiagListPrint(&list, out);
    rewind(out);
    char line[512];
    MS_ASSERT_TRUE(fgets(line, sizeof(line), out) != NULL);
    MS_ASSERT_TRUE(strcmp(line, "test.ms:1:1: error E101: first\n") == 0);
    MS_ASSERT_TRUE(fgets(line, sizeof(line), out) != NULL);
    MS_ASSERT_TRUE(strcmp(line, "test.ms:2:3: error E102: second\n") == 0);
    fclose(out);
  }
  msDiagListDestroy(&list);
  assertNoLeak();
}

static const MsTestCase msTestCases[] = {
    {"Diag.InitAndDestroy", testDiagInitAndDestroy},
    {"Diag.InitFailureReturnsOom", testDiagInitFailureReturnsOom},
    {"Diag.ReportRecordsFields", testDiagReportRecordsFields},
    {"Diag.LongMessageIsTruncated", testDiagLongMessageIsTruncated},
    {"Diag.CapacityCapStopsAtTwenty", testDiagCapacityCapStopsAtTwenty},
    {"Diag.FormatProducesExpectedText", testDiagFormatProducesExpectedText},
    {"Diag.PrintWritesOneLinePerEntry", testDiagPrintWritesOneLinePerEntry},
};

MS_TEST_MAIN(msTestCases)
