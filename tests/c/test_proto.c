#include <stdbool.h>

#include "core/ms_memory.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "ms_test.h"

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

static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks},
  {"Proto.EmitReturnsSequentialPc", testprotoemitReturnsSequentialPc},
  {"Proto.EmitGrowthPreservesContents", testprotoemitGrowthPreservesContents},
  {"Proto.EmitSAxBoundariesOk", testprotoemitSAxBoundariesOk},
  {"Proto.EmitSAxOutOfRangeSticky", testprotoemitSAxOutOfRangeSticky},
  {"Proto.EmitOomSticky", testprotoemitOomSticky},
  {"Proto.LineRunEncoding", testprotolineRunEncoding}
};

MS_TEST_MAIN(msTestCases)
