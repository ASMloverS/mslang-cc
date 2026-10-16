#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core/ms_memory.h"
#include "object/ms_object.h"
#include "object/ms_string.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "ms_test.h"

// Dumps proto into buf (NUL-terminated) via a tmpfile.
static void msTestDumpProto(const struct MsProto* proto, char* buf, size_t bufSize) {
  FILE* out = tmpfile();
  MS_ASSERT_TRUE(out != NULL);
  msProtoDump(proto, out);
  rewind(out);
  size_t n = fread(buf, 1, bufSize - 1, out);
  buf[n] = '\0';
  fclose(out);
}

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

MS_TEST(proto, addConstValueDedup) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  // Values >= 4096 are outside the int cache: every msIntNew returns a
  // distinct object, so equal values below always come from distinct pointers.
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* kA2 = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* kB = (struct MsObject*)msIntNew(&heap, 5001);
  struct MsObject* kC = (struct MsObject*)msIntNew(&heap, 5002);
  MS_ASSERT_TRUE(kA != kA2);   // distinct pointers, equal value
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, kA));
  MS_ASSERT_EQ(1, msProtoAddConst(&b, kB));
  // Value dedup: kA2 equals kA by value, so it reuses slot 0.
  MS_ASSERT_EQ(0, msProtoAddConst(&b, kA2));
  MS_ASSERT_EQ(2, msProtoAddConst(&b, kC));
  MS_ASSERT_EQ(0, msProtoAddConst(&b, kA));   // identity fast path also dedups
  MS_ASSERT_EQ(3, b.proto.constsLen);
  MS_ASSERT_TRUE(b.proto.consts[0] == kA);
  MS_ASSERT_TRUE(b.proto.consts[1] == kB);
  MS_ASSERT_TRUE(b.proto.consts[2] == kC);
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, addConstKeepsIntAndFloatSeparate) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* iOne = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* fOne = (struct MsObject*)msFloatNew(&heap, 5000.0);
  MS_ASSERT_TRUE(iOne != NULL && fOne != NULL);
  // Same numeric value, different tags: must NOT share a slot (05-bytecode-proto).
  MS_ASSERT_EQ(0, msProtoAddConst(&b, iOne));
  MS_ASSERT_EQ(1, msProtoAddConst(&b, fOne));
  MS_ASSERT_EQ(2, b.proto.constsLen);
  // Same-tag re-adds still dedup, including a fresh equal-value pointer.
  struct MsObject* iOne2 = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* fOne2 = (struct MsObject*)msFloatNew(&heap, 5000.0);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, iOne2));
  MS_ASSERT_EQ(1, msProtoAddConst(&b, fOne2));
  MS_ASSERT_EQ(2, b.proto.constsLen);
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, addConstStringContentDedup) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  char buf[41];
  memset(buf, 'z', 40);
  buf[40] = '\0';
  struct MsString* s1 = msStringNew(&heap, buf, 40);   // long: not interned
  struct MsString* s2 = msStringNew(&heap, buf, 40);
  MS_ASSERT_TRUE(s1 != NULL && s2 != NULL);
  MS_ASSERT_TRUE(s1 != s2);                            // distinct pointers, equal content
  MS_ASSERT_EQ(0, msProtoAddConst(&b, (struct MsObject*)s1));
  MS_ASSERT_EQ(0, msProtoAddConst(&b, (struct MsObject*)s2));
  MS_ASSERT_EQ(1, b.proto.constsLen);
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, addConstCap65536) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  // Simulate a full pool by writing the public builder field directly (the
  // compiler writes b.proto metadata the same way). The cap check runs
  // BEFORE the dedup scan, so no backing array is needed and the test is
  // O(1) -- filling 65536 real entries through the API would be O(n^2).
  b.proto.constsLen = 65536;
  MS_ASSERT_EQ(-1, msProtoAddConst(&b, kA));
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);
  b.proto.constsLen = 0;   // restore before destroy
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
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
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  for (int i = 0; i < 16; ++i) {
    // Each value is unique and outside the int cache: 16 distinct objects.
    struct MsObject* k = (struct MsObject*)msIntNew(&heap, 5000 + i);
    MS_ASSERT_EQ(i, msProtoAddConst(&b, k));
  }
  MS_ASSERT_EQ(16, b.proto.constsLen);
  MS_ASSERT_EQ(16, b.constsCap);   // doubling from 8: 8 -> 16
  // The value-dedup scan must still hit entries written before the
  // reallocation, even when the match arrives as a fresh pointer.
  struct MsObject* k3 = (struct MsObject*)msIntNew(&heap, 5003);
  MS_ASSERT_TRUE(k3 != b.proto.consts[3]);
  MS_ASSERT_EQ(3, msProtoAddConst(&b, k3));
  MS_ASSERT_EQ(16, b.proto.constsLen);
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, addConstAndTryBlockOomSticky) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* kB = (struct MsObject*)msIntNew(&heap, 5001);
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msMemSetFailAfter(0);   // the consts grow fails
  MS_ASSERT_EQ(-1, msProtoAddConst(&b, kA));
  MS_ASSERT_EQ(MS_ERROR_OOM, b.error);
  msMemSetFailAfter(-1);
  // Sticky: even with a healthy allocator the builder stays failed.
  MS_ASSERT_EQ(-1, msProtoAddConst(&b, kB));
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
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, buildShrinksToExactLen) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  // Baseline: the byte-exact check below counts proto allocations only.
  const long long heapBytes = (long long)stats.currentBytes;
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, kA));
  msProtoSetLine(&b, 1);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_LOAD_CONST, 0, 0));
  MS_ASSERT_EQ(1, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  MS_ASSERT_TRUE(proto != NULL);
  MS_ASSERT_EQ(2, proto->codeLen);
  MS_ASSERT_EQ(1, proto->constsLen);
  MS_ASSERT_EQ(1, proto->linesLen);
  MS_ASSERT_EQ(0, proto->tryBlocksLen);
  MS_ASSERT_TRUE(proto->tryBlocks == NULL);
  // len == cap after shrink: live bytes equal the exact sizes, no slack.
  msMemGetStats(&stats);
  MS_ASSERT_EQ((long long)(sizeof(struct MsProto) + 2 * sizeof(uint32_t)
      + sizeof(struct MsObject*) + sizeof(struct MsLineEntry)),
      (long long)stats.currentBytes - heapBytes);
  // Builder left empty (pin 11); a second destroy is a safe no-op.
  MS_ASSERT_TRUE(b.proto.code == NULL);
  MS_ASSERT_EQ(0, b.proto.codeLen);
  MS_ASSERT_EQ(0, b.codeCap);
  MS_ASSERT_EQ(MS_OK, b.error);
  msProtoBuilderDestroy(&b);
  msProtoFree(proto);
  msHeapDestroy(&heap);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, buildZeroLengthArraysNull) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));   // no SetLine, no consts
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  MS_ASSERT_TRUE(proto->consts == NULL);
  MS_ASSERT_EQ(0, proto->constsLen);
  MS_ASSERT_TRUE(proto->tryBlocks == NULL);
  MS_ASSERT_TRUE(proto->lines == NULL);
  MS_ASSERT_EQ(0, (long long)msProtoLineAt(proto, 0));   // empty line table reads as 0
  msProtoFree(proto);
}

MS_TEST(proto, buildEmptyCodeFails) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoBuild(&b, &proto));
  MS_ASSERT_TRUE(proto == NULL);            // out untouched
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, b.error);   // recorded sticky
  msProtoBuilderDestroy(&b);                // caller still owns the builder
}

MS_TEST(proto, buildWithStickyErrorFails) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(-1, msProtoEmitSAx(&b, MS_OP_JUMP, 8388608));   // sticky SYNTAX
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_ERROR_SYNTAX, msProtoBuild(&b, &proto));
  MS_ASSERT_TRUE(proto == NULL);
  MS_ASSERT_TRUE(b.proto.code != NULL);   // ownership retained by the caller
  msProtoBuilderDestroy(&b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, lineAtBinarySearch) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msProtoSetLine(&b, 1);
  msProtoEmitABC(&b, MS_OP_NOP, 0, 0);   // pc 0 -> line 1
  msProtoEmitABC(&b, MS_OP_NOP, 0, 0);   // pc 1 -> line 1
  msProtoSetLine(&b, 5);
  msProtoEmitABC(&b, MS_OP_NOP, 0, 0);   // pc 2 -> line 5
  msProtoEmitABC(&b, MS_OP_NOP, 0, 0);   // pc 3 -> line 5
  msProtoEmitABC(&b, MS_OP_NOP, 0, 0);   // pc 4 -> line 5
  msProtoSetLine(&b, 9);
  msProtoEmitABC(&b, MS_OP_NOP, 0, 0);   // pc 5 -> line 9
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  MS_ASSERT_EQ(3, proto->linesLen);
  MS_ASSERT_EQ(1, (long long)msProtoLineAt(proto, 0));    // first entry
  MS_ASSERT_EQ(1, (long long)msProtoLineAt(proto, 1));    // inside first run
  MS_ASSERT_EQ(5, (long long)msProtoLineAt(proto, 2));    // middle entry
  MS_ASSERT_EQ(5, (long long)msProtoLineAt(proto, 4));    // end of middle run
  MS_ASSERT_EQ(9, (long long)msProtoLineAt(proto, 5));    // last entry
  MS_ASSERT_EQ(9, (long long)msProtoLineAt(proto, 100));  // beyond last run: clamp
  msProtoFree(proto);
}

MS_TEST(proto, dumpGolden) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, kA));
  msProtoSetLine(&b, 1);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_LOAD_CONST, 0, 0));
  msProtoSetLine(&b, 3);
  int jumpPc = msProtoEmitSAx(&b, MS_OP_JUMP, 0);
  MS_ASSERT_EQ(1, jumpPc);
  MS_ASSERT_EQ(2, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  MS_ASSERT_EQ(3, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));
  MS_ASSERT_EQ(MS_OK, msProtoPatchSAx(&b, jumpPc, 1));   // target = 1 + 1 + 1 = 3
  MS_ASSERT_EQ(MS_OK, msProtoAddTryBlock(&b, 0, 2, MS_PROTO_PC_NONE, 3));
  // Metadata fields are written directly by the compiler before build.
  b.proto.paramCount = 2;
  b.proto.localCount = 4;
  b.proto.stackSize = 8;
  b.proto.hasVarArgs = true;
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  char buf[4096];
  msTestDumpProto(proto, buf, sizeof(buf));
  const char* expected =
      "== proto ==\n"
      "name=-\n"
      "sourceFile=-\n"
      "params=2 locals=4 stack=8 flags=varargs\n"
      "code[4]:\n"
      "0000 MS_OP_LOAD_CONST A=0 Bx=0\n"
      "0001 MS_OP_JUMP sAx=1\n"
      "0002 MS_OP_NOP A=0 Bx=0\n"
      "0003 MS_OP_RETURN A=0 Bx=0\n"
      "consts[1]:\n"
      "#k0\n"
      "tryBlocks[1]:\n"
      "[0, 2) handler=- finally=3\n"
      "lines[2]:\n"
      "pc=0 line=1\n"
      "pc=1 line=3\n";
  if (strcmp(buf, expected) != 0) {
    printf("--- expected ---\n%s--- actual ---\n%s---\n", expected, buf);
    MS_ASSERT_TRUE(false);
  }
  msProtoFree(proto);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, buildFreeNoLeaks) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsObject* kA = (struct MsObject*)msIntNew(&heap, 5000);
  struct MsObject* kB = (struct MsObject*)msIntNew(&heap, 5001);
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msProtoAddConst(&b, kA);
  msProtoAddConst(&b, kB);
  msProtoSetLine(&b, 1);
  msProtoEmitABC(&b, MS_OP_LOAD_CONST, 0, 0);
  msProtoSetLine(&b, 2);
  msProtoEmitABC(&b, MS_OP_RETURN, 0, 0);
  msProtoAddTryBlock(&b, 0, 1, 1, MS_PROTO_PC_NONE);
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  msProtoFree(proto);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, buildOomRecoverableByDestroy) {
  msMemResetStats();
  struct MsProtoBuilder b;
  struct MsProto* proto = NULL;
  // Case A: the code-array shrink realloc fails mid-build.
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));   // code at cap 8; rest len 0
  msMemSetFailAfter(0);
  MS_ASSERT_EQ(MS_ERROR_OOM, msProtoBuild(&b, &proto));
  MS_ASSERT_TRUE(proto == NULL);
  msMemSetFailAfter(-1);
  msProtoBuilderDestroy(&b);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
  // Case B: the shrink succeeds, the final proto allocation fails.
  // msRealloc on a live block consumes the failAfter budget exactly once,
  // same as msAlloc, so failAfter(1) lets the shrink pass and fails the alloc.
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));
  msMemSetFailAfter(1);
  MS_ASSERT_EQ(MS_ERROR_OOM, msProtoBuild(&b, &proto));
  MS_ASSERT_TRUE(proto == NULL);
  msMemSetFailAfter(-1);
  msProtoBuilderDestroy(&b);
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}

MS_TEST(proto, lineAtClampsBeforeFirstEntry) {
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));   // pc 0 records no line entry
  msProtoSetLine(&b, 7);
  MS_ASSERT_EQ(1, msProtoEmitABC(&b, MS_OP_NOP, 0, 0));   // first entry: pc 1 -> line 7
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  MS_ASSERT_EQ(1, proto->linesLen);
  MS_ASSERT_EQ(7, (long long)msProtoLineAt(proto, 0));   // clamped to the first run
  MS_ASSERT_EQ(7, (long long)msProtoLineAt(proto, 1));
  msProtoFree(proto);
}

MS_TEST(proto, dumpFlagsAndNamedProto) {
  msProtoFree(NULL);   // no-op, must not crash
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoEmitABC(&b, MS_OP_RETURN, 0, 0));
  b.proto.isAsync = true;
  b.proto.hasVarArgs = true;
  b.proto.hasKwArgs = true;
  // Non-NULL stand-ins, never dereferenced: dump prints fixed placeholders.
  char fakeName;
  char fakeFile;
  b.proto.name = (struct MsString*)&fakeName;
  b.proto.sourceFile = (struct MsString*)&fakeFile;
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  char buf[4096];
  msTestDumpProto(proto, buf, sizeof(buf));
  const char* expected =
      "== proto ==\n"
      "name=#name\n"
      "sourceFile=#file\n"
      "params=0 locals=0 stack=0 flags=async,varargs,kwargs\n"
      "code[1]:\n"
      "0000 MS_OP_RETURN A=0 Bx=0\n"
      "consts[0]:\n"
      "tryBlocks[0]:\n"
      "lines[0]:\n";
  if (strcmp(buf, expected) != 0) {
    printf("--- expected ---\n%s--- actual ---\n%s---\n", expected, buf);
    MS_ASSERT_TRUE(false);
  }
  msProtoFree(proto);
}

MS_TEST(proto, addConstPreservesSignedZero) {
  msMemResetStats();
  struct MsHeap heap;
  MS_ASSERT_EQ(MS_OK, msHeapInit(&heap));
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  struct MsFloat* positive = msFloatNew(&heap, 0.0);
  struct MsFloat* negative = msFloatNew(&heap, -0.0);
  MS_ASSERT_TRUE(positive != NULL && negative != NULL);
  int first = msProtoAddConst(&b, (struct MsObject*)positive);
  int second = msProtoAddConst(&b, (struct MsObject*)negative);
  MS_ASSERT_EQ(0, first);
  MS_ASSERT_EQ(1, second);
  MS_ASSERT_EQ(2, b.proto.constsLen);
  if (second == 1 && b.proto.constsLen == 2) {
    MS_ASSERT_TRUE(!signbit(msFloatValue(b.proto.consts[0])));
    MS_ASSERT_TRUE(signbit(msFloatValue(b.proto.consts[1])));
  }
  struct MsFloat* negativeAgain = msFloatNew(&heap, -0.0);
  MS_ASSERT_TRUE(negativeAgain != NULL);
  MS_ASSERT_EQ(1, msProtoAddConst(&b, (struct MsObject*)negativeAgain));
  MS_ASSERT_EQ(2, b.proto.constsLen);
  msProtoBuilderDestroy(&b);
  msHeapDestroy(&heap);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
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
  {"Proto.AddConstValueDedup", testprotoaddConstValueDedup},
  {"Proto.AddConstKeepsIntAndFloatSeparate", testprotoaddConstKeepsIntAndFloatSeparate},
  {"Proto.AddConstPreservesSignedZero", testprotoaddConstPreservesSignedZero},
  {"Proto.AddConstStringContentDedup", testprotoaddConstStringContentDedup},
  {"Proto.AddConstCap65536", testprotoaddConstCap65536},
  {"Proto.AddTryBlockFieldsPreserved", testprotoaddTryBlockFieldsPreserved},
  {"Proto.AddConstGrowthAndDedupAfterRegrow", testprotoaddConstGrowthAndDedupAfterRegrow},
  {"Proto.AddConstAndTryBlockOomSticky", testprotoaddConstAndTryBlockOomSticky},
  {"Proto.BuildShrinksToExactLen", testprotobuildShrinksToExactLen},
  {"Proto.BuildZeroLengthArraysNull", testprotobuildZeroLengthArraysNull},
  {"Proto.BuildEmptyCodeFails", testprotobuildEmptyCodeFails},
  {"Proto.BuildWithStickyErrorFails", testprotobuildWithStickyErrorFails},
  {"Proto.LineAtBinarySearch", testprotolineAtBinarySearch},
  {"Proto.DumpGolden", testprotodumpGolden},
  {"Proto.BuildFreeNoLeaks", testprotobuildFreeNoLeaks},
  {"Proto.BuildOomRecoverableByDestroy", testprotobuildOomRecoverableByDestroy},
  {"Proto.LineAtClampsBeforeFirstEntry", testprotolineAtClampsBeforeFirstEntry},
  {"Proto.DumpFlagsAndNamedProto", testprotodumpFlagsAndNamedProto}
};

MS_TEST_MAIN(msTestCases)
