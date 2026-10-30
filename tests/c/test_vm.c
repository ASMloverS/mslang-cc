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

static void msVmFixtureRunBinary(struct MsVmFixture* fixture, MsOpCode op,
    struct MsObject* lhs, struct MsObject* rhs) {
  int lhsIndex = msProtoAddConst(&fixture->builder, lhs);
  int rhsIndex = msProtoAddConst(&fixture->builder, rhs);
  MS_ASSERT_EQ(0, lhsIndex);
  MS_ASSERT_TRUE(rhsIndex >= 0);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, (uint16_t)rhsIndex));
  msProtoSetLine(&fixture->builder, 17);
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture->builder, op, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture->builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(fixture);
}

static void msVmFixtureRunUnary(struct MsVmFixture* fixture, MsOpCode op, struct MsObject* value) {
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture->builder, value));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 0));
  msProtoSetLine(&fixture->builder, 17);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture->builder, op, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture->builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(fixture);
}

struct MsVmIntBinaryCase {
  MsOpCode op;
  int64_t lhs;
  int64_t rhs;
  int64_t expected;
};

MS_TEST(vm, integerArithmeticAndShiftBoundaries) {
  const struct MsVmIntBinaryCase cases[] = {
    {MS_OP_ADD, 20, 22, 42},
    {MS_OP_ADD, INT64_MAX, 0, INT64_MAX},
    {MS_OP_ADD, INT64_MIN, 0, INT64_MIN},
    {MS_OP_SUB, 20, 22, -2},
    {MS_OP_SUB, INT64_MIN, 0, INT64_MIN},
    {MS_OP_MUL, -6, 7, -42},
    {MS_OP_MUL, INT64_MIN, 1, INT64_MIN},
    {MS_OP_FLOORDIV, -7, 2, -4},
    {MS_OP_FLOORDIV, 7, -2, -4},
    {MS_OP_MOD, -7, 2, 1},
    {MS_OP_MOD, 7, -3, -2},
    {MS_OP_POW, 2, 10, 1024},
    {MS_OP_POW, 2, 62, INT64_MAX / 2 + 1},
    {MS_OP_POW, -2, 63, INT64_MIN},
    {MS_OP_BITAND, 12, 10, 8},
    {MS_OP_BITOR, 12, 3, 15},
    {MS_OP_BITXOR, 12, 10, 6},
    {MS_OP_SHL, -3, 2, -12},
    {MS_OP_SHL, 1, 0, 1},
    {MS_OP_SHL, -1, 0, -1},
    {MS_OP_SHL, 0, 63, 0},
    {MS_OP_SHL, -1, 63, INT64_MIN},
    {MS_OP_SHL, 1, 62, INT64_MAX / 2 + 1},
    {MS_OP_SHR, -7, 2, -2},
    {MS_OP_SHR, INT64_MIN, 63, -1},
    {MS_OP_SHR, INT64_MAX, 63, 0},
    {MS_OP_SHR, -7, 0, -7},
    {MS_OP_SHR, 7, 0, 7}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunBinary(&fixture, cases[i].op,
        msVmFixtureTestInt(&fixture, cases[i].lhs), msVmFixtureTestInt(&fixture, cases[i].rhs));
    MS_ASSERT_EQ(MS_OK, fixture.status);
    MS_ASSERT_TRUE(fixture.result != NULL);
    if (fixture.result != NULL) {
      MS_ASSERT_EQ(MS_TYPE_INT, msObjectTag(fixture.result));
      MS_ASSERT_EQ(cases[i].expected, msIntWordValue(fixture.result));
    }
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(1, fixture.co.stackTop);
    msVmFixtureDestroy(&fixture);
  }
}

struct MsVmNumericCase {
  MsOpCode op;
  double lhs;
  double rhs;
  double expected;
  bool lhsIsInt;
  bool rhsIsInt;
};

static struct MsObject* msVmFixtureNumericValue(struct MsVmFixture* fixture, double value, bool isInt) {
  return isInt ? msVmFixtureTestInt(fixture, (int64_t)value) : msNewFloat(fixture->L, value);
}

MS_TEST(vm, numericArithmeticPromotesToFloat) {
  const struct MsVmNumericCase cases[] = {
    {MS_OP_ADD, 1, 2.5, 3.5, true, false},
    {MS_OP_ADD, 1.5, 2, 3.5, false, true},
    {MS_OP_ADD, 1.25, 2.5, 3.75, false, false},
    {MS_OP_SUB, 5, 2.5, 2.5, true, false},
    {MS_OP_SUB, 5.5, 2, 3.5, false, true},
    {MS_OP_SUB, 5.5, 2.25, 3.25, false, false},
    {MS_OP_MUL, -3, 2.5, -7.5, true, false},
    {MS_OP_MUL, -3.5, 2, -7, false, true},
    {MS_OP_MUL, -1.5, 2, -3, false, false},
    {MS_OP_DIV, 10, 4, 2.5, true, true},
    {MS_OP_DIV, 10, 4, 2.5, true, false},
    {MS_OP_DIV, 10, 4, 2.5, false, true},
    {MS_OP_DIV, 5, 2, 2.5, false, false},
    {MS_OP_FLOORDIV, -7, 2, -4, true, false},
    {MS_OP_FLOORDIV, -7, 2, -4, false, true},
    {MS_OP_FLOORDIV, -7, 2, -4, false, false},
    {MS_OP_MOD, 7, -3, -2, true, false},
    {MS_OP_MOD, 7, -3, -2, false, true},
    {MS_OP_MOD, 7, -3, -2, false, false},
    {MS_OP_POW, 2, -2, 0.25, true, true},
    {MS_OP_POW, 2, -2, 0.25, true, false},
    {MS_OP_POW, 2, -2, 0.25, false, true},
    {MS_OP_POW, 2, 3, 8, false, false}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunBinary(&fixture, cases[i].op,
        msVmFixtureNumericValue(&fixture, cases[i].lhs, cases[i].lhsIsInt),
        msVmFixtureNumericValue(&fixture, cases[i].rhs, cases[i].rhsIsInt));
    MS_ASSERT_EQ(MS_OK, fixture.status);
    MS_ASSERT_TRUE(fixture.result != NULL);
    if (fixture.result != NULL) {
      MS_ASSERT_EQ(MS_TYPE_FLOAT, msObjectTag(fixture.result));
      double actual = msFloatValue(fixture.result);
      if (actual != cases[i].expected) {
        printf("numeric case %zu (op %d): expected %.17g, actual %.17g\n",
            i, (int)cases[i].op, cases[i].expected, actual);
      }
      MS_ASSERT_TRUE(actual == cases[i].expected);
    }
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(1, fixture.co.stackTop);
    msVmFixtureDestroy(&fixture);
  }
}

struct MsVmIntErrorCase {
  MsOpCode op;
  int64_t lhs;
  int64_t rhs;
  const char* message;
};

static void msVmFixtureAssertError(struct MsVmFixture* fixture, const char* message) {
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture->status);
  MS_ASSERT_TRUE(fixture->result == NULL);
  const char* actual = msStateErrorMessage(fixture->L);
  MS_ASSERT_TRUE(actual != NULL);
  if (actual != NULL) {
    MS_ASSERT_TRUE(strstr(actual, message) != NULL);
    MS_ASSERT_TRUE(strstr(actual, "test.ms:17") != NULL);
  }
  MS_ASSERT_EQ(0, fixture->co.frameCount);
  MS_ASSERT_EQ(0, fixture->co.stackTop);
  msStateClearError(fixture->L);
}

MS_TEST(vm, arithmeticErrorsRestoreCoroutine) {
  const struct MsVmIntErrorCase cases[] = {
    {MS_OP_DIV, 1, 0, "division by zero"},
    {MS_OP_FLOORDIV, 1, 0, "division by zero"},
    {MS_OP_MOD, 1, 0, "division by zero"},
    {MS_OP_SHL, 1, -1, "invalid shift count"},
    {MS_OP_SHR, 1, 64, "invalid shift count"},
    {MS_OP_ADD, INT64_MAX, 1, "integer overflow"},
    {MS_OP_ADD, INT64_MIN, -1, "integer overflow"},
    {MS_OP_SUB, INT64_MIN, 1, "integer overflow"},
    {MS_OP_MUL, INT64_MIN, -1, "integer overflow"},
    {MS_OP_FLOORDIV, INT64_MIN, -1, "integer overflow"},
    {MS_OP_MOD, INT64_MIN, -1, "integer overflow"},
    {MS_OP_POW, 2, 63, "integer overflow"},
    {MS_OP_SHL, 1, 63, "integer overflow"},
    {MS_OP_SHL, -2, 63, "integer overflow"}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunBinary(&fixture, cases[i].op,
        msVmFixtureTestInt(&fixture, cases[i].lhs), msVmFixtureTestInt(&fixture, cases[i].rhs));
    msVmFixtureAssertError(&fixture, cases[i].message);
    msVmFixtureDestroy(&fixture);
  }

  const MsOpCode zeroDivisionOps[] = {MS_OP_DIV, MS_OP_FLOORDIV, MS_OP_MOD};
  const double zeroes[] = {0.0, -0.0};
  for (size_t opIndex = 0; opIndex < sizeof(zeroDivisionOps) / sizeof(zeroDivisionOps[0]); ++opIndex) {
    for (size_t zeroIndex = 0; zeroIndex < sizeof(zeroes) / sizeof(zeroes[0]); ++zeroIndex) {
      struct MsVmFixture fixture;
      msVmFixtureInit(&fixture);
      msVmFixtureRunBinary(&fixture, zeroDivisionOps[opIndex],
          msNewFloat(fixture.L, 1.0), msNewFloat(fixture.L, zeroes[zeroIndex]));
      msVmFixtureAssertError(&fixture, "division by zero");
      msVmFixtureDestroy(&fixture);
    }
  }

  const MsOpCode bitwiseOps[] = {MS_OP_BITAND, MS_OP_BITOR, MS_OP_BITXOR, MS_OP_SHL, MS_OP_SHR};
  for (size_t i = 0; i < sizeof(bitwiseOps) / sizeof(bitwiseOps[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunBinary(&fixture, bitwiseOps[i], msNewFloat(fixture.L, 1.0), msNewFloat(fixture.L, 1.0));
    msVmFixtureAssertError(&fixture, "TypeError:");
    msVmFixtureDestroy(&fixture);
  }
}

struct MsVmIntUnaryCase {
  MsOpCode op;
  int64_t value;
  int64_t expected;
};

MS_TEST(vm, unaryArithmeticAndTruthiness) {
  const struct MsVmIntUnaryCase cases[] = {
    {MS_OP_NEG, 42, -42},
    {MS_OP_NEG, -42, 42},
    {MS_OP_NEG, INT64_MAX, INT64_MIN + 1},
    {MS_OP_NEG, INT64_MIN + 1, INT64_MAX},
    {MS_OP_INVERT, 0, -1},
    {MS_OP_INVERT, -1, 0}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunUnary(&fixture, cases[i].op, msVmFixtureTestInt(&fixture, cases[i].value));
    MS_ASSERT_EQ(MS_OK, fixture.status);
    MS_ASSERT_TRUE(fixture.result != NULL);
    if (fixture.result != NULL) {
      MS_ASSERT_EQ(MS_TYPE_INT, msObjectTag(fixture.result));
      MS_ASSERT_EQ(cases[i].expected, msIntWordValue(fixture.result));
    }
    MS_ASSERT_EQ(1, fixture.co.stackTop);
    msVmFixtureDestroy(&fixture);
  }

  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  msVmFixtureRunUnary(&fixture, MS_OP_NEG, msNewFloat(fixture.L, 2.5));
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result != NULL);
  if (fixture.result != NULL) {
    MS_ASSERT_EQ(MS_TYPE_FLOAT, msObjectTag(fixture.result));
    MS_ASSERT_TRUE(msFloatValue(fixture.result) == -2.5);
  }
  msVmFixtureDestroy(&fixture);

  struct MsObject* notValues[4];
  const bool notExpected[] = {true, true, false, false};
  for (int i = 0; i < 4; ++i) {
    msVmFixtureInit(&fixture);
    notValues[0] = msNewNil(fixture.L);
    notValues[1] = (struct MsObject*)msStringFromCStr(&fixture.L->heap, "");
    notValues[2] = (struct MsObject*)msStringFromCStr(&fixture.L->heap, "value");
    notValues[3] = msVmFixtureTestInt(&fixture, 1);
    msVmFixtureRunUnary(&fixture, MS_OP_NOT, notValues[i]);
    MS_ASSERT_EQ(MS_OK, fixture.status);
    MS_ASSERT_TRUE(fixture.result != NULL);
    if (fixture.result != NULL) {
      MS_ASSERT_EQ(MS_TYPE_BOOL, msObjectTag(fixture.result));
      MS_ASSERT_TRUE(msBoolValue(fixture.result) == notExpected[i]);
    }
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(1, fixture.co.stackTop);
    msVmFixtureDestroy(&fixture);
  }

  msVmFixtureInit(&fixture);
  msVmFixtureRunUnary(&fixture, MS_OP_NEG, msVmFixtureTestInt(&fixture, INT64_MIN));
  msVmFixtureAssertError(&fixture, "integer overflow");
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  msVmFixtureRunUnary(&fixture, MS_OP_INVERT, msNewFloat(fixture.L, 1.0));
  msVmFixtureAssertError(&fixture, "TypeError:");
  msVmFixtureDestroy(&fixture);
}

struct MsVmComparisonCase {
  MsOpCode op;
  int64_t lhsInt;
  double lhsFloat;
  bool lhsIsInt;
  int64_t rhsInt;
  double rhsFloat;
  bool rhsIsInt;
  bool expected;
};

static struct MsObject* msVmFixtureComparisonValue(struct MsVmFixture* fixture,
    int64_t intValue, double floatValue, bool isInt) {
  return isInt ? msVmFixtureTestInt(fixture, intValue) : msNewFloat(fixture->L, floatValue);
}

static void msVmFixtureAssertBool(struct MsVmFixture* fixture, bool expected) {
  MS_ASSERT_EQ(MS_OK, fixture->status);
  MS_ASSERT_TRUE(fixture->result != NULL);
  if (fixture->result != NULL) {
    MS_ASSERT_EQ(MS_TYPE_BOOL, msObjectTag(fixture->result));
    MS_ASSERT_TRUE(msBoolValue(fixture->result) == expected);
  }
  MS_ASSERT_EQ(0, fixture->co.frameCount);
  MS_ASSERT_EQ(1, fixture->co.stackTop);
}

static void msVmTestComparisonCase(const struct MsVmComparisonCase* testCase) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  msVmFixtureRunBinary(&fixture, testCase->op,
      msVmFixtureComparisonValue(&fixture, testCase->lhsInt, testCase->lhsFloat, testCase->lhsIsInt),
      msVmFixtureComparisonValue(&fixture, testCase->rhsInt, testCase->rhsFloat, testCase->rhsIsInt));
  msVmFixtureAssertBool(&fixture, testCase->expected);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, numericComparisonsAreExact) {
  const MsOpCode ops[] = {MS_OP_EQ, MS_OP_NE, MS_OP_LT, MS_OP_LE, MS_OP_GT, MS_OP_GE};
  const bool equalExpected[] = {true, false, false, true, false, true};
  const bool lessExpected[] = {false, true, true, true, false, false};
  for (int kind = 0; kind < 4; ++kind) {
    for (int relation = 0; relation < 2; ++relation) {
      for (size_t opIndex = 0; opIndex < sizeof(ops) / sizeof(ops[0]); ++opIndex) {
        double rhs = relation == 0 ? 2.0 : 3.0;
        struct MsVmComparisonCase testCase = {
          .op = ops[opIndex],
          .lhsInt = 2,
          .lhsFloat = 2.0,
          .lhsIsInt = kind == 0 || kind == 2,
          .rhsInt = (int64_t)rhs,
          .rhsFloat = rhs,
          .rhsIsInt = kind == 0 || kind == 3,
          .expected = relation == 0 ? equalExpected[opIndex] : lessExpected[opIndex]
        };
        msVmTestComparisonCase(&testCase);
      }
    }
  }

  const int64_t aboveDoubleExact = INT64_C(9007199254740993);
  const double nearestDouble = 9007199254740992.0;
  const struct MsVmComparisonCase cases[] = {
    {MS_OP_EQ, aboveDoubleExact, 0.0, true, 0, nearestDouble, false, false},
    {MS_OP_NE, aboveDoubleExact, 0.0, true, 0, nearestDouble, false, true},
    {MS_OP_LT, aboveDoubleExact, 0.0, true, 0, nearestDouble, false, false},
    {MS_OP_LE, aboveDoubleExact, 0.0, true, 0, nearestDouble, false, false},
    {MS_OP_GT, aboveDoubleExact, 0.0, true, 0, nearestDouble, false, true},
    {MS_OP_GE, aboveDoubleExact, 0.0, true, 0, nearestDouble, false, true},
    {MS_OP_EQ, 0, nearestDouble, false, aboveDoubleExact, 0.0, true, false},
    {MS_OP_NE, 0, nearestDouble, false, aboveDoubleExact, 0.0, true, true},
    {MS_OP_LT, 0, nearestDouble, false, aboveDoubleExact, 0.0, true, true},
    {MS_OP_LE, 0, nearestDouble, false, aboveDoubleExact, 0.0, true, true},
    {MS_OP_GT, 0, nearestDouble, false, aboveDoubleExact, 0.0, true, false},
    {MS_OP_GE, 0, nearestDouble, false, aboveDoubleExact, 0.0, true, false},
    {MS_OP_LT, INT64_C(9007199254740992), 0.0, true, 0, nearestDouble, false, false},
    {MS_OP_LE, INT64_C(9007199254740992), 0.0, true, 0, nearestDouble, false, true},
    {MS_OP_LT, -2, 0.0, true, 0, -1.5, false, true},
    {MS_OP_GT, -1, 0.0, true, 0, -1.5, false, true},
    {MS_OP_LT, 1, 0.0, true, 0, 1.5, false, true},
    {MS_OP_GT, 1, 0.0, true, 0, 0.5, false, true},
    {MS_OP_LT, 0, -1.5, false, -2, 0.0, true, false},
    {MS_OP_GT, 0, -1.5, false, -1, 0.0, true, false},
    {MS_OP_LT, 0, 1.5, false, 1, 0.0, true, false},
    {MS_OP_GT, 0, 0.5, false, 1, 0.0, true, false}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    msVmTestComparisonCase(&cases[i]);
  }
}

MS_TEST(vm, nanZeroAndBoundaryComparisons) {
  const struct MsVmComparisonCase cases[] = {
    {MS_OP_EQ, 0, NAN, false, 0, NAN, false, false},
    {MS_OP_NE, 0, NAN, false, 0, NAN, false, true},
    {MS_OP_LT, 0, NAN, false, 0, 0.0, false, false},
    {MS_OP_LE, 0, 0.0, false, 0, NAN, false, false},
    {MS_OP_GT, 0, NAN, false, 0, 0.0, false, false},
    {MS_OP_GE, 0, 0.0, false, 0, NAN, false, false},
    {MS_OP_EQ, 1, 0.0, true, 0, NAN, false, false},
    {MS_OP_NE, 0, NAN, false, 1, 0.0, true, true},
    {MS_OP_LT, 1, 0.0, true, 0, NAN, false, false},
    {MS_OP_EQ, 0, -0.0, false, 0, 0.0, false, true},
    {MS_OP_NE, 0, -0.0, false, 0, 0.0, false, false},
    {MS_OP_LE, 0, -0.0, false, 0, 0.0, false, true},
    {MS_OP_GE, 0, -0.0, false, 0, 0.0, false, true},
    {MS_OP_LT, INT64_MAX, 0.0, true, 0, INFINITY, false, true},
    {MS_OP_GT, INT64_MIN, 0.0, true, 0, -INFINITY, false, true},
    {MS_OP_GT, 0, INFINITY, false, INT64_MAX, 0.0, true, true},
    {MS_OP_LT, 0, -INFINITY, false, INT64_MIN, 0.0, true, true},
    {MS_OP_LT, INT64_MAX, 0.0, true, 0, 9223372036854775808.0, false, true},
    {MS_OP_GT, 0, 9223372036854775808.0, false, INT64_MAX, 0.0, true, true},
    {MS_OP_EQ, INT64_MIN, 0.0, true, 0, -9223372036854775808.0, false, true},
    {MS_OP_LE, INT64_MIN, 0.0, true, 0, -9223372036854775808.0, false, true},
    {MS_OP_GE, INT64_MIN, 0.0, true, 0, -9223372036854775808.0, false, true},
    {MS_OP_GT, INT64_MIN, 0.0, true, 0, -9223372036854777856.0, false, true},
    {MS_OP_LT, 0, -9223372036854777856.0, false, INT64_MIN, 0.0, true, true}
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    msVmTestComparisonCase(&cases[i]);
  }
}

static void msVmFixtureRunBinaryArgs(struct MsVmFixture* fixture, MsOpCode op,
    struct MsObject* lhs, struct MsObject* rhs) {
  fixture->builder.proto.paramCount = 2;
  fixture->builder.proto.localCount = 2;
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_LOCAL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_LOCAL, 1, 0));
  msProtoSetLine(&fixture->builder, 17);
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture->builder, op, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture->builder, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&fixture->builder, &fixture->proto));
  struct MsObject* args[] = {lhs, rhs};
  fixture->status = msCallProto(fixture->L, &fixture->co, fixture->proto, 2, args, &fixture->result);
}

MS_TEST(vm, stringComparisonsAndIdentity) {
  const MsOpCode ops[] = {MS_OP_LT, MS_OP_LE, MS_OP_GT, MS_OP_GE, MS_OP_NE, MS_OP_EQ};
  const bool expected[] = {true, true, false, false, true, false};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunBinary(&fixture, ops[i],
        (struct MsObject*)msStringFromCStr(&fixture.L->heap, "abc"),
        (struct MsObject*)msStringFromCStr(&fixture.L->heap, "abd"));
    msVmFixtureAssertBool(&fixture, expected[i]);
    msVmFixtureDestroy(&fixture);
  }

  const char* longString = "a-long-string-with-shared-contents-but-distinct-object-identities";
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  struct MsObject* lhs = (struct MsObject*)msStringFromCStr(&fixture.L->heap, longString);
  struct MsObject* rhs = (struct MsObject*)msStringFromCStr(&fixture.L->heap, longString);
  MS_ASSERT_TRUE(lhs != rhs);
  msVmFixtureRunBinaryArgs(&fixture, MS_OP_EQ, lhs, rhs);
  msVmFixtureAssertBool(&fixture, true);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  lhs = (struct MsObject*)msStringFromCStr(&fixture.L->heap, longString);
  rhs = (struct MsObject*)msStringFromCStr(&fixture.L->heap, longString);
  MS_ASSERT_TRUE(lhs != rhs);
  msVmFixtureRunBinaryArgs(&fixture, MS_OP_IS, lhs, rhs);
  msVmFixtureAssertBool(&fixture, false);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  msVmFixtureRunBinary(&fixture, MS_OP_IS, msNewNil(fixture.L), msNewNil(fixture.L));
  msVmFixtureAssertBool(&fixture, true);
  msVmFixtureDestroy(&fixture);
}

static void msVmFixtureRunChain(struct MsVmFixture* fixture,
    int64_t lhs, int64_t middle, int64_t rhs) {
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, lhs)));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, middle)));
  MS_ASSERT_EQ(2, msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, rhs)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture->builder, MS_OP_CMP_CHAIN, MS_OP_LT, 2));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 2));
  MS_ASSERT_EQ(4, msProtoEmitABC(&fixture->builder, MS_OP_LT, 0, 0));
  MS_ASSERT_EQ(5, msProtoEmitABC(&fixture->builder, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(6, fixture->builder.proto.codeLen);
  MS_ASSERT_TRUE(msOpDecodeOp(fixture->builder.proto.code[5]) == MS_OP_RETURN);
  msVmFixtureRun(fixture);
}

MS_TEST(vm, comparisonChainsJumpToSharedReturn) {
  const int64_t values[][3] = {{1, 2, 3}, {1, 3, 2}, {2, 1, 0}};
  const bool expected[] = {true, false, false};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunChain(&fixture, values[i][0], values[i][1], values[i][2]);
    msVmFixtureAssertBool(&fixture, expected[i]);
    msVmFixtureDestroy(&fixture);
  }
}

static void msVmFixtureRunChainDivisionTail(struct MsVmFixture* fixture, int64_t lhs, int64_t middle) {
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, lhs)));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, middle)));
  int oneIndex = msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, 1));
  int zeroIndex = msProtoAddConst(&fixture->builder, msVmFixtureTestInt(fixture, 0));
  MS_ASSERT_TRUE(oneIndex >= 0);
  MS_ASSERT_TRUE(zeroIndex >= 0);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture->builder, MS_OP_CMP_CHAIN, MS_OP_LT, 4));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, (uint16_t)oneIndex));
  MS_ASSERT_EQ(4, msProtoEmitABC(&fixture->builder, MS_OP_LOAD_CONST, 0, (uint16_t)zeroIndex));
  msProtoSetLine(&fixture->builder, 17);
  MS_ASSERT_EQ(5, msProtoEmitABC(&fixture->builder, MS_OP_DIV, 0, 0));
  MS_ASSERT_EQ(6, msProtoEmitABC(&fixture->builder, MS_OP_LT, 0, 0));
  MS_ASSERT_EQ(7, msProtoEmitABC(&fixture->builder, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(8, fixture->builder.proto.codeLen);
  MS_ASSERT_TRUE(msOpDecodeOp(fixture->builder.proto.code[7]) == MS_OP_RETURN);
  msVmFixtureRun(fixture);
}

MS_TEST(vm, comparisonChainShortCircuitsErrorTail) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  msVmFixtureRunChainDivisionTail(&fixture, 2, 1);
  msVmFixtureAssertBool(&fixture, false);
  MS_ASSERT_TRUE(msStateErrorMessage(fixture.L) == NULL);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  msVmFixtureRunChainDivisionTail(&fixture, 1, 2);
  msVmFixtureAssertError(&fixture, "division by zero");
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, invalidComparisonChainOpcodeRestoresCoroutine) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  msProtoSetLine(&fixture.builder, 23);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_CMP_CHAIN, MS_OP_ADD, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "invalid comparison chain opcode") != NULL);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "test.ms:23") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);

  msStateClearError(fixture.L);
  struct MsProto* recovery = msVmTestRawProto();
  MS_ASSERT_TRUE(recovery != NULL);
  fixture.status = msCallProto(fixture.L, &fixture.co, recovery, 0, NULL, &fixture.result);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result == msNewNil(fixture.L));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  msProtoFree(recovery);
  msVmFixtureDestroy(&fixture);
}

static void msVmTestCallProtoBuilderInit(MsState* L, struct MsProtoBuilder* builder, int stackSize) {
  msProtoBuilderInit(builder);
  builder->proto.name = msStringFromCStr(&L->heap, "<call-test>");
  builder->proto.sourceFile = msStringFromCStr(&L->heap, "test.ms");
  builder->proto.stackSize = stackSize;
}

MS_TEST(vm, scriptFunctionCallReturns42) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  struct MsProtoBuilder addBuilder;
  msVmTestCallProtoBuilderInit(fixture.L, &addBuilder, 2);
  addBuilder.proto.paramCount = 2;
  addBuilder.proto.localCount = 2;
  MS_ASSERT_EQ(0, msProtoEmitABC(&addBuilder, MS_OP_LOAD_LOCAL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&addBuilder, MS_OP_LOAD_LOCAL, 1, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&addBuilder, MS_OP_ADD, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&addBuilder, MS_OP_RETURN, 0, 0));
  struct MsProto* addProto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&addBuilder, &addProto));
  struct MsObject* addFunction = msNewFunction(fixture.L, addProto);
  MS_ASSERT_TRUE(addFunction != NULL);

  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, addFunction));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 20)));
  MS_ASSERT_EQ(2, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 22)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 2));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 2, 0));
  MS_ASSERT_EQ(4, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result != NULL);
  if (fixture.result != NULL) {
    MS_ASSERT_EQ(42, msIntWordValue(fixture.result));
  }
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  msProtoFree(addProto);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, nestedScriptCallsGrowFrameArray) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  enum { levelCount = 9 };
  struct MsProtoBuilder builders[levelCount];
  struct MsProto* protos[levelCount];
  for (int i = 0; i < levelCount; ++i) {
    msVmTestCallProtoBuilderInit(fixture.L, &builders[i], 2);
    protos[i] = NULL;
  }

  struct MsObject* nextFunction = NULL;
  for (int i = levelCount - 1; i >= 0; --i) {
    if (i == levelCount - 1) {
      MS_ASSERT_EQ(0, msProtoAddConst(&builders[i], msVmFixtureTestInt(&fixture, 42)));
      MS_ASSERT_EQ(0, msProtoEmitABC(&builders[i], MS_OP_LOAD_CONST, 0, 0));
    } else {
      MS_ASSERT_EQ(0, msProtoAddConst(&builders[i], nextFunction));
      MS_ASSERT_EQ(0, msProtoEmitABC(&builders[i], MS_OP_LOAD_CONST, 0, 0));
      MS_ASSERT_EQ(1, msProtoEmitABC(&builders[i], MS_OP_CALL, 0, 0));
    }
    int returnPc = i == levelCount - 1 ? 1 : 2;
    MS_ASSERT_EQ(returnPc, msProtoEmitABC(&builders[i], MS_OP_RETURN, 0, 0));
    MS_ASSERT_EQ(MS_OK, msProtoBuild(&builders[i], &protos[i]));
    nextFunction = msNewFunction(fixture.L, protos[i]);
    MS_ASSERT_TRUE(nextFunction != NULL);
  }

  fixture.builder.proto.stackSize = 2;
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, nextFunction));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result != NULL);
  if (fixture.result != NULL) {
    MS_ASSERT_EQ(42, msIntWordValue(fixture.result));
  }
  MS_ASSERT_TRUE(fixture.co.framesCap >= levelCount + 1);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  for (int i = 0; i < levelCount; ++i) {
    msProtoFree(protos[i]);
  }
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, cFunctionCallReturnsAndPropagatesErrors) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  struct MsObject* function = msNewCFunction(fixture.L,
      msStringFromCStr(&fixture.L->heap, "addOne"), msVmTestCAddOne);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, function));
  MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 41)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 1, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result != NULL);
  if (fixture.result != NULL) {
    MS_ASSERT_EQ(42, msIntWordValue(fixture.result));
  }
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  function = msNewCFunction(fixture.L,
      msStringFromCStr(&fixture.L->heap, "addOne"), msVmTestCAddOne);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, function));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  msProtoSetLine(&fixture.builder, 23);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(fixture.L));
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:23: TypeError: expected one int"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, callValidationReportsLocationAndRestores) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  struct MsProtoBuilder identityBuilder;
  msVmTestCallProtoBuilderInit(fixture.L, &identityBuilder, 1);
  identityBuilder.proto.paramCount = 1;
  identityBuilder.proto.localCount = 1;
  MS_ASSERT_EQ(0, msProtoEmitABC(&identityBuilder, MS_OP_LOAD_LOCAL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&identityBuilder, MS_OP_RETURN, 0, 0));
  struct MsProto* identityProto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&identityBuilder, &identityProto));
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msNewFunction(fixture.L, identityProto)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  msProtoSetLine(&fixture.builder, 19);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:19: argument count mismatch"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);

  msStateClearError(fixture.L);
  struct MsObject* args[] = {msVmFixtureTestInt(&fixture, 1)};
  fixture.result = msNewNil(fixture.L);
  fixture.status = msCallProto(fixture.L, &fixture.co, fixture.proto, 1, args, &fixture.result);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "argument count mismatch"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msProtoFree(identityProto);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 99)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  msProtoSetLine(&fixture.builder, 29);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:29: object is not callable"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);

  msVmFixtureInit(&fixture);
  msProtoSetLine(&fixture.builder, 31);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 1, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:31: invalid call stack"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, callRejectsUnsupportedFunctionKinds) {
  for (int kind = 0; kind < 3; ++kind) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    struct MsProtoBuilder childBuilder;
    msVmTestCallProtoBuilderInit(fixture.L, &childBuilder, 1);
    childBuilder.proto.hasVarArgs = kind == 0;
    childBuilder.proto.hasKwArgs = kind == 1;
    childBuilder.proto.isAsync = kind == 2;
    MS_ASSERT_EQ(0, msProtoEmitABC(&childBuilder, MS_OP_LOAD_NIL, 0, 0));
    MS_ASSERT_EQ(1, msProtoEmitABC(&childBuilder, MS_OP_RETURN, 0, 0));
    struct MsProto* childProto = NULL;
    MS_ASSERT_EQ(MS_OK, msProtoBuild(&childBuilder, &childProto));
    MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msNewFunction(fixture.L, childProto)));
    MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
    msProtoSetLine(&fixture.builder, (uint32_t)(13 + kind));
    MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
    MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
    msVmFixtureRun(&fixture);

    const char* message = kind == 2 ? "async call requires the scheduler" : "variadic calls not supported";
    char expected[128];
    snprintf(expected, sizeof(expected), "test.ms:%d: %s", 13 + kind, message);
    MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
    MS_ASSERT_TRUE(fixture.result == NULL);
    MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), expected));
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(0, fixture.co.stackTop);
    msProtoFree(childProto);
    msVmFixtureDestroy(&fixture);
  }
}

MS_TEST(vm, recursiveCallOverflowRestoresCoroutine) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.localCount = 0;
  fixture.builder.proto.stackSize = 2;
  struct MsString* name = msStringFromCStr(&fixture.L->heap, "recur");
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, (struct MsObject*)name));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_GLOBAL, 0, 0));
  msProtoSetLine(&fixture.builder, 37);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&fixture.builder, &fixture.proto));
  struct MsObject* function = msNewFunction(fixture.L, fixture.proto);
  MS_ASSERT_TRUE(function != NULL);
  MS_ASSERT_EQ(MS_OK, msStateSetGlobal(fixture.L, name, function));

  fixture.status = msCallProto(fixture.L, &fixture.co, fixture.proto, 0, NULL, &fixture.result);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, msStateErrorResult(fixture.L));
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:37: call stack overflow"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, coroutineCanRunNewProtoAfterError) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  msVmFixtureRunBinary(&fixture, MS_OP_DIV,
      msVmFixtureTestInt(&fixture, 1), msVmFixtureTestInt(&fixture, 0));
  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(0, strcmp(msStateErrorMessage(fixture.L), "test.ms:17: division by zero"));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);

  msStateClearError(fixture.L);
  msProtoFree(fixture.proto);
  fixture.proto = NULL;
  msProtoBuilderDestroy(&fixture.builder);
  msVmTestCallProtoBuilderInit(fixture.L, &fixture.builder, 1);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 42)));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);
  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result != NULL);
  if (fixture.result != NULL) {
    MS_ASSERT_EQ(42, msIntWordValue(fixture.result));
  }
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

static struct MsObject* msVmTestCLocatedError(MsState* L, int64_t argc, struct MsObject** argv) {
  (void)argc;
  (void)argv;
  msRaiseRuntimeError(L, "test.ms:23: TypeError: located callback failure");
  return NULL;
}

MS_TEST(vm, cFunctionCallPreservesExistingLocation) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  struct MsObject* function = msNewCFunction(fixture.L,
      msStringFromCStr(&fixture.L->heap, "locatedError"), msVmTestCLocatedError);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, function));
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  msProtoSetLine(&fixture.builder, 19);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_CALL, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  const char* message = msStateErrorMessage(fixture.L);
  const char* expected = "test.ms:23: TypeError: located callback failure";
  if (strcmp(message, expected) != 0) {
    printf("actual error: %s\n", message);
  }
  MS_ASSERT_EQ(0, strcmp(message, expected));
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, allocationFailuresRestoreAndDoNotLeak) {
  bool sawResultAllocationOom = false;
  bool sawSuccess = false;
  for (int64_t failAfter = 0; failAfter <= 39; ++failAfter) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 4095)));
    MS_ASSERT_EQ(1, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 1)));
    MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
    MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 1));
    MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_ADD, 0, 0));
    MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
    MS_ASSERT_EQ(MS_OK, msProtoBuild(&fixture.builder, &fixture.proto));

    msMemSetFailAfter(failAfter);
    fixture.status = msCallProto(fixture.L, &fixture.co, fixture.proto, 0, NULL, &fixture.result);
    msMemSetFailAfter(-1);
    MS_ASSERT_TRUE(fixture.status == MS_OK || fixture.status == MS_ERROR_OOM);
    if (failAfter == 2) {
      MS_ASSERT_EQ(MS_ERROR_OOM, fixture.status);
      sawResultAllocationOom = fixture.status == MS_ERROR_OOM;
    }
    if (fixture.status == MS_OK) {
      sawSuccess = true;
      MS_ASSERT_TRUE(fixture.result != NULL);
      if (fixture.result != NULL) {
        MS_ASSERT_EQ(4096, msIntWordValue(fixture.result));
      }
      MS_ASSERT_EQ(0, fixture.co.frameCount);
      MS_ASSERT_EQ(1, fixture.co.stackTop);
    } else {
      MS_ASSERT_EQ(MS_ERROR_OOM, fixture.status);
      MS_ASSERT_TRUE(fixture.result == NULL);
      MS_ASSERT_EQ(0, fixture.co.frameCount);
      MS_ASSERT_EQ(0, fixture.co.stackTop);
      MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(fixture.L));
    }
    msVmFixtureDestroy(&fixture);
  }
  MS_ASSERT_TRUE(sawResultAllocationOom);
  MS_ASSERT_TRUE(sawSuccess);
}

MS_TEST(vm, targetedAllocationFailuresSetOom) {
  msMemResetStats();
  MsState* L = msNewState();
  MS_ASSERT_TRUE(L != NULL);
  if (L == NULL) {
    return;
  }
  struct MsProto* proto = msVmTestRawProto();
  struct MsString* globalName = msStringFromCStr(&L->heap, "oomGlobal");
  struct MsString* functionName = msStringFromCStr(&L->heap, "oomFunction");
  struct MsObject* value = msNewInt(L, INT64_MAX);
  struct MsObject* lhs = (struct MsObject*)msStringFromCStr(&L->heap, "left");
  struct MsObject* rhs = (struct MsObject*)msStringFromCStr(&L->heap, "right");
  MS_ASSERT_TRUE(proto != NULL);
  MS_ASSERT_TRUE(globalName != NULL);
  MS_ASSERT_TRUE(functionName != NULL);
  MS_ASSERT_TRUE(value != NULL);
  MS_ASSERT_TRUE(lhs != NULL);
  MS_ASSERT_TRUE(rhs != NULL);

  msMemSetFailAfter(0);
  MsResult status = msStateSetGlobal(L, globalName, value);
  msMemSetFailAfter(-1);
  MS_ASSERT_EQ(MS_ERROR_OOM, status);
  MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(L));
  MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);
  MS_ASSERT_EQ(0, L->globalsLen);
  MS_ASSERT_EQ(0, L->globalsCap);
  msStateClearError(L);

  msMemSetFailAfter(0);
  struct MsObject* function = msNewFunction(L, proto);
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(function == NULL);
  MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(L));
  MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);
  msStateClearError(L);

  msMemSetFailAfter(0);
  function = msNewCFunction(L, functionName, msVmTestCAddOne);
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(function == NULL);
  MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(L));
  MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);
  msStateClearError(L);

  for (int64_t failAfter = 0; failAfter <= 1; ++failAfter) {
    msMemSetFailAfter(failAfter);
    struct MsObject* concatenated = msObjectBinaryOp(L, MS_OP_ADD, lhs, rhs);
    msMemSetFailAfter(-1);
    MS_ASSERT_TRUE(concatenated == NULL);
    MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(L));
    MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);
    msStateClearError(L);
  }

  msMemSetFailAfter(0);
  struct MsObject* arithmetic = msObjectBinaryOp(L, MS_OP_BITAND, value, value);
  msMemSetFailAfter(-1);
  MS_ASSERT_TRUE(arithmetic == NULL);
  MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(L));
  MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);
  msStateClearError(L);

  for (int64_t failAfter = 0; failAfter <= 2; ++failAfter) {
    msMemSetFailAfter(failAfter);
    msRaiseRuntimeError(L, "formatted %s", "failure");
    msMemSetFailAfter(-1);
    MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(L));
    MS_ASSERT_TRUE(msStateErrorMessage(L) == NULL);
    msStateClearError(L);
  }

  msProtoFree(proto);
  msVmTestCloseState(L);
}

MS_TEST(vm, stateCreationOomEventuallySucceeds) {
  bool sawSuccess = false;
  for (int64_t failAfter = 0; failAfter <= 39; ++failAfter) {
    msMemResetStats();
    msMemSetFailAfter(failAfter);
    MsState* L = msNewState();
    msMemSetFailAfter(-1);
    if (L == NULL) {
      struct MsMemStats stats;
      msMemGetStats(&stats);
      MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
    } else {
      sawSuccess = true;
      msVmTestCloseState(L);
    }
  }
  MS_ASSERT_TRUE(sawSuccess);
}

MS_TEST(vm, locationFormattingOomReturnsOom) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder,
      (struct MsObject*)msStringFromCStr(&fixture.L->heap, "missing")));
  msProtoSetLine(&fixture.builder, 41);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_GLOBAL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&fixture.builder, &fixture.proto));

  msMemSetFailAfter(2);
  fixture.status = msCallProto(fixture.L, &fixture.co, fixture.proto, 0, NULL, &fixture.result);
  msMemSetFailAfter(-1);
  MS_ASSERT_EQ(MS_ERROR_OOM, fixture.status);
  MS_ASSERT_TRUE(fixture.result == NULL);
  MS_ASSERT_EQ(MS_ERROR_OOM, msStateErrorResult(fixture.L));
  MS_ASSERT_TRUE(msStateErrorMessage(fixture.L) == NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, deepEvaluationStackGrowsInSingleProto) {
  enum { valueCount = 65 };
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  fixture.builder.proto.stackSize = valueCount;
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder, msVmFixtureTestInt(&fixture, 1)));
  for (int i = 0; i < valueCount; ++i) {
    MS_ASSERT_EQ(i, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_CONST, 0, 0));
  }
  for (int i = 0; i < valueCount - 1; ++i) {
    MS_ASSERT_EQ(valueCount + i, msProtoEmitABC(&fixture.builder, MS_OP_ADD, 0, 0));
  }
  MS_ASSERT_EQ(2 * valueCount - 1, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_OK, fixture.status);
  MS_ASSERT_TRUE(fixture.result != NULL);
  if (fixture.result != NULL) {
    MS_ASSERT_EQ(valueCount, msIntWordValue(fixture.result));
  }
  MS_ASSERT_TRUE(fixture.co.stackCap > 64);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(1, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, diagnosticUsesMatchingLineSegment) {
  struct MsVmFixture fixture;
  msVmFixtureInit(&fixture);
  MS_ASSERT_EQ(0, msProtoAddConst(&fixture.builder,
      (struct MsObject*)msStringFromCStr(&fixture.L->heap, "missing")));
  msProtoSetLine(&fixture.builder, 3);
  MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, MS_OP_NOP, 0, 0));
  msProtoSetLine(&fixture.builder, 11);
  MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_NOP, 0, 0));
  msProtoSetLine(&fixture.builder, 29);
  MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_GLOBAL, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
  msVmFixtureRun(&fixture);

  MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
  MS_ASSERT_EQ(3, fixture.proto->linesLen);
  MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "test.ms:29: undefined name 'missing'") != NULL);
  MS_ASSERT_EQ(0, fixture.co.frameCount);
  MS_ASSERT_EQ(0, fixture.co.stackTop);
  msVmFixtureDestroy(&fixture);
}

MS_TEST(vm, booleanIdentityUsesSingletons) {
  const bool values[][2] = {{true, true}, {false, false}, {true, false}};
  const bool expected[] = {true, true, false};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    msVmFixtureRunBinary(&fixture, MS_OP_IS,
        msNewBool(fixture.L, values[i][0]), msNewBool(fixture.L, values[i][1]));
    msVmFixtureAssertBool(&fixture, expected[i]);
    msVmFixtureDestroy(&fixture);
  }
}

MS_TEST(vm, instructionPointerOutsideCodeRestores) {
  const int32_t offsets[] = {1, -2};
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    MS_ASSERT_EQ(0, msProtoEmitSAx(&fixture.builder, MS_OP_JUMP, offsets[i]));
    msVmFixtureRun(&fixture);
    MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
    MS_ASSERT_TRUE(fixture.result == NULL);
    MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "instruction pointer out of bounds") != NULL);
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(0, fixture.co.stackTop);
    msVmFixtureDestroy(&fixture);
  }
}

#ifdef NDEBUG
MS_TEST(vm, unsupportedOpcodeRestores) {
  const MsOpCode ops[] = {MS_OP_BUILD_LIST, MS_OP_MAKE_FUNCTION, MS_OP_GET_ITER};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
    struct MsVmFixture fixture;
    msVmFixtureInit(&fixture);
    MS_ASSERT_EQ(0, msProtoEmitABC(&fixture.builder, ops[i], 0, 0));
    MS_ASSERT_EQ(1, msProtoEmitABC(&fixture.builder, MS_OP_LOAD_NIL, 0, 0));
    MS_ASSERT_EQ(2, msProtoEmitABC(&fixture.builder, MS_OP_RETURN, 0, 0));
    msVmFixtureRun(&fixture);

    MS_ASSERT_EQ(MS_ERROR_RUNTIME, fixture.status);
    MS_ASSERT_TRUE(fixture.result == NULL);
    MS_ASSERT_TRUE(strstr(msStateErrorMessage(fixture.L), "opcode not implemented") != NULL);
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(0, fixture.co.stackTop);

    msStateClearError(fixture.L);
    struct MsProto* recovery = msVmTestRawProto();
    MS_ASSERT_TRUE(recovery != NULL);
    fixture.status = msCallProto(fixture.L, &fixture.co, recovery, 0, NULL, &fixture.result);
    MS_ASSERT_EQ(MS_OK, fixture.status);
    MS_ASSERT_TRUE(fixture.result == msNewNil(fixture.L));
    MS_ASSERT_EQ(0, fixture.co.frameCount);
    MS_ASSERT_EQ(1, fixture.co.stackTop);
    msProtoFree(recovery);
    msVmFixtureDestroy(&fixture);
  }
}
#endif

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
  {"Vm.IntegerArithmeticAndShiftBoundaries", testvmintegerArithmeticAndShiftBoundaries},
  {"Vm.NumericArithmeticPromotesToFloat", testvmnumericArithmeticPromotesToFloat},
  {"Vm.ArithmeticErrorsRestoreCoroutine", testvmarithmeticErrorsRestoreCoroutine},
  {"Vm.UnaryArithmeticAndTruthiness", testvmunaryArithmeticAndTruthiness},
  {"Vm.NumericComparisonsAreExact", testvmnumericComparisonsAreExact},
  {"Vm.NanZeroAndBoundaryComparisons", testvmnanZeroAndBoundaryComparisons},
  {"Vm.StringComparisonsAndIdentity", testvmstringComparisonsAndIdentity},
  {"Vm.ComparisonChainsJumpToSharedReturn", testvmcomparisonChainsJumpToSharedReturn},
  {"Vm.ComparisonChainShortCircuitsErrorTail", testvmcomparisonChainShortCircuitsErrorTail},
  {"Vm.InvalidComparisonChainOpcodeRestoresCoroutine", testvminvalidComparisonChainOpcodeRestoresCoroutine},
  {"Vm.ScriptFunctionCallReturns42", testvmscriptFunctionCallReturns42},
  {"Vm.NestedScriptCallsGrowFrameArray", testvmnestedScriptCallsGrowFrameArray},
  {"Vm.CFunctionCallReturnsAndPropagatesErrors", testvmcFunctionCallReturnsAndPropagatesErrors},
  {"Vm.CallValidationReportsLocationAndRestores", testvmcallValidationReportsLocationAndRestores},
  {"Vm.CallRejectsUnsupportedFunctionKinds", testvmcallRejectsUnsupportedFunctionKinds},
  {"Vm.RecursiveCallOverflowRestoresCoroutine", testvmrecursiveCallOverflowRestoresCoroutine},
  {"Vm.CoroutineCanRunNewProtoAfterError", testvmcoroutineCanRunNewProtoAfterError},
  {"Vm.CFunctionCallPreservesExistingLocation", testvmcFunctionCallPreservesExistingLocation},
  {"Vm.AllocationFailuresRestoreAndDoNotLeak", testvmallocationFailuresRestoreAndDoNotLeak},
  {"Vm.TargetedAllocationFailuresSetOom", testvmtargetedAllocationFailuresSetOom},
  {"Vm.StateCreationOomEventuallySucceeds", testvmstateCreationOomEventuallySucceeds},
  {"Vm.LocationFormattingOomReturnsOom", testvmlocationFormattingOomReturnsOom},
  {"Vm.DeepEvaluationStackGrowsInSingleProto", testvmdeepEvaluationStackGrowsInSingleProto},
  {"Vm.DiagnosticUsesMatchingLineSegment", testvmdiagnosticUsesMatchingLineSegment},
  {"Vm.BooleanIdentityUsesSingletons", testvmbooleanIdentityUsesSingletons},
  {"Vm.InstructionPointerOutsideCodeRestores", testvminstructionPointerOutsideCodeRestores},
#ifdef NDEBUG
  {"Vm.UnsupportedOpcodeRestores", testvmunsupportedOpcodeRestores},
#endif
};

MS_TEST_MAIN(msTestCases)
