#include "core/ms_common.h"

#include <stdbool.h>
#include <string.h>

#include "ms_test.h"

MS_TEST(Common, ResultValuesMatchCApiSpec) {
  MS_ASSERT_EQ(0, MS_OK);
  MS_ASSERT_EQ(1, MS_ERROR_RUNTIME);
  MS_ASSERT_EQ(2, MS_ERROR_SYNTAX);
  MS_ASSERT_EQ(3, MS_ERROR_OOM);
}

MS_TEST(Common, ArrayLen) {
  int one[1] = {0};
  int five[5] = {0};
  int twenty[20] = {0};
  MS_ASSERT_EQ(1, MS_ARRAY_LEN(one));
  MS_ASSERT_EQ(5, MS_ARRAY_LEN(five));
  MS_ASSERT_EQ(20, MS_ARRAY_LEN(twenty));
}

MS_TEST(Common, AssertTruePasses) {
  MS_ASSERT(true);
  MS_ASSERT_TRUE(true);
}

static int unusedProbe(int unusedParam) {
  MS_UNUSED(unusedParam);
  return 42;
}

MS_TEST(Common, UnusedSilencesWarning) {
  MS_ASSERT_EQ(42, unusedProbe(7));
}

static void unreachableProbe(int value) {
  switch (value) {
    case 0:
      break;
    default:
      MS_UNREACHABLE();
  }
}

MS_TEST(Common, UnreachableCompilesAndPassesOnHandledPath) {
  unreachableProbe(0);
  MS_ASSERT_TRUE(true);
}

MS_TEST(Common, ResultNameCoversAllValues) {
  MS_ASSERT_TRUE(strcmp(msResultName(MS_OK), "MS_OK") == 0);
  MS_ASSERT_TRUE(strcmp(msResultName(MS_ERROR_RUNTIME), "MS_ERROR_RUNTIME") == 0);
  MS_ASSERT_TRUE(strcmp(msResultName(MS_ERROR_SYNTAX), "MS_ERROR_SYNTAX") == 0);
  MS_ASSERT_TRUE(strcmp(msResultName(MS_ERROR_OOM), "MS_ERROR_OOM") == 0);
}

MS_TEST(Common, ResultNameNeverReturnsNull) {
  MS_ASSERT_TRUE(msResultName((MsResult)999) != NULL);
}

static const MsTestCase msTestCases[] = {
    {"Common.ResultValuesMatchCApiSpec", testCommonResultValuesMatchCApiSpec},
    {"Common.ArrayLen", testCommonArrayLen},
    {"Common.AssertTruePasses", testCommonAssertTruePasses},
    {"Common.UnusedSilencesWarning", testCommonUnusedSilencesWarning},
    {"Common.UnreachableCompilesAndPassesOnHandledPath", testCommonUnreachableCompilesAndPassesOnHandledPath},
    {"Common.ResultNameCoversAllValues", testCommonResultNameCoversAllValues},
    {"Common.ResultNameNeverReturnsNull", testCommonResultNameNeverReturnsNull},
};

MS_TEST_MAIN(msTestCases)
