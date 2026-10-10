#ifndef MSLANG_SRC_VM_MS_OPCODE_H_
#define MSLANG_SRC_VM_MS_OPCODE_H_

#include <stdint.h>

#include "core/ms_common.h"

typedef enum {
  MS_OPFMT_ABC,    // opcode(8) | A(8) | Bx(16)
  MS_OPFMT_SAX     // opcode(8) | sAx(24, signed two's complement)
} MsOpFormat;

typedef enum {
  // Constants and moves (v0.1)
  MS_OP_LOAD_CONST, MS_OP_LOAD_NIL, MS_OP_LOAD_TRUE, MS_OP_LOAD_FALSE, MS_OP_MOVE,
  // Locals and upvalues (v0.1)
  MS_OP_LOAD_LOCAL, MS_OP_STORE_LOCAL, MS_OP_LOAD_UPVAL, MS_OP_STORE_UPVAL, MS_OP_CLOSE_UPVALS,
  // Globals and modules (LOAD/STORE_GLOBAL: v0.1; IMPORT*: v0.2)
  MS_OP_LOAD_GLOBAL, MS_OP_STORE_GLOBAL, MS_OP_IMPORT, MS_OP_IMPORT_FROM,
  // Arithmetic (v0.1)
  MS_OP_ADD, MS_OP_SUB, MS_OP_MUL, MS_OP_DIV, MS_OP_FLOORDIV, MS_OP_MOD, MS_OP_POW,
  MS_OP_NEG, MS_OP_NOT,
  MS_OP_BITAND, MS_OP_BITOR, MS_OP_BITXOR, MS_OP_SHL, MS_OP_SHR, MS_OP_INVERT,
  // Comparison (v0.1)
  MS_OP_EQ, MS_OP_NE, MS_OP_LT, MS_OP_LE, MS_OP_GT, MS_OP_GE, MS_OP_IS, MS_OP_IN, MS_OP_CMP_CHAIN,
  // Jumps (v0.1)
  MS_OP_JUMP, MS_OP_JUMP_IF_FALSE, MS_OP_JUMP_IF_TRUE, MS_OP_JUMP_IF_NIL,
  // Containers (LIST/DICT/INDEX/SET_INDEX/DEL_INDEX/APPEND: v0.1; TUPLE/SET/SLICE: v0.2)
  MS_OP_BUILD_LIST, MS_OP_BUILD_TUPLE, MS_OP_BUILD_DICT, MS_OP_BUILD_SET,
  MS_OP_INDEX, MS_OP_SET_INDEX, MS_OP_DEL_INDEX, MS_OP_SLICE, MS_OP_APPEND,
  // Calls (v0.1)
  MS_OP_CALL, MS_OP_CALL_KW, MS_OP_TAIL_CALL, MS_OP_RETURN, MS_OP_LOAD_METHOD, MS_OP_CALL_METHOD,
  // Functions and classes (v0.1)
  MS_OP_MAKE_FUNCTION, MS_OP_MAKE_CLASS, MS_OP_MAKE_LAMBDA,
  // Attributes (v0.1)
  MS_OP_GET_ATTR, MS_OP_SET_ATTR, MS_OP_DEL_ATTR,
  // Iteration (v0.1)
  MS_OP_GET_ITER, MS_OP_ITER_NEXT, MS_OP_UNPACK,
  // Exceptions (v0.2)
  MS_OP_SETUP_TRY, MS_OP_POP_TRY, MS_OP_RAISE, MS_OP_RERAISE,
  // Concurrency (v0.3)
  MS_OP_SPAWN, MS_OP_AWAIT,
  MS_OP_CHAN_NEW, MS_OP_CHAN_SEND, MS_OP_CHAN_RECV, MS_OP_CHAN_TRY_RECV,
  MS_OP_SELECT_BEGIN, MS_OP_SELECT_ADD_RECV, MS_OP_SELECT_ADD_SEND, MS_OP_SELECT_EXEC,
  // Miscellaneous (v0.1)
  MS_OP_PRINT_EXPR, MS_OP_NOP,
  MS_OP_COUNT
} MsOpCode;

_Static_assert(MS_OP_COUNT <= 256, "opcode must fit in 8 bits");

// Instruction bit layout (host-endianness independent, pure shifts):
//   ABC:  bits [0:8) opcode, bits [8:16) A, bits [16:32) Bx
//   sAx:  bits [0:8) opcode, bits [8:32) sAx (signed 24-bit two's complement)
// Jump offsets are relative to the instruction AFTER the jump:
// target = pc + 1 + sAx.

static inline uint32_t msOpEncodeABC(MsOpCode op, uint8_t a, uint16_t bx) {
  return (uint32_t)op | ((uint32_t)a << 8) | ((uint32_t)bx << 16);
}

static inline uint32_t msOpEncodeSAx(MsOpCode op, int32_t sax) {
  MS_ASSERT(sax >= -8388608 && sax <= 8388607);
  return (uint32_t)op | (((uint32_t)sax & 0xFFFFFFu) << 8);
}

static inline MsOpCode msOpDecodeOp(uint32_t instr) {
  return (MsOpCode)(instr & 0xFFu);
}

static inline uint8_t msOpDecodeA(uint32_t instr) {
  return (uint8_t)((instr >> 8) & 0xFFu);
}

static inline uint16_t msOpDecodeBx(uint32_t instr) {
  return (uint16_t)(instr >> 16);
}

static inline int32_t msOpDecodeSAx(uint32_t instr) {
  uint32_t raw = instr >> 8;
  // Sign-extend from 24 bits without shifting into the sign bit (C11-safe).
  return (raw & 0x800000u) != 0 ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
}

// Returns the declared operand format of op. Table-driven; total order with
// the enum is verified by unit tests. Out-of-range op is a programming error.
MsOpFormat msOpFormatOf(MsOpCode op);

// Static name table for diagnostics, dumps and tests ("MS_OP_ADD" etc.).
const char* msOpName(MsOpCode op);

#endif  // MSLANG_SRC_VM_MS_OPCODE_H_
