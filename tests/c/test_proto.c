#include <stdbool.h>

#include "core/ms_memory.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "ms_test.h"

// Stand-ins for struct MsObject until task 06 lands; never dereferenced.
static char msTestFakeConstA;
static char msTestFakeConstB;
static char msTestFakeConstC;
static char msTestFakeConstPool[16];

MS_TEST(proto, builderInitZeroed) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_TRUE(b.proto.code == NULL);
  MS_ASSERT_TRUE(b.proto.consts == NULL);
  MS_ASSERT_TRUE(b.proto.tryBlocks == NULL);
  MS_ASSERT_TRUE(b.proto.lines == NULL);
  MS_ASSERT_EQ(0, b.proto.codeLen);
  MS_ASSERT_EQ(0, b.proto.constsLen);
  MS_ASSERT_EQ(0, b.proto.tryBlocksLen);
  MS_ASSERT_EQ(0, b.proto.linesLen);
  MS_ASSERT_EQ(0, b.proto.paramCount);
  MS_ASSERT_EQ(0, b.proto.localCount);
  MS_ASSERT_EQ(0, b.proto.stackSize);
  MS_ASSERT_TRUE(b.proto.name == NULL);
  MS_ASSERT_TRUE(b.proto.sourceFile == NULL);
  MS_ASSERT_TRUE(!b.proto.isAsync);
  MS_ASSERT_TRUE(!b.proto.hasVarArgs);
  MS_ASSERT_TRUE(!b.proto.hasKwArgs);
  MS_ASSERT_EQ(0, b.codeCap);
  MS_ASSERT_EQ(0, b.constsCap);
  MS_ASSERT_EQ(0, b.tryCap);
  MS_ASSERT_EQ(0, b.linesCap);
  MS_ASSERT_TRUE(!b.hasPendingLine);
  MS_ASSERT_EQ(MS_OK, b.error);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, builderDestroyEmptyNoLeaks) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msProtoBuilderDestroy(&b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.allocCount);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, emitReturnsSequentialPc) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_LOAD_NIL, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitSAx(&b, MS_OP_JUMP, 5));
  MS_ASSERT_EQ(2, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(3, b.proto.codeLen);
  MS_ASSERT_EQ(MS_OP_LOAD_NIL, msOpDecodeOp(b.proto.code[0]));
  MS_ASSERT_EQ(MS_OP_JUMP, msOpDecodeOp(b.proto.code[1]));
  MS_ASSERT_EQ(5, msOpDecodeSAx(b.proto.code[1]));
  MS_ASSERT_EQ(MS_OP_RETURN, msOpDecodeOp(b.proto.code[2]));
  MS_ASSERT_EQ(MS_OK, b.error);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, emitGrowthPreservesContents) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  for (int i = 0; i < 100; ++i) {
    MS_ASSERT_EQ(i, msProtoEmitABC(&b, MS_OP_LOAD_CONST, 0, (uint16_t)i));
  }
  MS_ASSERT_EQ(100, b.proto.codeLen);
  MS_ASSERT_EQ(128, b.codeCap);   // doubling from 8: 8 -> 16 -> 32 -> 64 -> 128
  for (int i = 0; i < 100; ++i) {
    MS_ASSERT_EQ(i, msOpDecodeBx(b.proto.code[i]));
  }
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, emitSAxBoundariesOk) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitSAx(&b, MS_OP_JUMP, 8388607));
  MS_ASSERT_EQ(1, msProtoEmitSAx(&b, MS_OP_JUMP, -8388608));
  MS_ASSERT_EQ(8388607, msOpDecodeSAx(b.proto.code[0]));
  MS_ASSERT_EQ(-8388608, msOpDecodeSAx(b.proto.code[1]));
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, emitSAxOutOfRangeSticky) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(-1, msProtoEmitSAx(&b, MS_OP_JUMP, 8388608));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);
  MS_ASSERT_EQ(0, b.proto.codeLen);   // nothing emitted
  // Later calls short-circuit on the sticky error; the first failure sticks.
  MS_ASSERT_EQ(-1, msProtoEmitSAx(&b, MS_OP_JUMP, -8388609));
  MS_ASSERT_EQ(-1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);
  MS_ASSERT_EQ(0, b.proto.codeLen);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, emitOomSticky) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msMemSetFailAfter(0);   // the very first allocation (code grow) fails
  MS_ASSERT_EQ(-1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  MS_ASSERT_EQ(0, b.proto.codeLen);
  msMemSetFailAfter(-1);
  // Sticky: even with a healthy allocator the builder stays failed.
  MS_ASSERT_EQ(-1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, lineRunEncoding) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msProtoSetLine(&b, 1);
  msProtoSetLine(&b, 1);   // collapse: overwritten before any emit
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(1, b.proto.linesLen);
  MS_ASSERT_EQ(0, (long long)b.proto.lines[0].pc);
  MS_ASSERT_EQ(1, (long long)b.proto.lines[0].line);
  msProtoSetLine(&b, 1);   // same line as the recorded run: no new entry
  MS_ASSERT_EQ(1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(1, b.proto.linesLen);
  msProtoSetLine(&b, 2);
  msProtoSetLine(&b, 3);   // intermediate pending line dropped (no emit between)
  MS_ASSERT_EQ(2, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(2, b.proto.linesLen);
  MS_ASSERT_EQ(2, (long long)b.proto.lines[1].pc);
  MS_ASSERT_EQ(3, (long long)b.proto.lines[1].line);
  // Emits without a pending line record nothing.
  MS_ASSERT_EQ(3, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(2, b.proto.linesLen);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, emitOomAfterGrowthDestroyNoLeaks) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  for (int i = 0; i < 16; ++i) {
    // Fills code exactly to capacity (8 -> 16), so the next emit must grow.
    MS_ASSERT_EQ(i, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  }
  msMemSetFailAfter(0);   // the next grow (code array re-alloc) fails
  MS_ASSERT_EQ(-1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  msMemSetFailAfter(-1);
  // Destroy must free the live arrays even with the sticky OOM set.
  msProtoBuilderDestroy(&b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, lineTableGrowthPreservesRuns) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  for (int i = 0; i < 20; ++i) {
    msProtoSetLine(&b, (uint32_t)(i + 1));
    MS_ASSERT_EQ(i, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  }
  MS_ASSERT_EQ(20, b.proto.linesLen);
  MS_ASSERT_EQ(32, b.linesCap);   // doubling from 8: 8 -> 16 -> 32
  MS_ASSERT_EQ(0, (long long)b.proto.lines[0].pc);
  MS_ASSERT_EQ(1, (long long)b.proto.lines[0].line);
  MS_ASSERT_EQ(19, (long long)b.proto.lines[19].pc);
  MS_ASSERT_EQ(20, (long long)b.proto.lines[19].line);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, patchSAxJump) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  int jumpPc = msProtoEmitSAx(&b, MS_OP_JUMP, 0);   // placeholder offset
  MS_ASSERT_EQ(0, jumpPc);
  MS_ASSERT_EQ(1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(2, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));
  // target = pc + 1 + sAx = 0 + 1 + 1 = 2 -> the RETURN.
  MS_ASSERT_EQ(MS_OK, msProtoPatchSAx(&b, jumpPc, 1));
  MS_ASSERT_EQ(MS_OP_JUMP, msOpDecodeOp(b.proto.code[0]));   // opcode preserved
  MS_ASSERT_EQ(1, msOpDecodeSAx(b.proto.code[0]));
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, patchRejectsAbcInstruction) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_LOAD_NIL, 0, 0));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoPatchSAx(&b, 0, 0));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);   // sticky (pin 5)
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, patchRejectsBadPcAndRange) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoPatchSAx(&b, 0, 0));   // empty code
  msProtoBuilderDestroy(&b);
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitSAx(&b, MS_OP_JUMP, 0));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoPatchSAx(&b, 5, 0));    // pc out of range
  msProtoBuilderDestroy(&b);
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitSAx(&b, MS_OP_JUMP, 0));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoPatchSAx(&b, -1, 0));   // negative pc
  msProtoBuilderDestroy(&b);
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitSAx(&b, MS_OP_JUMP, 0));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoPatchSAx(&b, 0, 8388608));   // sAx out of range
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, patchSAxLastInstruction) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  int jumpPc = msProtoEmitSAx(&b, MS_OP_JUMP, 0);   // placeholder as the last instruction
  MS_ASSERT_EQ(1, jumpPc);
  MS_ASSERT_EQ(jumpPc, b.proto.codeLen - 1);   // patching the final pc must be accepted
  // target = pc + 1 + sAx = 1 + 1 - 1 = 1 (self); only the decode matters here.
  MS_ASSERT_EQ(MS_OK, msProtoPatchSAx(&b, jumpPc, -1));
  MS_ASSERT_EQ(MS_OP_JUMP, msOpDecodeOp(b.proto.code[1]));
  MS_ASSERT_EQ(-1, msOpDecodeSAx(b.proto.code[1]));
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, patchStickyShortCircuit) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(-1, msProtoEmitSAx(&b, MS_OP_JUMP, 8388608));   // force sticky failure
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);
  // Patch returns the sticky error without validating anything.
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoPatchSAx(&b, 0, 0));
  MS_ASSERT_EQ(0, b.proto.codeLen);
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, patchSAxNegativeBoundary) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitSAx(&b, MS_OP_JUMP, 0));   // placeholder offset
  MS_ASSERT_EQ(MS_OK, msProtoPatchSAx(&b, 0, -8388608));   // min 24-bit sAx accepted
  MS_ASSERT_EQ(-8388608, msOpDecodeSAx(b.proto.code[0]));
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, addConstPointerDedup) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA));
  MS_ASSERT_EQ(1, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstB));
  MS_ASSERT_EQ(0, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA));   // dedup
  MS_ASSERT_EQ(2, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstC));
  MS_ASSERT_EQ(3, b.proto.constsLen);
  MS_ASSERT_TRUE(b.proto.consts[0] == (struct MsObject*)&msTestFakeConstA);
  MS_ASSERT_TRUE(b.proto.consts[1] == (struct MsObject*)&msTestFakeConstB);
  MS_ASSERT_TRUE(b.proto.consts[2] == (struct MsObject*)&msTestFakeConstC);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, addConstCap65536) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  // Simulate a full pool by writing the public builder field directly (the
  // compiler writes b.proto metadata the same way). The cap check runs
  // BEFORE the dedup scan, so no backing array is needed and the test is
  // O(1) -- filling 65536 real entries through the API would be O(n^2).
  b.proto.constsLen = 65536;
  MS_ASSERT_EQ(-1, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);
  b.proto.constsLen = 0;   // restore before destroy
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, addTryBlockFieldsPreserved) {
  MS_ASSERT_EQ(0xFFFFFFFFu, (long long)MS_PROTO_PC_NONE);   // UINT32_MAX sentinel
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(MS_OK, msProtoAddTryBlock(&b, 0, 10, 5, MS_PROTO_PC_NONE));
  MS_ASSERT_EQ(MS_OK, msProtoAddTryBlock(&b, 20, 30, MS_PROTO_PC_NONE, 25));
  MS_ASSERT_EQ(2, b.proto.tryBlocksLen);
  MS_ASSERT_EQ(0, (long long)b.proto.tryBlocks[0].pcStart);
  MS_ASSERT_EQ(10, (long long)b.proto.tryBlocks[0].pcEnd);
  MS_ASSERT_EQ(5, (long long)b.proto.tryBlocks[0].handlerPc);
  MS_ASSERT_EQ(0xFFFFFFFFu, (long long)b.proto.tryBlocks[0].finallyPc);
  MS_ASSERT_EQ(20, (long long)b.proto.tryBlocks[1].pcStart);
  MS_ASSERT_EQ(30, (long long)b.proto.tryBlocks[1].pcEnd);
  MS_ASSERT_EQ(0xFFFFFFFFu, (long long)b.proto.tryBlocks[1].handlerPc);
  MS_ASSERT_EQ(25, (long long)b.proto.tryBlocks[1].finallyPc);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, addConstGrowthAndDedupAfterRegrow) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  for (int i = 0; i < 16; ++i) {
    MS_ASSERT_EQ(i, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstPool[i]));
  }
  MS_ASSERT_EQ(16, b.proto.constsLen);
  MS_ASSERT_EQ(16, b.constsCap);   // doubling from 8: 8 -> 16
  // The dedup scan must still hit entries written before the reallocation.
  MS_ASSERT_EQ(3, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstPool[3]));
  MS_ASSERT_EQ(16, b.proto.constsLen);
  msProtoBuilderDestroy(&b);
}

MS_TEST(proto, addConstAndTryBlockOomSticky) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msMemSetFailAfter(0);   // the consts grow fails
  MS_ASSERT_EQ(-1, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  msMemSetFailAfter(-1);
  // Sticky: even with a healthy allocator the builder stays failed.
  MS_ASSERT_EQ(-1, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstB));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  msProtoBuilderDestroy(&b);
  msProtoBuilderInit(&b);
  msMemSetFailAfter(0);   // the tryBlocks grow fails
  MS_ASSERT_EQ(MS_ERROR_OOM,
      msProtoAddTryBlock(&b, 0, 1, MS_PROTO_PC_NONE, MS_PROTO_PC_NONE));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  msMemSetFailAfter(-1);
  MS_ASSERT_EQ(MS_ERROR_OOM,
      msProtoAddTryBlock(&b, 0, 1, MS_PROTO_PC_NONE, MS_PROTO_PC_NONE));
  msProtoBuilderDestroy(&b);
}

static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks},
  {"Proto.EmitReturnsSequentialPc", testprotoemitReturnsSequentialPc},
  {"Proto.EmitGrowthPreservesContents", testprotoemitGrowthPreservesContents},
  {"Proto.EmitSAxBoundariesOk", testprotoemitSAxBoundariesOk},
  {"Proto.EmitSAxOutOfRangeSticky", testprotoemitSAxOutOfRangeSticky},
  {"Proto.EmitOomSticky", testprotoemitOomSticky},
  {"Proto.LineRunEncoding", testprotolineRunEncoding},
  {"Proto.EmitOomAfterGrowthDestroyNoLeaks", testprotoemitOomAfterGrowthDestroyNoLeaks},
  {"Proto.LineTableGrowthPreservesRuns", testprotolineTableGrowthPreservesRuns},
  {"Proto.PatchSAxJump", testprotopatchSAxJump},
  {"Proto.PatchRejectsAbcInstruction", testprotopatchRejectsAbcInstruction},
  {"Proto.PatchRejectsBadPcAndRange", testprotopatchRejectsBadPcAndRange},
  {"Proto.PatchSAxLastInstruction", testprotopatchSAxLastInstruction},
  {"Proto.PatchStickyShortCircuit", testprotopatchStickyShortCircuit},
  {"Proto.PatchSAxNegativeBoundary", testprotopatchSAxNegativeBoundary},
  {"Proto.AddConstPointerDedup", testprotoaddConstPointerDedup},
  {"Proto.AddConstCap65536", testprotoaddConstCap65536},
  {"Proto.AddTryBlockFieldsPreserved", testprotoaddTryBlockFieldsPreserved},
  {"Proto.AddConstGrowthAndDedupAfterRegrow", testprotoaddConstGrowthAndDedupAfterRegrow},
  {"Proto.AddConstAndTryBlockOomSticky", testprotoaddConstAndTryBlockOomSticky}
};

MS_TEST_MAIN(msTestCases)
