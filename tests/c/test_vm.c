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
#include "vm/ms_vm.h"
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

  struct MsObject* empty = (struct MsObject*)msStringFromCStr(&L->heap, "");
  result = msObjectBinaryOp(L, MS_OP_ADD, empty, empty);
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_EQ(0, (long long)msStringLen((struct MsString*)result));

  const char nulLhs[] = {'a', '\0', 'b'};
  const char nulRhs[] = {'c', '\0', 'd'};
  const char nulExpected[] = {'a', '\0', 'b', 'c', '\0', 'd'};
  result = msObjectBinaryOp(L, MS_OP_ADD,
      (struct MsObject*)msStringNew(&L->heap, nulLhs, sizeof(nulLhs)),
      (struct MsObject*)msStringNew(&L->heap, nulRhs, sizeof(nulRhs)));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_EQ(6, (long long)msStringLen((struct MsString*)result));
  MS_ASSERT_EQ(0, memcmp(msStringData((struct MsString*)result), nulExpected, sizeof(nulExpected)));

  result = msObjectBinaryOp(L, MS_OP_FLOORDIV, msNewFloat(L, -7.0), msNewFloat(L, 2.0));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_TRUE(fabs(msFloatValue(result) - -4.0) < 1e-12);
  result = msObjectBinaryOp(L, MS_OP_MOD, msNewFloat(L, 7.0), msNewFloat(L, -3.0));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_TRUE(fabs(msFloatValue(result) - -2.0) < 1e-12);

  const MsOpCode divisionOps[] = {MS_OP_DIV, MS_OP_FLOORDIV, MS_OP_MOD};
  for (size_t i = 0; i < sizeof(divisionOps) / sizeof(divisionOps[0]); ++i) {
    result = msObjectBinaryOp(L, divisionOps[i], msNewFloat(L, 1.0), msNewFloat(L, 0.0));
    MS_ASSERT_TRUE(result == NULL);
    MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(L));
    MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(L), "division by zero"));
    msStateClearError(L);
  }

  result = msObjectBinaryOp(L, MS_OP_SHR, msNewInt(L, -2), msNewInt(L, 1));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_EQ(-1, msIntWordValue(result));
  result = msObjectBinaryOp(L, MS_OP_SHR, msNewInt(L, INT64_MIN), msNewInt(L, 63));
  MS_ASSERT_TRUE(result != NULL);
  MS_ASSERT_EQ(-1, msIntWordValue(result));

  result = msObjectBinaryOp(L, MS_OP_BITAND, msNewFloat(L, 1.0), msNewFloat(L, 1.0));
  MS_ASSERT_TRUE(result == NULL);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(L));
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(L), "TypeError:") != NULL);
  msVmTestCloseState(L);
}

struct MsVmFixture {
  MsState* L;
  struct MsCoroutine co;
  struct MsProtoBuilder builder;
  struct MsProto* proto;
  struct MsObject* result;
  MsResult status;
};

static void msVmFixtureInit(struct MsVmFixture* fixture) {
  memset(fixture, 0, sizeof(*fixture));
  msMemResetStats();
  fixture->L = msNewState();
  MS_ASSERT_TRUE(fixture->L != NULL);
  if (fixture->L == NULL) {
    return;
  }
  msCoroutineInit(&fixture->co);
  msProtoBuilderInit(&fixture->builder);
  fixture->builder.proto.name = msStringFromCStr(&fixture->L->heap, "<test>");
  fixture->builder.proto.sourceFile = msStringFromCStr(&fixture->L->heap, "test.ms");
  fixture->builder.proto.stackSize = 8;
}

static void msVmFixtureDestroy(struct MsVmFixture* fixture) {
  msMemSetFailAfter(-1);
  msProtoFree(fixture->proto);
  msProtoBuilderDestroy(&fixture->builder);
  msCoroutineDestroy(&fixture->co);
  msVmTestCloseState(fixture->L);
}

static void msVmFixtureRun(struct MsVmFixture* fixture) {
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&fixture->builder, &fixture->proto));
  fixture->status = msCallProto(fixture->L, &fixture->co, fixture->proto, 0, NULL, &fixture->result);
}

static struct MsObject* msVmFixtureTestInt(struct MsVmFixture* fixture, int64_t value) {
  struct MsObject* object = msNewInt(fixture->L, value);
  MS_ASSERT_TRUE(object != NULL);
  return object;
}

static void msVmFixtureTestConst(struct MsVmFixture* fixture, MsOpCode op) {
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture->builder, op, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture->builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(fixture);
  MS_ASSERT_EQ(MS_OK, fixture->status);
  MS_ASSERT_TRUE(fixture->result != NULL);
}

MS_TEST(vm, coroutineLifecycleAndNilReturn) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  MS_ASSERT_TRUE(fixture.co.frames == NULL);
  MS_ASSERT_TRUE(fixture.co.stack == NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);

  msVmFixtureTestConst(&fixture, MS_OP_LOAD_NIL);
  MS_ASSERT_TRUE(fixture.result == msNewNil(fixture.L));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, booleanConstantsLoad) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  msVmFixtureTestConst(&fixture, MS_OP_LOAD_TRUE);
  MS_ASSERT_EQ(MS_TYPE_BOOL, msObjectTag(fixture.result));
  MS_ASSERT_TRUE(msBoolValue(fixture.result));
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  msVmFixtureTestConst(&fixture, MS_OP_LOAD_FALSE);
  MS_ASSERT_EQ(MS_TYPE_BOOL, msObjectTag(fixture.result));
  MS_ASSERT_TRUE(!msBoolValue(fixture.result));
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, pushFrameBuildsWindowAndGrows) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.paramCount = 2;
  fixture.builder.proto.localCount = 4;
  fixture.builder.proto.stackSize = 70;
  struct MsObject* args[] = {
    msVmFixtureTestInt(&fixture, 10),
    msVmFixtureTestInt(&fixture, 20)
  };

  fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto, 2, args);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_EQ(1, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.frames[0].baseIndex);
  MS_ASSERT_TRUE(fixture.co.frames[0].upvalues == NULL);
  MS_ASSERT_TRUE(fixture.co.stackCap >= 75);
  MS_ASSERT_TRUE(fixture.co.stack[0] == msNewNil(fixture.L));
  MS_ASSERT_TRUE(fixture.co.stack[1] == args[0]);
  MS_ASSERT_TRUE(fixture.co.stack[2] == args[1]);
  MS_ASSERT_TRUE(fixture.co.stack[3] == msNewNil(fixture.L));
  MS_ASSERT_TRUE(fixture.co.stack[4] == msNewNil(fixture.L));
  MS_ASSERT_EQ(5, fixture.co.stackTop);

  for (int i = 0; i < 11; ++i) {
    fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto, 2, args);
    MS_ASSERT_EQ(MS_OK, fixture.status);
  }
  MS_ASSERT_EQ(12, fixture.co.frameCount);
  MS_ASSERT_TRUE(fixture.co.framesCap >= 12);
  MS_ASSERT_TRUE(fixture.co.stackCap >= 130);
  for (int i = 0; i < fixture.co.frameCount; ++i) {
    int base = i * 5 + 1;
    MS_ASSERT_EQ(base, fixture.co.frames[i].baseIndex);
    MS_ASSERT_TRUE(fixture.co.stack[base - 1] == msNewNil(fixture.L));
    MS_ASSERT_EQ(10, msIntWordValue(fixture.co.stack[base]));
    MS_ASSERT_EQ(20, msIntWordValue(fixture.co.stack[base + 1]));
    MS_ASSERT_TRUE(fixture.co.stack[base + 2] == msNewNil(fixture.L));
    MS_ASSERT_TRUE(fixture.co.stack[base + 3] == msNewNil(fixture.L));
  }
  MS_ASSERT_EQ(60, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, pushFrameValidatesArgumentsAndLimits) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.paramCount = 2;
  fixture.builder.proto.localCount = 4;
  struct MsObject* args[] = {
    msVmFixtureTestInt(&fixture, 10),
    msVmFixtureTestInt(&fixture, 20)
  };

  fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto, 1, args);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  MS_ASSERT_TRUE(fixture.co.frames == NULL);
  MS_ASSERT_TRUE(fixture.co.stack == NULL);

  msStateClearError(fixture.L);
  fixture.builder.proto.stackSize = MS_VM_STACK_MAX_CAP;
  fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto, 2, args);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "stack overflow") != NULL);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, pushFrameRelocatesAliasedArguments) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.paramCount = 2;
  fixture.builder.proto.localCount = 60;
  fixture.builder.proto.stackSize = 3;
  struct MsObject* args[] = {
    msVmFixtureTestInt(&fixture, 10),
    msVmFixtureTestInt(&fixture, 20)
  };
  fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto, 2, args);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_EQ(64, fixture.co.stackCap);
  struct MsObject** oldStack = fixture.co.stack;
  struct MsCallFrame* oldFrames = fixture.co.frames;
  struct MsCallFrame* newFrames = msAlloc((size_t)fixture.co.framesCap * sizeof(*newFrames));
  MS_ASSERT_TRUE(newFrames != NULL);
  memcpy(newFrames, oldFrames, (size_t)fixture.co.framesCap * sizeof(*newFrames));
  fixture.co.frames = newFrames;
  void* guard = msAlloc(512);
  MS_ASSERT_TRUE(guard != NULL);

  fixture.builder.proto.localCount = 4;
  fixture.builder.proto.stackSize = 70;
  fixture.co.frameCount = fixture.co.framesCap;
  fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto,
      2, &fixture.co.stack[1]);
  msFree(guard);
  msFree(oldFrames);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(oldStack != fixture.co.stack);
  MS_ASSERT_EQ(10, msIntWordValue(fixture.co.stack[1]));
  MS_ASSERT_EQ(20, msIntWordValue(fixture.co.stack[2]));
  MS_ASSERT_TRUE(fixture.co.stack[60] == msNewNil(fixture.L));
  MS_ASSERT_EQ(62, fixture.co.frames[fixture.co.frameCount - 1].baseIndex);
  MS_ASSERT_EQ(10, msIntWordValue(fixture.co.stack[62]));
  MS_ASSERT_EQ(20, msIntWordValue(fixture.co.stack[63]));
  MS_ASSERT_TRUE(fixture.co.stack[64] == msNewNil(fixture.L));
  MS_ASSERT_TRUE(fixture.co.stack[65] == msNewNil(fixture.L));
  MS_ASSERT_EQ(66, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, pushFrameOomMatchesErrorState) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.localCount = 0;
  fixture.builder.proto.stackSize = 0;
  msMemSetFailAfter(0);
  fixture.status = msVmPushFrame(fixture.L, &fixture.co, &fixture.builder.proto, 0, NULL);
  MS_ASSERT_EQ(MS_ERROR_OOM, fixture.status);
  MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(fixture.L));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, runValidatesStopDepthAndRecoversAfterUnderflow) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.status = msVmRun(fixture.L, &fixture.co, 0);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  fixture.status = msVmRun(fixture.L, &fixture.co, -1);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  fixture.status = msVmRun(fixture.L, &fixture.co, 1);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);

  msStateClearError(fixture.L);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:0: evaluation stack underflow"));
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, constantsLocalsGlobalsAndMove) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.localCount = 2;
  const char* globalName = "a-global-name-longer-than-thirty-two-bytes";
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 40)));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 2)));
  MS_ASSERT_EQ(2, msProtoAddConst(&fixture.builder,
      (struct MsObject*)msStringFromCStr(&fixture.L->heap, globalName)));

  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_STORE_LOCAL, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(4, msProtoEmitABC(&fixture.builder, MS_OP_STORE_LOCAL, 1, 0));
  MS_ASSERT_EQ(5, msProtoEmitABC(&fixture.builder, MS_OP_MOVE, 0, 1));
  MS_ASSERT_EQ(6, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_LOCAL, 0, 0));
  MS_ASSERT_EQ(7, msProtoEmitABC(&fixture.builder, MS_OP_STORE_GLOBAL, 0, 2));
  MS_ASSERT_EQ(8, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_GLOBAL, 0, 2));
  MS_ASSERT_EQ(9, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_EQ(2, msIntWordValue(fixture.result));
  struct MsString* sameName = msStringFromCStr(&fixture.L->heap, globalName);
  MS_ASSERT_TRUE(msStateGetGlobal(fixture.L, sameName) == fixture.result);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, jumpBackEdgeComputesLoop) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.localCount = 2;
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 0)));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 1)));
  MS_ASSERT_EQ(2, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 6)));

  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_STORE_LOCAL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_STORE_LOCAL, 1, 0));
  MS_ASSERT_EQ(4, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_LOCAL, 0, 0));
  MS_ASSERT_EQ(5, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_LOCAL, 1, 0));
  MS_ASSERT_EQ(6, msProtoEmitABC(&fixture.builder, MS_OP_ADD, 0, 0));
  MS_ASSERT_EQ(7, msProtoEmitABC(&fixture.builder, MS_OP_STORE_LOCAL, 0, 0));
  MS_ASSERT_EQ(8, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_LOCAL, 1, 0));
  MS_ASSERT_EQ(9, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(10, msProtoEmitABC(&fixture.builder, MS_OP_ADD, 0, 0));
  MS_ASSERT_EQ(11, msProtoEmitABC(&fixture.builder, MS_OP_STORE_LOCAL, 1, 0));
  MS_ASSERT_EQ(12, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_LOCAL, 1, 0));
  MS_ASSERT_EQ(13, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 2));
  MS_ASSERT_EQ(14, msProtoEmitABC(&fixture.builder, MS_OP_LT, 0, 0));
  MS_ASSERT_EQ(15, msProtoEmitSAx(&fixture.builder, MS_OP_JUMP_IF_FALSE, 1));
  MS_ASSERT_EQ(16, msProtoEmitSAx(&fixture.builder, MS_OP_JUMP, -13));
  MS_ASSERT_EQ(17, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_LOCAL, 0, 0));
  MS_ASSERT_EQ(18, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_EQ(15, msIntWordValue(fixture.result));
  msVmFixtureDestroy(&fixture);
}

static struct MsObject* msVmFixtureTestConditionValue(struct MsVmFixture* fixture, int valueIndex) {
  switch (valueIndex) {
    case 0:
      return msNewNil(fixture->L);
    case 1:
      return msNewBool(fixture->L, false);
    case 2:
      return msVmFixtureTestInt(fixture, 0);
    case 3:
      return (struct MsObject*)msFloatNew(&fixture->L->heap, 0.0);
    case 4:
      return (struct MsObject*)msStringFromCStr(&fixture->L->heap, "");
    case 5:
      return msNewBool(fixture->L, true);
    case 6:
      return msVmFixtureTestInt(fixture, -1);
    case 7:
      return (struct MsObject*)msFloatNew(&fixture->L->heap, 0.5);
    case 8:
      return (struct MsObject*)msStringFromCStr(&fixture->L->heap, "value");
    default:
      return NULL;
  }
}

MS_TEST(vm, conditionalJumpsCoverTruthiness) {
  const MsOpCode jumpOps[] = {MS_OP_JUMP_IF_FALSE, MS_OP_JUMP_IF_TRUE, MS_OP_JUMP_IF_NIL};
  const bool truthy[] = {false, false, false, false, false, true, true, true, true};
  for (size_t opIndex = 0; opIndex < sizeof(jumpOps) / sizeof(jumpOps[0]); ++opIndex) {
    for (int valueIndex = 0; valueIndex < 9; ++valueIndex) {
      struct MsVmFixture fixture;
      msVmFixtureInit(&fixture);
      struct MsObject* value = msVmFixtureTestConditionValue(&fixture, valueIndex);
      MS_ASSERT_TRUE(value != NULL);
      MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, value));
      MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 10)));
      MS_ASSERT_EQ(2, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 20)));
      MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
      MS_ASSERT_EQ(1, msProtoEmitSAx(&fixture.builder, jumpOps[opIndex], 2));
      MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
      MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
      MS_ASSERT_EQ(4, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 2));
      MS_ASSERT_EQ(5, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
      msVmFixtureRun(&fixture);

      bool shouldJump = jumpOps[opIndex] == MS_OP_JUMP_IF_NIL
          ? valueIndex == 0
          : truthy[valueIndex] == (jumpOps[opIndex] == MS_OP_JUMP_IF_TRUE);
      MS_ASSERT_EQ(MS_OK, fixture.status);
      MS_ASSERT_EQ(shouldJump ? 20 : 10, msIntWordValue(fixture.result));
      MS_ASSERT_EQ(0, fixture.co.frameCount);
      MS_ASSERT_EQ(1, fixture.co.stackTop);
      msVmFixtureDestroy(&fixture);
    }
  }
}

MS_TEST(vm, undefinedGlobalReportsLocationAndRestores) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder,
      (struct MsObject*)msStringFromCStr(&fixture.L->heap, "missing")));
  msProtoSetLine(&fixture.builder, 9);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_GLOBAL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "undefined name 'missing'") != NULL);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "test.ms:9") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, invalidOperandsReportRuntimeErrors) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "constant index out of bounds") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_MOVE, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "local index out of bounds") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 1)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_GLOBAL, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "invalid global name") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, integerAddOverflowUsesSlowPath) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, INT64_MAX)));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 1)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_ADD, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "integer overflow") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

static const MsTestCase msTestCases[] = {
  {"Vm.StateLifecycleGlobalsAndErrors", testvmstateLifecycleGlobalsAndErrors},
  {"Vm.FunctionObjects", testvmfunctionObjects},
  {"Vm.SlowOperatorSemantics", testvmslowOperatorSemantics},
  {"Vm.CoroutineLifecycleAndNilReturn", testvmcoroutineLifecycleAndNilReturn},
  {"Vm.BooleanConstantsLoad", testvmbooleanConstantsLoad},
  {"Vm.PushFrameBuildsWindowAndGrows", testvmpushFrameBuildsWindowAndGrows},
  {"Vm.PushFrameValidatesArgumentsAndLimits", testvmpushFrameValidatesArgumentsAndLimits},
  {"Vm.PushFrameRelocatesAliasedArguments", testvmpushFrameRelocatesAliasedArguments},
  {"Vm.PushFrameOomMatchesErrorState", testvmpushFrameOomMatchesErrorState},
  {"Vm.RunValidatesStopDepthAndRecoversAfterUnderflow", testvmrunValidatesStopDepthAndRecoversAfterUnderflow},
  {"Vm.ConstantsLocalsGlobalsAndMove", testvmconstantsLocalsGlobalsAndMove},
  {"Vm.JumpBackEdgeComputesLoop", testvmjumpBackEdgeComputesLoop},
  {"Vm.ConditionalJumpsCoverTruthiness", testvmconditionalJumpsCoverTruthiness},
  {"Vm.UndefinedGlobalReportsLocationAndRestores", testvmundefinedGlobalReportsLocationAndRestores},
  {"Vm.InvalidOperandsReportRuntimeErrors", testvminvalidOperandsReportRuntimeErrors},
  {"Vm.IntegerAddOverflowUsesSlowPath", testvmintegerAddOverflowUsesSlowPath}
};

MS_TEST_MAIN(msTestCases)
