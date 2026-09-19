#include "ms_test.h"

MS_TEST(MsTest, AssertEqPassesOnEquality) {
  MS_ASSERT_EQ(42, 40 + 2);
}

MS_TEST(MsTest, AssertTruePassesOnTrue) {
  MS_ASSERT_TRUE(1 < 2);
}

// Intentionally triggers a failed assertion to verify the failure counter
// increments and execution continues, then restores the counter so the
// suite still passes.
MS_TEST(MsTest, AssertEqCountsFailureWithoutAborting) {
  int failuresBefore = msTestFailureCount;
  MS_ASSERT_EQ(1, 2);
  MS_ASSERT_TRUE(msTestFailureCount == failuresBefore + 1);
  msTestFailureCount = failuresBefore;
}

MS_TEST(MsTest, AssertTrueCountsFailureWithoutAborting) {
  int failuresBefore = msTestFailureCount;
  MS_ASSERT_TRUE(1 > 2);
  MS_ASSERT_TRUE(msTestFailureCount == failuresBefore + 1);
  msTestFailureCount = failuresBefore;
}

static const MsTestCase kCases[] = {
    {"MsTest.AssertEqPassesOnEquality", testMsTestAssertEqPassesOnEquality},
    {"MsTest.AssertTruePassesOnTrue", testMsTestAssertTruePassesOnTrue},
    {"MsTest.AssertEqCountsFailureWithoutAborting", testMsTestAssertEqCountsFailureWithoutAborting},
    {"MsTest.AssertTrueCountsFailureWithoutAborting", testMsTestAssertTrueCountsFailureWithoutAborting},
};

MS_TEST_MAIN(kCases)
