#ifndef MSLANG_TESTS_C_MS_TEST_H_
#define MSLANG_TESTS_C_MS_TEST_H_

#include <stddef.h>
#include <stdio.h>

typedef void (*MsTestFunc)(void);

typedef struct {
  const char* name;
  MsTestFunc func;
} MsTestCase;

static int msTestFailureCount = 0;
static const char* msTestCurrentName = "<unknown>";

#define MS_TEST(suite, name) static void test##suite##name(void)

#define MS_ASSERT_TRUE(condition) \
  do { \
    if (!(condition)) { \
      ++msTestFailureCount; \
      printf("FAIL %s:%d: %s: assertion failed: %s\n", __FILE__, __LINE__, \
             msTestCurrentName, #condition); \
    } \
  } while (0)

#define MS_ASSERT_EQ(expected, actual) \
  do { \
    long long msExpected_ = (long long)(expected); \
    long long msActual_ = (long long)(actual); \
    if (msExpected_ != msActual_) { \
      ++msTestFailureCount; \
      printf("FAIL %s:%d: %s: expected %lld, actual %lld\n", __FILE__, __LINE__, \
             msTestCurrentName, msExpected_, msActual_); \
    } \
  } while (0)

#define MS_TEST_MAIN(cases) \
  int main(void) { \
    size_t msTotal_ = sizeof(cases) / sizeof((cases)[0]); \
    size_t msFailed_ = 0; \
    for (size_t msI_ = 0; msI_ < msTotal_; ++msI_) { \
      msTestCurrentName = (cases)[msI_].name; \
      int msFailuresBefore_ = msTestFailureCount; \
      (cases)[msI_].func(); \
      if (msTestFailureCount == msFailuresBefore_) { \
        printf("PASS %s\n", (cases)[msI_].name); \
      } else { \
        ++msFailed_; \
        printf("FAIL %s\n", (cases)[msI_].name); \
      } \
    } \
    printf("%zu of %zu tests passed\n", msTotal_ - msFailed_, msTotal_); \
    return msFailed_ == 0 ? 0 : 1; \
  }

#endif  // MSLANG_TESTS_C_MS_TEST_H_
