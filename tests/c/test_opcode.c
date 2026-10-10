#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_common.h"
#include "vm/ms_opcode.h"
#include "ms_test.h"

MS_TEST(opcode, encodeDecodeABCRoundTrip) {
  static const uint8_t aValues[] = { 0, 1, 255 };
  static const uint16_t bxValues[] = { 0, 1, 65535 };
  for (int op = 0; op < MS_OP_COUNT; ++op) {
    for (size_t a = 0; a < MS_ARRAY_LEN(aValues); ++a) {
      for (size_t bx = 0; bx < MS_ARRAY_LEN(bxValues); ++bx) {
        uint32_t instr = msOpEncodeABC((MsOpCode)op, aValues[a], bxValues[bx]);
        MS_ASSERT_EQ(op, msOpDecodeOp(instr));
        MS_ASSERT_EQ(aValues[a], msOpDecodeA(instr));
        MS_ASSERT_EQ(bxValues[bx], msOpDecodeBx(instr));
      }
    }
  }
}

MS_TEST(opcode, encodeDecodeSAxRoundTrip) {
  // Includes the 24-bit two's-complement boundaries; sign extension must
  // round-trip the negative values exactly.
  static const int32_t saxValues[] = { 0, 1, -1, 123456, -123456, 8388607, -8388608 };
  for (int op = 0; op < MS_OP_COUNT; ++op) {
    for (size_t i = 0; i < MS_ARRAY_LEN(saxValues); ++i) {
      uint32_t instr = msOpEncodeSAx((MsOpCode)op, saxValues[i]);
      MS_ASSERT_EQ(op, msOpDecodeOp(instr));
      MS_ASSERT_EQ(saxValues[i], msOpDecodeSAx(instr));
    }
  }
}

MS_TEST(opcode, bitLayout) {
  // Field positions are pinned by masking, not by full-word comparison:
  // enum numeric values are internal and may change before v1.0.
  uint32_t abc = msOpEncodeABC(MS_OP_MOVE, 0x12, 0x3456);
  MS_ASSERT_EQ(MS_OP_MOVE, abc & 0xFFu);
  MS_ASSERT_EQ(0x12, (abc >> 8) & 0xFFu);
  MS_ASSERT_EQ(0x3456, abc >> 16);
  uint32_t sax = msOpEncodeSAx(MS_OP_JUMP, -1);
  MS_ASSERT_EQ(MS_OP_JUMP, sax & 0xFFu);
  MS_ASSERT_EQ(0xFFFFFFu, sax >> 8);
}

MS_TEST(opcode, subsetCounts) {
  // Opcodes NOT part of the v0.1 subset: v0.2 imports/containers/exceptions
  // and the ten v0.3 concurrency ops. Everything else is v0.1: 82 - 19 = 63.
  static const MsOpCode excluded[] = {
    MS_OP_IMPORT, MS_OP_IMPORT_FROM,
    MS_OP_BUILD_TUPLE, MS_OP_BUILD_SET, MS_OP_SLICE,
    MS_OP_SETUP_TRY, MS_OP_POP_TRY, MS_OP_RAISE, MS_OP_RERAISE,
    MS_OP_SPAWN, MS_OP_AWAIT,
    MS_OP_CHAN_NEW, MS_OP_CHAN_SEND, MS_OP_CHAN_RECV, MS_OP_CHAN_TRY_RECV,
    MS_OP_SELECT_BEGIN, MS_OP_SELECT_ADD_RECV, MS_OP_SELECT_ADD_SEND, MS_OP_SELECT_EXEC
  };
  MS_ASSERT_EQ(82, MS_OP_COUNT);
  MS_ASSERT_TRUE(MS_OP_COUNT <= 256);
  MS_ASSERT_EQ(19, (long long)MS_ARRAY_LEN(excluded));
  MS_ASSERT_EQ(63, MS_OP_COUNT - (long long)MS_ARRAY_LEN(excluded));
}

MS_TEST(opcode, nameTableCompleteAndDistinct) {
  // Table length == MS_OP_COUNT is enforced by the array declaration itself;
  // here every entry must have a non-NULL, correctly prefixed, unique name.
  for (int op = 0; op < MS_OP_COUNT; ++op) {
    const char* name = msOpName((MsOpCode)op);
    MS_ASSERT_TRUE(name != NULL);
    MS_ASSERT_TRUE(strncmp(name, "MS_OP_", 6) == 0);
    for (int other = op + 1; other < MS_OP_COUNT; ++other) {
      MS_ASSERT_TRUE(strcmp(name, msOpName((MsOpCode)other)) != 0);
    }
  }
  MS_ASSERT_EQ(0, strcmp(msOpName(MS_OP_LOAD_CONST), "MS_OP_LOAD_CONST"));
  MS_ASSERT_EQ(0, strcmp(msOpName(MS_OP_ITER_NEXT), "MS_OP_ITER_NEXT"));
  MS_ASSERT_EQ(0, strcmp(msOpName(MS_OP_SELECT_EXEC), "MS_OP_SELECT_EXEC"));
  MS_ASSERT_EQ(0, strcmp(msOpName(MS_OP_NOP), "MS_OP_NOP"));
}

MS_TEST(opcode, formatOfSaxOps) {
  // Exactly five ops are sAx format; everything else is ABC.
  for (int op = 0; op < MS_OP_COUNT; ++op) {
    bool expectSAx = op == MS_OP_JUMP || op == MS_OP_JUMP_IF_FALSE
        || op == MS_OP_JUMP_IF_TRUE || op == MS_OP_JUMP_IF_NIL
        || op == MS_OP_ITER_NEXT;
    MS_ASSERT_EQ(expectSAx ? MS_OPFMT_SAX : MS_OPFMT_ABC,
        msOpFormatOf((MsOpCode)op));
  }
}

static const MsTestCase msTestCases[] = {
  {"Opcode.EncodeDecodeABCRoundTrip", testopcodeencodeDecodeABCRoundTrip},
  {"Opcode.EncodeDecodeSAxRoundTrip", testopcodeencodeDecodeSAxRoundTrip},
  {"Opcode.BitLayout", testopcodebitLayout},
  {"Opcode.SubsetCounts", testopcodesubsetCounts},
  {"Opcode.NameTableCompleteAndDistinct", testopcodenameTableCompleteAndDistinct},
  {"Opcode.FormatOfSaxOps", testopcodeformatOfSaxOps}
};

MS_TEST_MAIN(msTestCases)
