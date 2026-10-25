#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/ms_memory.h"
#include "object/ms_object.h"
#include "object/ms_operator.h"
#include "object/ms_string.h"
#include "vm/ms_function.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "vm/ms_state_internal.h"
#include "ms_test.h"

static void msVmTestCloseState(MsState* L) {
  msMemSetFailAfter(-1);
  msCloseState(L);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

static struct MsObject* msVmTestCAddOne(MsState* L, int64_t argc, struct MsObject** argv) {
  if (argc != 1 || msObjectTag(argv[0]) != MS_TYPE_INT) {
    msRaiseTypeError(L, "expected one int");
    return NULL;
  }
  return msNewInt(L, msIntWordValue(argv[0]) + 1);
}

static struct MsProto* msVmTestRawProto(void) {
  struct MsProtoBuilder builder;
  msProtoBuilderInit(&builder);
  MS_ASSERT_EQ(0, msProtoEmitABC(&builder, MS_OP_LOAD_NIL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&builder, MS_OP_RETURN, 0, 0));
  builder.proto.stackSize = 1;
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&builder, &proto));
  return proto;
}

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

  char nameBuffer[32];
  for (int i = 0; i < 9; ++i) {
    snprintf(nameBuffer, sizeof(nameBuffer), "global_%d", i);
    struct MsString* globalName = msStringFromCStr(&L->heap, nameBuffer);
    MS_ASSERT_TRUE(globalName != NULL);
    MS_ASSERT_EQ(MS_OK, msStateSetGlobal(L, globalName, answer));
  }
  MS_ASSERT_EQ(10, L->globalsLen);
  MS_ASSERT_EQ(16, L->globalsCap);
  MS_ASSERT_TRUE(msStateGetGlobal(L, name) == answer);
  MS_ASSERT_TRUE(msStateGetGlobal(L, msStringFromCStr(&L->heap, "global_0")) == answer);
  MS_ASSERT_EQ(MS_OK, msStateSetGlobal(L, name, ratio));
  MS_ASSERT_EQ(10, L->globalsLen);
  MS_ASSERT_TRUE(msStateGetGlobal(L, name) == ratio);

  msRaiseTypeError(L, "bad %s", "value");
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(L));
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(L), "TypeError: bad value") != NULL);
  msStateClearError(L);
  MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);

  msRaiseRuntimeError(L, "");
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(L), ""));
  msRaiseRuntimeError(L, "first %s", "message");
  msRaiseRuntimeError(L, "latest %s", "message");
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(L), "latest message"));

  msVmTestCloseState(L);
}

MS_TEST(vm, functionObjects) {
  msMemResetStats();
  MsState* L = msNewState();
  MS_ASSERT_TRUE(L != NULL);
  if (L == NULL) {
    return;
  }
  struct MsProto* proto = msVmTestRawProto();
  MS_ASSERT_TRUE(proto != NULL);

  struct MsObject* function = msNewFunction(L, proto);
  MS_ASSERT_TRUE(function != NULL);
  MS_ASSERT_EQ(MS_TYPE_FUNCTION, msObjectTag(function));
  MS_ASSERT_TRUE(msFunctionProto(function) == proto);
  MS_ASSERT_TRUE(((struct MsFunction*)function)->upvalues == NULL);
  MS_ASSERT_EQ(0, ((struct MsFunction*)function)->upvalueCount);
  MS_ASSERT_TRUE(msAsCFunction(function) == NULL);

  struct MsString* name = msStringFromCStr(&L->heap, "addOne");
  struct MsObject* cFunction = msNewCFunction(L, name, msVmTestCAddOne);
  MS_ASSERT_TRUE(cFunction != NULL);
  MS_ASSERT_EQ(MS_TYPE_C_FUNCTION, msObjectTag(cFunction));
  MS_ASSERT_TRUE(msAsCFunction(cFunction) == msVmTestCAddOne);
  MS_ASSERT_TRUE(msFunctionProto(cFunction) == NULL);
  MS_ASSERT_TRUE(msFunctionProto(NULL) == NULL);
  MS_ASSERT_TRUE(msAsCFunction(NULL) == NULL);
  struct MsObject* args[] = {msNewInt(L, 41)};
  MS_ASSERT_EQ(42, msIntWordValue(msAsCFunction(cFunction)(L, 1, args)));

  msProtoFree(proto);
  msVmTestCloseState(L);
}

MS_TEST(vm, slowOperatorSemantics) {
  msMemResetStats();
  MsState* L = msNewState();
  MS_ASSERT_TRUE(L != NULL);
  if (L == NULL) {
    return;
  }
  struct MsObject* result = msObjectBinaryOp(L, MS_OP_ADD,
      msNewInt(L, INT64_MAX), msNewInt(L, 1));
  MS_ASSERT_TRUE(result == NULL);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(L));
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(L), "integer overflow"));
  msStateClearError(L);

  result = msObjectBinaryOp(L, MS_OP_ADD,
      (struct MsObject*)msStringFromCStr(&L->heap, "ab"),
      (struct MsObject*)msStringFromCStr(&L->heap, "cd"));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_EQ(0, strcmp(msStringData((struct MsString*)result), "abcd"));

  result = msObjectBinaryOp(L, MS_OP_FLOORDIV, msNewFloat(L, -7.0), msNewFloat(L, 2.0));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_TRUE(fabs(msFloatValue(result) - -4.0) < 1e-12);
  result = msObjectBinaryOp(L, MS_OP_MOD, msNewFloat(L, 7.0), msNewFloat(L, -3.0));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_TRUE(fabs(msFloatValue(result) - -2.0) < 1e-12);

  result = msObjectBinaryOp(L, MS_OP_BITAND, msNewFloat(L, 1.0), msNewFloat(L, 1.0));
  MS_ASSERT_TRUE(result == NULL);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(L));
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(L), "TypeError:") != NULL);
  msVmTestCloseState(L);
}

static const MsTestCase msTestCases[] = {
  {"Vm.StateLifecycleGlobalsAndErrors", testvmstateLifecycleGlobalsAndErrors},
  {"Vm.FunctionObjects", testvmfunctionObjects},
  {"Vm.SlowOperatorSemantics", testvmslowOperatorSemantics}
};

MS_TEST_MAIN(msTestCases)
