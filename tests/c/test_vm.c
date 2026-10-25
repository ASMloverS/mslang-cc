#include <string.h>

#include "core/ms_memory.h"
#include "object/ms_object.h"
#include "object/ms_string.h"
#include "vm/ms_state_internal.h"
#include "ms_test.h"

MS_TEST(vm, stateLifecycleGlobalsAndErrors) {
  msMemResetStats();
  MsState* L = msNewState();
  MS_ASSERT_TRUE(L != NULL);
  if (L == NULL) {
    return;
  }

  MS_ASSERT_TRUE(msNewNil(L) == msNewNil(L));
  MS_ASSERT_TRUE(msNewBool(L, true) == msNewBool(L, true));
  MS_ASSERT_TRUE(msNewBool(L, false) != msNewBool(L, true));
  struct MsObject* answer = msNewInt(L, 42);
  struct MsObject* ratio = msNewFloat(L, 2.5);
  MS_ASSERT_EQ(42, msIntWordValue(answer));
  MS_ASSERT_TRUE(msFloatValue(ratio) == 2.5);

  struct MsString* name = msStringFromCStr(&L->heap,
      "a-global-name-longer-than-thirty-two-bytes");
  MS_ASSERT_TRUE(name != NULL);
  MS_ASSERT_EQ(MS_OK, msStateSetGlobal(L, name, answer));
  MS_ASSERT_TRUE(msStateGetGlobal(L, name) == answer);
  MS_ASSERT_TRUE(msStateGetGlobal(L, msStringFromCStr(&L->heap, "missing")) == NULL);

  msRaiseTypeError(L, "bad %s", "value");
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(L));
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(L), "TypeError: bad value") != NULL);
  msStateClearError(L);
  MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);

  msCloseState(L);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static const MsTestCase msTestCases[] = {
  {"Vm.StateLifecycleGlobalsAndErrors", testvmstateLifecycleGlobalsAndErrors}
};

MS_TEST_MAIN(msTestCases)
