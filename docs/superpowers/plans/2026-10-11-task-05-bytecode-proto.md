# Bytecode Format & MsProto (Task 05) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver the mslang stack-bytecode format and the `MsProto` function-prototype module (`src/vm/ms_opcode.{h,c}` and `src/vm/ms_proto.{h,c}`): the full 82-entry `MsOpCode` enum with v0.2/v0.3 placeholders, the two fixed 4-byte instruction encodings (ABC / sAx) with `static inline` encode/decode, the opcode metadata table, and the compiler-facing `MsProtoBuilder` (emit, line registration, back-patching, const pool, try blocks, shrink-on-build), verified by C unit tests in `tests/c/test_opcode.c` and `tests/c/test_proto.c`.

**Architecture:** Header-only instruction encoding (pure shifts, host-endianness independent) + table-driven opcode metadata + a builder that owns four growing arrays (code / consts / tryBlocks / lines) under a sticky-error model; `msProtoBuild` shrinks all arrays to exact length and hands ownership to the caller. No dependency on lexer/parser/object model — `struct MsObject` / `struct MsString` are forward declarations only.

**Tech Stack:** C11, CMake + Ninja (existing `build/`, MSVC /W4 /WX via `MSLANG_STRICT_WARNINGS`), task-02 core infra (`msAlloc/msRealloc/msFree`, `MsMemStats`, `MS_ASSERT`), self-built `tests/c/ms_test.h`.

**Spec (authoritative, read fully before each task):** `docs/tasks/05-bytecode-proto.md` — it contains the complete interface design: the exact `MsOpCode` enum order with phase annotations, the instruction bit layout, the six inline encode/decode signatures, `MsOpFormat`, the metadata table design, all four struct field lists (`MsTryBlock` / `MsLineEntry` / `MsProto` / `MsProtoBuilder`), every builder API signature with semantics (sticky errors, capacity doubling from 8, shrink-on-build, 65536 const cap, `MS_PROTO_PC_NONE`, run-encoded line table, `msProtoLineAt` returning 0 for empty, `msProtoDump` with `#k<idx>` placeholders), the test-plan coverage lists, and the acceptance criteria. Do NOT deviate from the spec's names/signatures; this plan pins only the gaps the spec explicitly leaves open (dump text format, growth-helper shape, line-encoding edge cases, cap-test technique).

**Style (binding):** `docs/language/10-c-style.md` — 2-space indent, 120 col, K&R braces (single-statement blocks included), `Type* name` (star to type), internal structs NOT typedef'd (`struct MsProto`), enums may be typedef'd with globally unique `MS_`-prefixed values, `//` comments only (English), fixed-width ints from `<stdint.h>`, `bool` from `<stdbool.h>`, output params last, includes ordered: own header → C system → project headers, guards `MSLANG_SRC_VM_MS_OPCODE_H_` / `MSLANG_SRC_VM_MS_PROTO_H_` (no `#pragma once`). Source files must be pure ASCII (MSVC C4819 breaks the build otherwise); do not copy the spec's `§` characters into C comments — write "section" instead.

**Task-02 interfaces (verified against the headers, use exactly these):**
- `include/mslang/error.h`: `MsResult` = `MS_OK` / `MS_ERROR_RUNTIME` / `MS_ERROR_SYNTAX` / `MS_ERROR_OOM`. **The spec's assumed `"core/ms_result.h"` does NOT exist — always use `<mslang/error.h>`.**
- `src/core/ms_memory.h`: `void* msAlloc(size_t)` (NULL on OOM or size == 0), `void* msRealloc(void*, size_t)` (NULL ptr == alloc; failure leaves the original block intact), `void msFree(void*)` (NULL no-op), `struct MsMemStats { allocCount, reallocCount, freeCount, liveBlocks, currentBytes, peakBytes, totalAllocatedBytes }`, `msMemGetStats`, `msMemResetStats`, `msMemSetFailAfter(int64_t)` (OOM injection; `n < 0` disables).
- `src/core/ms_common.h`: `MS_ASSERT(cond)` (debug-only), `MS_ARRAY_LEN`, `MS_UNREACHABLE`, `MS_UNUSED`.
- Test harness `tests/c/ms_test.h`: `MS_TEST(suite, name)` → `static void test##suite##name(void)`; `MS_ASSERT_TRUE(cond)`; `MS_ASSERT_EQ(expected, actual)` (long long compare); each test file ends with an explicit `static const MsTestCase msTestCases[] = {{"Suite.Name", testSuiteName}, ...};` + `MS_TEST_MAIN(msTestCases)`. Assertions do not abort; the test function runs to completion.

**Build / test commands (run from the repo root):**
```bash
cmake -S . -B build                 # re-configure after CMakeLists.txt changes
cmake --build build                 # incremental build (ninja + MSVC /W4 /WX)
ctest --test-dir build --output-on-failure -R test_opcode   # opcode tests only
ctest --test-dir build --output-on-failure -R test_proto    # proto tests only
ctest --test-dir build --output-on-failure                  # full suite (must stay green)
```

**Commit convention (AGENTS.md, binding):** `<gitmoji> <type>(<scope>): <message>`. Use `✨ feat(vm):` for implementation steps and `📝 docs(vm):` for the final status flip.

---

## Spec-gap pins (decisions this plan makes where the spec delegates)

1. **Includes:** the spec's assumed `"core/ms_result.h"` is replaced by `<mslang/error.h>` (verified). `ms_opcode.h` includes `<stdint.h>` + `"core/ms_common.h"` (needs `MS_ASSERT` for the inline sAx encoder; it does not need `MsResult`). `ms_proto.h` includes `<stdbool.h>`, `<stdint.h>`, `<stdio.h>` (FILE), `<mslang/error.h>` (MsResult), and `"vm/ms_opcode.h"` (MsOpCode in the builder API).
2. **`msProtoDump` text format** (golden tests compare byte-exactly). Every line ends with `\n`; no indentation anywhere:
   ```
   == proto ==
   name=-               (or: name=#name  when proto->name != NULL)
   sourceFile=-         (or: sourceFile=#file  when proto->sourceFile != NULL)
   params=2 locals=4 stack=8 flags=varargs
   code[4]:
   0000 MS_OP_LOAD_CONST A=0 Bx=0
   0001 MS_OP_JUMP sAx=1
   consts[1]:
   #k0
   tryBlocks[1]:
   [0, 2) handler=- finally=3
   lines[2]:
   pc=0 line=1
   pc=1 line=3
   ```
   Rules: `flags=` is the comma-joined list of the present flags among `async`, `varargs`, `kwargs` (in that fixed order), `-` when none. Instruction lines: `%04d` pc (decimal, zero-padded to 4), one space, op name, then ` A=%u Bx=%u` (ABC, both operands always printed) or ` sAx=%d` (SAX). Consts print one `#k%d\n` per entry (placeholder until task 06). Try blocks print `[%u, %u) handler=%s finally=%s` with `-` for `MS_PROTO_PC_NONE`. Lines print `pc=%u line=%u`. `name`/`sourceFile` print the fixed placeholders `#name`/`#file` when non-NULL (string contents are unreadable until task 06) and `-` when NULL.
3. **Growth helper shape:** file-static `static bool msProtoGrowArray(void** items, int* cap, size_t itemSize)` — new cap is 8 (`MS_PROTO_INITIAL_CAP`) when `*cap == 0`, else `*cap * 2`; grows via `msRealloc` (NULL original == fresh alloc); on OOM returns false with the original block untouched. Shrink helper: `static bool msProtoShrinkArray(void** items, int len, size_t itemSize)` — `len == 0` frees and stores NULL, else `msRealloc` to exact length. Both are the ONLY growth/shrink paths for the four arrays.
4. **Shared emit core:** file-static `static int msProtoEmit(struct MsProtoBuilder* b, uint32_t instr)` used by both emit variants. Order: sticky check → compute `needLineEntry` → grow code (if full) → grow lines (if a new run will be recorded and lines is full) → append instruction → append line entry. Growing BEFORE writing means an OOM failure leaves nothing appended. The sticky check inside `msProtoEmit` is what makes `msProtoEmitABC` short-circuit.
5. **Sticky-error rule, uniform:** every builder API call that fails records `b->error` (first failure sticks) and every later call short-circuits: `int`-returning APIs return -1, `MsResult`-returning APIs return `b->error`. This includes `msProtoPatchSAx` validation failures and `msProtoBuild` with `codeLen == 0` (recorded as sticky `MS_ERROR_SYNTAX`).
6. **sAx range check placement:** `msProtoEmitSAx` checks `-8388608 <= sax <= 8388607` itself (out of range → sticky `MS_ERROR_SYNTAX`, return -1, nothing emitted); `msOpEncodeSAx` only `MS_ASSERT`s the range (programming error in release). `msProtoPatchSAx` applies the same range check → `MS_ERROR_SYNTAX` (sticky per pin 5). The sticky check comes first in `msProtoEmitSAx` so an out-of-range `sax` never reaches the asserting encoder on an already-failed builder.
7. **Patch validation:** `pc >= 0 && pc < codeLen` AND `msOpFormatOf(msOpDecodeOp(code[pc])) == MS_OPFMT_SAX` AND sax in 24-bit range — any violation → `MS_ERROR_SYNTAX` (sticky). Success rewrites the word preserving the opcode: `code[pc] = msOpEncodeSAx(msOpDecodeOp(code[pc]), sax)`.
8. **Line run-encoding details:** `msProtoSetLine` only stores `pendingLine`/`hasPendingLine` (multiple SetLine calls without an intervening emit collapse to the last one). On the next successful emit, an entry `{pc, pendingLine}` is appended iff `linesLen == 0` OR `lines[linesLen-1].line != pendingLine`; `hasPendingLine` is then cleared regardless. Emits without a pending line record nothing.
9. **`msProtoLineAt` clamping:** binary search for the last entry with `entry.pc <= pc`; empty table → 0; `pc` beyond the last entry → last entry's line; `pc < lines[0].pc` → first entry's line (clamped to the first run — cannot happen in practice because the compiler registers a line before the first emit, so `lines[0].pc` is normally 0).
10. **65536 cap-test technique:** the cap check in `msProtoAddConst` runs BEFORE the pointer-dedup scan (`constsLen >= 65536` needs no backing array). The test writes `b.proto.constsLen = 65536;` directly — legitimate because `struct MsProto proto` is a public builder field the compiler writes metadata into — with an explicit comment that it simulates a full pool; one `msProtoAddConst` call must then return -1 with sticky `MS_ERROR_SYNTAX`. The test restores `b.proto.constsLen = 0;` before destroy. (Emitting 65536 real consts is rejected: pointer dedup is a linear scan, O(n^2) would take minutes.)
11. **`msProtoBuild` on success:** `msAlloc(sizeof(struct MsProto))`, struct-copy `b->proto` into it, then `memset(b, 0, sizeof(*b))` — the builder is left fully zeroed (arrays detached, caps 0, error `MS_OK`), so `msProtoBuilderDestroy` is a no-op and the builder is reusable. On any failure (sticky error, empty code, OOM during shrink/alloc) ownership stays with the caller, who must destroy the builder; `msRealloc` shrink failure leaves the original arrays intact, so mid-build OOM is always recoverable by destroy.
12. **`msProtoFree(NULL)` is a no-op.** It frees the four arrays and the struct, never the consts entries nor `name`/`sourceFile` (object-model ownership).
13. **Const dedup is pointer-identity only** in this task (`consts[i] == value` linear scan); value dedup via `msObjectValueEquals` is task 06's integration point and is explicitly NOT tested here (per the spec's test plan note).
14. **Enum numeric values are internal** (spec: order may change before v1.0). Tests never hardcode opcode numbers; the bit-layout test masks fields (`instr & 0xFF` etc.) instead of comparing full words.
15. **v0.1 subset count test:** the test file carries a `static const MsOpCode excluded[]` table with the 19 non-v0.1 opcodes (IMPORT, IMPORT_FROM, BUILD_TUPLE, BUILD_SET, SLICE, SETUP_TRY, POP_TRY, RAISE, RERAISE + the 10 concurrency ops) and asserts `MS_OP_COUNT == 82`, `MS_OP_COUNT <= 256`, and `82 - 19 == 63`.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/vm/ms_opcode.h` | `MsOpFormat`, full `MsOpCode` enum (82 + `MS_OP_COUNT`), `_Static_assert` on the 8-bit opcode bound, six `static inline` encode/decode functions, `msOpFormatOf` / `msOpName` declarations. Self-contained. |
| `src/vm/ms_opcode.c` | `static const struct MsOpInfo opInfoTable[MS_OP_COUNT]` (name + format, enum order) and the two lookup functions. |
| `src/vm/ms_proto.h` | `struct MsTryBlock`, `struct MsLineEntry`, `struct MsProto`, `struct MsProtoBuilder`, `MS_PROTO_PC_NONE`, all builder/query/free/dump declarations; forward-declares `struct MsObject` / `struct MsString`. Self-contained. |
| `src/vm/ms_proto.c` | Builder implementation: grow/shrink helpers (file-static), emit core, line registration, patch, const pool, try blocks, build, free, lineAt, dump. |
| `tests/c/test_opcode.c` | Encode/decode round-trips, bit layout, subset counts, metadata table tests. |
| `tests/c/test_proto.c` | Builder lifecycle, emit/lines/patch/consts/try/build/free/lineAt/dump tests, leak assertions via `MsMemStats`. |
| `CMakeLists.txt` | Add `src/vm/ms_opcode.c` + `src/vm/ms_proto.c` to `add_library(mslang ...)`; register `mslang_add_c_test(test_opcode)` + `mslang_add_c_test(test_proto)`. |

---

## Task 1: `ms_opcode.h` — enum, formats, inline encode/decode

**Files:**
- Create: `src/vm/ms_opcode.h`
- Test: `tests/c/test_opcode.c` (new file)
- Modify: `CMakeLists.txt` (register `test_opcode` only — no `ms_opcode.c` exists yet)

- [ ] **Step 1: Write the failing tests** in `tests/c/test_opcode.c` (complete initial file):

```c
#include <stdint.h>

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

static const MsTestCase msTestCases[] = {
  {"Opcode.EncodeDecodeABCRoundTrip", testopcodeencodeDecodeABCRoundTrip},
  {"Opcode.EncodeDecodeSAxRoundTrip", testopcodeencodeDecodeSAxRoundTrip},
  {"Opcode.BitLayout", testopcodebitLayout},
  {"Opcode.SubsetCounts", testopcodesubsetCounts}
};

MS_TEST_MAIN(msTestCases)
```

- [ ] **Step 2: Wire CMake and run to verify the red state**

In `CMakeLists.txt`, inside the `if(MSLANG_BUILD_TESTS)` block, after `mslang_add_c_test(test_parser)` add:

```cmake
  mslang_add_c_test(test_opcode)
```

Run: `cmake -S . -B build && cmake --build build`
Expected: FAIL — `vm/ms_opcode.h` not found when compiling `tests/c/test_opcode.c`. That is the expected red state.

- [ ] **Step 3: Write `src/vm/ms_opcode.h` in full** (the complete pinned header; enum order and phase comments per the spec, ASCII-only comments):

```c
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
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_opcode`
Expected: 4/4 opcode tests PASS; then full suite `ctest --test-dir build --output-on-failure` stays green.

- [ ] **Step 5: Commit**

```bash
git add src/vm/ms_opcode.h tests/c/test_opcode.c CMakeLists.txt
git commit -m "✨ feat(vm): add opcode enum and instruction encoding"
```

---

## Task 2: `ms_opcode.c` — opcode metadata table

**Files:**
- Create: `src/vm/ms_opcode.c`
- Test: `tests/c/test_opcode.c`
- Modify: `CMakeLists.txt` (add `src/vm/ms_opcode.c` to the `mslang` library)

- [ ] **Step 1: Write the failing tests** — append to `tests/c/test_opcode.c`, and add `#include <string.h>` to the C-system include group at the top (before the project includes):

```c
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
```

Updated case table (full replacement of `msTestCases`):

```c
static const MsTestCase msTestCases[] = {
  {"Opcode.EncodeDecodeABCRoundTrip", testopcodeencodeDecodeABCRoundTrip},
  {"Opcode.EncodeDecodeSAxRoundTrip", testopcodeencodeDecodeSAxRoundTrip},
  {"Opcode.BitLayout", testopcodebitLayout},
  {"Opcode.SubsetCounts", testopcodesubsetCounts},
  {"Opcode.NameTableCompleteAndDistinct", testopcodenameTableCompleteAndDistinct},
  {"Opcode.FormatOfSaxOps", testopcodeformatOfSaxOps}
};
```

- [ ] **Step 2: Wire CMake and run to verify the red state**

In `CMakeLists.txt`, in `add_library(mslang ...)`, after `src/parser/ms_parser.c` add `src/vm/ms_opcode.c` so the list ends:

```cmake
    src/parser/ms_ast.c
    src/parser/ms_parser.c
    src/vm/ms_opcode.c)
```

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure -R test_opcode`
Expected: FAIL — unresolved externals `msOpName` / `msOpFormatOf` (link error). That is the expected red state.

- [ ] **Step 3: Write `src/vm/ms_opcode.c` in full** (entries in EXACT enum order — the table is declared `[MS_OP_COUNT]` with designated initializers, so a missed entry would zero-init and fail the non-NULL name test):

```c
#include "vm/ms_opcode.h"

#include "core/ms_common.h"

struct MsOpInfo {
  const char* name;
  MsOpFormat format;
};

static const struct MsOpInfo opInfoTable[MS_OP_COUNT] = {
  [MS_OP_LOAD_CONST] = { "MS_OP_LOAD_CONST", MS_OPFMT_ABC },
  [MS_OP_LOAD_NIL] = { "MS_OP_LOAD_NIL", MS_OPFMT_ABC },
  [MS_OP_LOAD_TRUE] = { "MS_OP_LOAD_TRUE", MS_OPFMT_ABC },
  [MS_OP_LOAD_FALSE] = { "MS_OP_LOAD_FALSE", MS_OPFMT_ABC },
  [MS_OP_MOVE] = { "MS_OP_MOVE", MS_OPFMT_ABC },
  [MS_OP_LOAD_LOCAL] = { "MS_OP_LOAD_LOCAL", MS_OPFMT_ABC },
  [MS_OP_STORE_LOCAL] = { "MS_OP_STORE_LOCAL", MS_OPFMT_ABC },
  [MS_OP_LOAD_UPVAL] = { "MS_OP_LOAD_UPVAL", MS_OPFMT_ABC },
  [MS_OP_STORE_UPVAL] = { "MS_OP_STORE_UPVAL", MS_OPFMT_ABC },
  [MS_OP_CLOSE_UPVALS] = { "MS_OP_CLOSE_UPVALS", MS_OPFMT_ABC },
  [MS_OP_LOAD_GLOBAL] = { "MS_OP_LOAD_GLOBAL", MS_OPFMT_ABC },
  [MS_OP_STORE_GLOBAL] = { "MS_OP_STORE_GLOBAL", MS_OPFMT_ABC },
  [MS_OP_IMPORT] = { "MS_OP_IMPORT", MS_OPFMT_ABC },
  [MS_OP_IMPORT_FROM] = { "MS_OP_IMPORT_FROM", MS_OPFMT_ABC },
  [MS_OP_ADD] = { "MS_OP_ADD", MS_OPFMT_ABC },
  [MS_OP_SUB] = { "MS_OP_SUB", MS_OPFMT_ABC },
  [MS_OP_MUL] = { "MS_OP_MUL", MS_OPFMT_ABC },
  [MS_OP_DIV] = { "MS_OP_DIV", MS_OPFMT_ABC },
  [MS_OP_FLOORDIV] = { "MS_OP_FLOORDIV", MS_OPFMT_ABC },
  [MS_OP_MOD] = { "MS_OP_MOD", MS_OPFMT_ABC },
  [MS_OP_POW] = { "MS_OP_POW", MS_OPFMT_ABC },
  [MS_OP_NEG] = { "MS_OP_NEG", MS_OPFMT_ABC },
  [MS_OP_NOT] = { "MS_OP_NOT", MS_OPFMT_ABC },
  [MS_OP_BITAND] = { "MS_OP_BITAND", MS_OPFMT_ABC },
  [MS_OP_BITOR] = { "MS_OP_BITOR", MS_OPFMT_ABC },
  [MS_OP_BITXOR] = { "MS_OP_BITXOR", MS_OPFMT_ABC },
  [MS_OP_SHL] = { "MS_OP_SHL", MS_OPFMT_ABC },
  [MS_OP_SHR] = { "MS_OP_SHR", MS_OPFMT_ABC },
  [MS_OP_INVERT] = { "MS_OP_INVERT", MS_OPFMT_ABC },
  [MS_OP_EQ] = { "MS_OP_EQ", MS_OPFMT_ABC },
  [MS_OP_NE] = { "MS_OP_NE", MS_OPFMT_ABC },
  [MS_OP_LT] = { "MS_OP_LT", MS_OPFMT_ABC },
  [MS_OP_LE] = { "MS_OP_LE", MS_OPFMT_ABC },
  [MS_OP_GT] = { "MS_OP_GT", MS_OPFMT_ABC },
  [MS_OP_GE] = { "MS_OP_GE", MS_OPFMT_ABC },
  [MS_OP_IS] = { "MS_OP_IS", MS_OPFMT_ABC },
  [MS_OP_IN] = { "MS_OP_IN", MS_OPFMT_ABC },
  [MS_OP_CMP_CHAIN] = { "MS_OP_CMP_CHAIN", MS_OPFMT_ABC },
  [MS_OP_JUMP] = { "MS_OP_JUMP", MS_OPFMT_SAX },
  [MS_OP_JUMP_IF_FALSE] = { "MS_OP_JUMP_IF_FALSE", MS_OPFMT_SAX },
  [MS_OP_JUMP_IF_TRUE] = { "MS_OP_JUMP_IF_TRUE", MS_OPFMT_SAX },
  [MS_OP_JUMP_IF_NIL] = { "MS_OP_JUMP_IF_NIL", MS_OPFMT_SAX },
  [MS_OP_BUILD_LIST] = { "MS_OP_BUILD_LIST", MS_OPFMT_ABC },
  [MS_OP_BUILD_TUPLE] = { "MS_OP_BUILD_TUPLE", MS_OPFMT_ABC },
  [MS_OP_BUILD_DICT] = { "MS_OP_BUILD_DICT", MS_OPFMT_ABC },
  [MS_OP_BUILD_SET] = { "MS_OP_BUILD_SET", MS_OPFMT_ABC },
  [MS_OP_INDEX] = { "MS_OP_INDEX", MS_OPFMT_ABC },
  [MS_OP_SET_INDEX] = { "MS_OP_SET_INDEX", MS_OPFMT_ABC },
  [MS_OP_DEL_INDEX] = { "MS_OP_DEL_INDEX", MS_OPFMT_ABC },
  [MS_OP_SLICE] = { "MS_OP_SLICE", MS_OPFMT_ABC },
  [MS_OP_APPEND] = { "MS_OP_APPEND", MS_OPFMT_ABC },
  [MS_OP_CALL] = { "MS_OP_CALL", MS_OPFMT_ABC },
  [MS_OP_CALL_KW] = { "MS_OP_CALL_KW", MS_OPFMT_ABC },
  [MS_OP_TAIL_CALL] = { "MS_OP_TAIL_CALL", MS_OPFMT_ABC },
  [MS_OP_RETURN] = { "MS_OP_RETURN", MS_OPFMT_ABC },
  [MS_OP_LOAD_METHOD] = { "MS_OP_LOAD_METHOD", MS_OPFMT_ABC },
  [MS_OP_CALL_METHOD] = { "MS_OP_CALL_METHOD", MS_OPFMT_ABC },
  [MS_OP_MAKE_FUNCTION] = { "MS_OP_MAKE_FUNCTION", MS_OPFMT_ABC },
  [MS_OP_MAKE_CLASS] = { "MS_OP_MAKE_CLASS", MS_OPFMT_ABC },
  [MS_OP_MAKE_LAMBDA] = { "MS_OP_MAKE_LAMBDA", MS_OPFMT_ABC },
  [MS_OP_GET_ATTR] = { "MS_OP_GET_ATTR", MS_OPFMT_ABC },
  [MS_OP_SET_ATTR] = { "MS_OP_SET_ATTR", MS_OPFMT_ABC },
  [MS_OP_DEL_ATTR] = { "MS_OP_DEL_ATTR", MS_OPFMT_ABC },
  [MS_OP_GET_ITER] = { "MS_OP_GET_ITER", MS_OPFMT_ABC },
  [MS_OP_ITER_NEXT] = { "MS_OP_ITER_NEXT", MS_OPFMT_SAX },
  [MS_OP_UNPACK] = { "MS_OP_UNPACK", MS_OPFMT_ABC },
  [MS_OP_SETUP_TRY] = { "MS_OP_SETUP_TRY", MS_OPFMT_ABC },
  [MS_OP_POP_TRY] = { "MS_OP_POP_TRY", MS_OPFMT_ABC },
  [MS_OP_RAISE] = { "MS_OP_RAISE", MS_OPFMT_ABC },
  [MS_OP_RERAISE] = { "MS_OP_RERAISE", MS_OPFMT_ABC },
  [MS_OP_SPAWN] = { "MS_OP_SPAWN", MS_OPFMT_ABC },
  [MS_OP_AWAIT] = { "MS_OP_AWAIT", MS_OPFMT_ABC },
  [MS_OP_CHAN_NEW] = { "MS_OP_CHAN_NEW", MS_OPFMT_ABC },
  [MS_OP_CHAN_SEND] = { "MS_OP_CHAN_SEND", MS_OPFMT_ABC },
  [MS_OP_CHAN_RECV] = { "MS_OP_CHAN_RECV", MS_OPFMT_ABC },
  [MS_OP_CHAN_TRY_RECV] = { "MS_OP_CHAN_TRY_RECV", MS_OPFMT_ABC },
  [MS_OP_SELECT_BEGIN] = { "MS_OP_SELECT_BEGIN", MS_OPFMT_ABC },
  [MS_OP_SELECT_ADD_RECV] = { "MS_OP_SELECT_ADD_RECV", MS_OPFMT_ABC },
  [MS_OP_SELECT_ADD_SEND] = { "MS_OP_SELECT_ADD_SEND", MS_OPFMT_ABC },
  [MS_OP_SELECT_EXEC] = { "MS_OP_SELECT_EXEC", MS_OPFMT_ABC },
  [MS_OP_PRINT_EXPR] = { "MS_OP_PRINT_EXPR", MS_OPFMT_ABC },
  [MS_OP_NOP] = { "MS_OP_NOP", MS_OPFMT_ABC }
};

MsOpFormat msOpFormatOf(MsOpCode op) {
  MS_ASSERT(op >= 0 && op < MS_OP_COUNT);
  return opInfoTable[op].format;
}

const char* msOpName(MsOpCode op) {
  MS_ASSERT(op >= 0 && op < MS_OP_COUNT);
  return opInfoTable[op].name;
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_opcode`
Expected: 6/6 opcode tests PASS; full suite stays green.

- [ ] **Step 5: Commit**

```bash
git add src/vm/ms_opcode.c tests/c/test_opcode.c CMakeLists.txt
git commit -m "✨ feat(vm): add opcode metadata table"
```

---

## Task 3: `ms_proto.h` + builder init/destroy skeleton

**Files:**
- Create: `src/vm/ms_proto.h`
- Create: `src/vm/ms_proto.c`
- Test: `tests/c/test_proto.c` (new file)
- Modify: `CMakeLists.txt` (add `src/vm/ms_proto.c` to the library, register `test_proto`)

- [ ] **Step 1: Write the failing tests** in `tests/c/test_proto.c` (complete initial file):

```c
#include <stdbool.h>

#include "core/ms_memory.h"
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

static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks}
};

MS_TEST_MAIN(msTestCases)
```

- [ ] **Step 2: Wire CMake and run to verify the red state**

In `CMakeLists.txt`:
- In `add_library(mslang ...)`, after `src/vm/ms_opcode.c` add `src/vm/ms_proto.c` (keep the closing `)` on the last entry).
- In the `if(MSLANG_BUILD_TESTS)` block, after `mslang_add_c_test(test_opcode)` add:

```cmake
  mslang_add_c_test(test_proto)
```

Run: `cmake -S . -B build && cmake --build build`
Expected: FAIL — `vm/ms_proto.h` not found, then `src/vm/ms_proto.c` missing. That is the expected red state.

- [ ] **Step 3: Write `src/vm/ms_proto.h` in full** (complete pinned header — struct fields and signatures per the spec, comments ASCII-only):

```c
#ifndef MSLANG_SRC_VM_MS_PROTO_H_
#define MSLANG_SRC_VM_MS_PROTO_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <mslang/error.h>

#include "vm/ms_opcode.h"

// Incomplete until task 06 (object model); the pool and name fields are
// pointers only, so forward declarations suffice here.
struct MsObject;
struct MsString;

struct MsTryBlock {        // exception table entry (08-vm-internals section 2.1 layout)
  uint32_t pcStart;        // protected range [pcStart, pcEnd)
  uint32_t pcEnd;
  uint32_t handlerPc;      // catch entry; MS_PROTO_PC_NONE if absent
  uint32_t finallyPc;      // finally entry; MS_PROTO_PC_NONE if absent
};

#define MS_PROTO_PC_NONE UINT32_MAX

struct MsLineEntry {       // pc -> line map, sorted by pc, run-encoded
  uint32_t pc;             // first pc of this run
  uint32_t line;           // 1-based source line
};

struct MsProto {
  uint32_t* code;           // instruction stream, msAlloc'd, codeLen entries
  struct MsObject** consts; // constant pool: numbers, strings, child protos
  int codeLen;
  int constsLen;
  int paramCount;           // parameter count
  int localCount;           // register window size (first paramCount slots hold args)
  int stackSize;            // max eval stack depth (computed at compile time)
  struct MsTryBlock* tryBlocks;  // exception table; NULL/0 until exceptions land (v0.2)
  int tryBlocksLen;
  struct MsLineEntry* lines;     // pc -> line map
  int linesLen;
  struct MsString* name;         // function name; "<main>" for module top level
  struct MsString* sourceFile;
  bool isAsync;
  bool hasVarArgs;
  bool hasKwArgs;
};

struct MsProtoBuilder {
  struct MsProto proto;    // arrays grow to capacity; shrunk and detached by msProtoBuild
  int codeCap;
  int constsCap;
  int tryCap;
  int linesCap;
  uint32_t pendingLine;    // line registered for the next emit
  bool hasPendingLine;
  MsResult error;          // sticky first failure (MS_OK while healthy)
};

void msProtoBuilderInit(struct MsProtoBuilder* b);

// Frees the builder-owned arrays and re-zeroes the builder. Safe after a
// failed build; a no-op after a successful one.
void msProtoBuilderDestroy(struct MsProtoBuilder* b);

// Emits one instruction with the pending line; returns its pc, or -1 (sticky
// MS_ERROR_OOM, or MS_ERROR_SYNTAX when sax is out of 24-bit range).
// MS_ASSERTs that op's declared format matches the emit variant used.
int msProtoEmitABC(struct MsProtoBuilder* b, MsOpCode op, uint8_t a, uint16_t bx);
int msProtoEmitSAx(struct MsProtoBuilder* b, MsOpCode op, int32_t sax);

// Registers the source line for subsequently emitted instructions.
void msProtoSetLine(struct MsProtoBuilder* b, uint32_t line);

// Rewrites the sAx operand of a previously emitted jump (back-patching).
// pc must address an MS_OPFMT_SAX instruction.
MsResult msProtoPatchSAx(struct MsProtoBuilder* b, int pc, int32_t sax);

// Adds a constant with pointer-identity dedup; returns the pool index, or -1
// (sticky MS_ERROR_OOM, or MS_ERROR_SYNTAX when the pool exceeds 65536
// entries). Value dedup arrives with task 06 (msObjectValueEquals).
int msProtoAddConst(struct MsProtoBuilder* b, struct MsObject* value);

// Appends an exception table entry (v0.2 consumers; defined now per spec layout).
MsResult msProtoAddTryBlock(struct MsProtoBuilder* b, uint32_t pcStart, uint32_t pcEnd,
    uint32_t handlerPc, uint32_t finallyPc);

// Shrinks all arrays to exact length and hands the proto to *out (caller owns,
// frees with msProtoFree). Fails with the sticky error if any call failed, or
// with MS_ERROR_SYNTAX if codeLen == 0. On success the builder is left empty;
// on failure the caller still owns the builder and must destroy it.
MsResult msProtoBuild(struct MsProtoBuilder* b, struct MsProto** out);

// Frees a proto's arrays and the struct itself. Does NOT free consts entries
// or name/sourceFile (owned by the object model / GC once task 06 lands).
void msProtoFree(struct MsProto* proto);

// Binary-searches the line table; returns 0 when the table is empty.
uint32_t msProtoLineAt(const struct MsProto* proto, int pc);

// Disassembles the instruction stream and tables for debugging and tests.
// Constants print as "#k<index>" placeholders until the object model lands.
void msProtoDump(const struct MsProto* proto, FILE* out);

#endif  // MSLANG_SRC_VM_MS_PROTO_H_
```

- [ ] **Step 4: Write `src/vm/ms_proto.c` skeleton** (init/destroy only — the remaining declared functions land in Tasks 4–7; a static library needs no stubs for functions nothing references yet):

```c
#include "vm/ms_proto.h"

#include <string.h>

#include "core/ms_memory.h"

void msProtoBuilderInit(struct MsProtoBuilder* b) {
  memset(b, 0, sizeof(*b));
}

void msProtoBuilderDestroy(struct MsProtoBuilder* b) {
  msFree(b->proto.code);
  msFree(b->proto.consts);
  msFree(b->proto.tryBlocks);
  msFree(b->proto.lines);
  msProtoBuilderInit(b);
}
```

- [ ] **Step 5: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: 2/2 proto tests PASS; full suite stays green.

- [ ] **Step 6: Commit**

```bash
git add src/vm/ms_proto.h src/vm/ms_proto.c tests/c/test_proto.c CMakeLists.txt
git commit -m "✨ feat(vm): add proto builder skeleton"
```

---

## Task 4: Emit + line registration

**Files:**
- Modify: `src/vm/ms_proto.c`
- Test: `tests/c/test_proto.c`

- [ ] **Step 1: Write the failing tests** — append to `tests/c/test_proto.c`:

```c
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
```

Updated case table (full replacement of `msTestCases`):

```c
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
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: FAIL — unresolved externals `msProtoEmitABC` / `msProtoEmitSAx` / `msProtoSetLine` (link error).

- [ ] **Step 3: Implement emit + line registration** — append to `src/vm/ms_proto.c` (the grow helper and emit core are file-static per pin 3/4):

```c
#define MS_PROTO_INITIAL_CAP 8

// Doubles *cap (starting at MS_PROTO_INITIAL_CAP) and grows *items to match.
// Returns false on OOM; the original block is left untouched.
static bool msProtoGrowArray(void** items, int* cap, size_t itemSize) {
  int newCap = *cap == 0 ? MS_PROTO_INITIAL_CAP : *cap * 2;
  void* newItems = msRealloc(*items, (size_t)newCap * itemSize);
  if (newItems == NULL) {
    return false;
  }
  *items = newItems;
  *cap = newCap;
  return true;
}

// Shared emit core: grows code (and lines when a new run starts) BEFORE
// writing, so a failure leaves nothing appended. Returns the pc or -1.
static int msProtoEmit(struct MsProtoBuilder* b, uint32_t instr) {
  if (b->error != MS_OK) {
    return -1;
  }
  bool needLineEntry = b->hasPendingLine
      && (b->proto.linesLen == 0 || b->proto.lines[b->proto.linesLen - 1].line != b->pendingLine);
  if (b->proto.codeLen == b->codeCap
      && !msProtoGrowArray((void**)&b->proto.code, &b->codeCap, sizeof(uint32_t))) {
    b->error = MS_ERROR_OOM;
    return -1;
  }
  if (needLineEntry && b->proto.linesLen == b->linesCap
      && !msProtoGrowArray((void**)&b->proto.lines, &b->linesCap, sizeof(struct MsLineEntry))) {
    b->error = MS_ERROR_OOM;
    return -1;
  }
  int pc = b->proto.codeLen;
  b->proto.code[pc] = instr;
  b->proto.codeLen = pc + 1;
  if (b->hasPendingLine) {
    b->hasPendingLine = false;
    if (needLineEntry) {
      b->proto.lines[b->proto.linesLen].pc = (uint32_t)pc;
      b->proto.lines[b->proto.linesLen].line = b->pendingLine;
      ++b->proto.linesLen;
    }
  }
  return pc;
}

int msProtoEmitABC(struct MsProtoBuilder* b, MsOpCode op, uint8_t a, uint16_t bx) {
  MS_ASSERT(msOpFormatOf(op) == MS_OPFMT_ABC);
  return msProtoEmit(b, msOpEncodeABC(op, a, bx));
}

int msProtoEmitSAx(struct MsProtoBuilder* b, MsOpCode op, int32_t sax) {
  MS_ASSERT(msOpFormatOf(op) == MS_OPFMT_SAX);
  if (b->error != MS_OK) {
    return -1;
  }
  if (sax < -8388608 || sax > 8388607) {
    b->error = MS_ERROR_SYNTAX;
    return -1;
  }
  return msProtoEmit(b, msOpEncodeSAx(op, sax));
}

void msProtoSetLine(struct MsProtoBuilder* b, uint32_t line) {
  b->pendingLine = line;
  b->hasPendingLine = true;
}
```

Also add `"core/ms_common.h"` to the project-include group of `ms_proto.c` (for `MS_ASSERT`), keeping alphabetical order: `"core/ms_common.h"` before `"core/ms_memory.h"`.

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: 8/8 proto tests PASS; full suite stays green.

- [ ] **Step 5: Commit**

```bash
git add src/vm/ms_proto.c tests/c/test_proto.c
git commit -m "✨ feat(vm): add instruction emission and line registration"
```

---

## Task 5: `msProtoPatchSAx` — jump back-patching

**Files:**
- Modify: `src/vm/ms_proto.c`
- Test: `tests/c/test_proto.c`

- [ ] **Step 1: Write the failing tests** — append to `tests/c/test_proto.c`:

```c
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
```

Updated case table (full replacement of `msTestCases`):

```c
static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks},
  {"Proto.EmitReturnsSequentialPc", testprotoemitReturnsSequentialPc},
  {"Proto.EmitGrowthPreservesContents", testprotoemitGrowthPreservesContents},
  {"Proto.EmitSAxBoundariesOk", testprotoemitSAxBoundariesOk},
  {"Proto.EmitSAxOutOfRangeSticky", testprotoemitSAxOutOfRangeSticky},
  {"Proto.EmitOomSticky", testprotoemitOomSticky},
  {"Proto.LineRunEncoding", testprotolineRunEncoding},
  {"Proto.PatchSAxJump", testprotopatchSAxJump},
  {"Proto.PatchRejectsAbcInstruction", testprotopatchRejectsAbcInstruction},
  {"Proto.PatchRejectsBadPcAndRange", testprotopatchRejectsBadPcAndRange}
};
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: FAIL — unresolved external `msProtoPatchSAx`.

- [ ] **Step 3: Implement `msProtoPatchSAx`** — append to `src/vm/ms_proto.c` (validation per pin 7; sticky per pin 5):

```c
MsResult msProtoPatchSAx(struct MsProtoBuilder* b, int pc, int32_t sax) {
  if (b->error != MS_OK) {
    return b->error;
  }
  if (pc < 0 || pc >= b->proto.codeLen
      || msOpFormatOf(msOpDecodeOp(b->proto.code[pc])) != MS_OPFMT_SAX
      || sax < -8388608 || sax > 8388607) {
    b->error = MS_ERROR_SYNTAX;
    return MS_ERROR_SYNTAX;
  }
  b->proto.code[pc] = msOpEncodeSAx(msOpDecodeOp(b->proto.code[pc]), sax);
  return MS_OK;
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: 11/11 proto tests PASS; full suite stays green.

- [ ] **Step 5: Commit**

```bash
git add src/vm/ms_proto.c tests/c/test_proto.c
git commit -m "✨ feat(vm): add sAx jump back-patching"
```

---

## Task 6: `msProtoAddConst` + `msProtoAddTryBlock`

**Files:**
- Modify: `src/vm/ms_proto.c`
- Test: `tests/c/test_proto.c`

- [ ] **Step 1: Write the failing tests** — append to `tests/c/test_proto.c`. Add the fake-constant objects at file scope, right after the includes (they are never dereferenced — pointer identity only, per pin 13):

```c
// Stand-ins for struct MsObject until task 06 lands; never dereferenced.
static char msTestFakeConstA;
static char msTestFakeConstB;
static char msTestFakeConstC;
```

New tests:

```c
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
```

Updated case table (full replacement of `msTestCases`):

```c
static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks},
  {"Proto.EmitReturnsSequentialPc", testprotoemitReturnsSequentialPc},
  {"Proto.EmitGrowthPreservesContents", testprotoemitGrowthPreservesContents},
  {"Proto.EmitSAxBoundariesOk", testprotoemitSAxBoundariesOk},
  {"Proto.EmitSAxOutOfRangeSticky", testprotoemitSAxOutOfRangeSticky},
  {"Proto.EmitOomSticky", testprotoemitOomSticky},
  {"Proto.LineRunEncoding", testprotolineRunEncoding},
  {"Proto.PatchSAxJump", testprotopatchSAxJump},
  {"Proto.PatchRejectsAbcInstruction", testprotopatchRejectsAbcInstruction},
  {"Proto.PatchRejectsBadPcAndRange", testprotopatchRejectsBadPcAndRange},
  {"Proto.AddConstPointerDedup", testprotoaddConstPointerDedup},
  {"Proto.AddConstCap65536", testprotoaddConstCap65536},
  {"Proto.AddTryBlockFieldsPreserved", testprotoaddTryBlockFieldsPreserved}
};
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: FAIL — unresolved externals `msProtoAddConst` / `msProtoAddTryBlock`.

- [ ] **Step 3: Implement** — append to `src/vm/ms_proto.c`:

```c
#define MS_PROTO_CONSTS_MAX 65536   // Bx-indexed pool: 2^16 entries per proto

int msProtoAddConst(struct MsProtoBuilder* b, struct MsObject* value) {
  if (b->error != MS_OK) {
    return -1;
  }
  // The cap check runs before the dedup scan: a full pool needs no scan.
  if (b->proto.constsLen >= MS_PROTO_CONSTS_MAX) {
    b->error = MS_ERROR_SYNTAX;
    return -1;
  }
  // Pointer-identity dedup until task 06 lands msObjectValueEquals.
  for (int i = 0; i < b->proto.constsLen; ++i) {
    if (b->proto.consts[i] == value) {
      return i;
    }
  }
  if (b->proto.constsLen == b->constsCap
      && !msProtoGrowArray((void**)&b->proto.consts, &b->constsCap, sizeof(struct MsObject*))) {
    b->error = MS_ERROR_OOM;
    return -1;
  }
  b->proto.consts[b->proto.constsLen] = value;
  return b->proto.constsLen++;
}

MsResult msProtoAddTryBlock(struct MsProtoBuilder* b, uint32_t pcStart, uint32_t pcEnd,
    uint32_t handlerPc, uint32_t finallyPc) {
  if (b->error != MS_OK) {
    return b->error;
  }
  if (b->proto.tryBlocksLen == b->tryCap
      && !msProtoGrowArray((void**)&b->proto.tryBlocks, &b->tryCap, sizeof(struct MsTryBlock))) {
    b->error = MS_ERROR_OOM;
    return MS_ERROR_OOM;
  }
  struct MsTryBlock* block = &b->proto.tryBlocks[b->proto.tryBlocksLen];
  block->pcStart = pcStart;
  block->pcEnd = pcEnd;
  block->handlerPc = handlerPc;
  block->finallyPc = finallyPc;
  ++b->proto.tryBlocksLen;
  return MS_OK;
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: 14/14 proto tests PASS; full suite stays green.

- [ ] **Step 5: Commit**

```bash
git add src/vm/ms_proto.c tests/c/test_proto.c
git commit -m "✨ feat(vm): add constant pool and try-block builder APIs"
```

---

## Task 7: `msProtoBuild` / `msProtoFree` / `msProtoLineAt` / `msProtoDump`

**Files:**
- Modify: `src/vm/ms_proto.c`
- Test: `tests/c/test_proto.c`

- [ ] **Step 1: Write the failing tests** — append to `tests/c/test_proto.c`. Add `#include <stdio.h>` and `#include <string.h>` to the C-system include group at the top, and this dump helper after the fake-constant declarations:

```c
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
```

New tests:

```c
MS_TEST(proto, buildShrinksToExactLen) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA));
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
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ((long long)(sizeof(struct MsProto) + 2 * sizeof(uint32_t)
      + sizeof(struct MsObject*) + sizeof(struct MsLineEntry)), (long long)stats.currentBytes);
  // Builder left empty (pin 11); a second destroy is a safe no-op.
  MS_ASSERT_TRUE(b.proto.code == NULL);
  MS_ASSERT_EQ(0, b.proto.codeLen);
  MS_ASSERT_EQ(0, b.codeCap);
  MS_ASSERT_EQ(MS_OK, b.error);
  msProtoBuilderDestroy(&b);
  msProtoFree(proto);
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
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  MS_ASSERT_EQ(0, msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA));
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
}

MS_TEST(proto, buildFreeNoLeaks) {
  msMemResetStats();
  struct MsProtoBuilder b;
  msProtoBuilderInit(&b);
  msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstA);
  msProtoAddConst(&b, (struct MsObject*)&msTestFakeConstB);
  msProtoSetLine(&b, 1);
  msProtoEmitABC(&b, MS_OP_LOAD_CONST, 0, 0);
  msProtoSetLine(&b, 2);
  msProtoEmitABC(&b, MS_OP_RETURN, 0, 0);
  msProtoAddTryBlock(&b, 0, 1, 1, MS_PROTO_PC_NONE);
  struct MsProto* proto = NULL;
  MS_ASSERT_EQ(MS_OK, msProtoBuild(&b, &proto));
  msProtoFree(proto);
  struct MsMemStats stats;
  msMemGetStats(&stats);
  MS_ASSERT_EQ(0, (long long)stats.liveBlocks);
}
```

Updated case table (full replacement of `msTestCases` — final shape, 21 tests):

```c
static const MsTestCase msTestCases[] = {
  {"Proto.BuilderInitZeroed", testprotobuilderInitZeroed},
  {"Proto.BuilderDestroyEmptyNoLeaks", testprotobuilderDestroyEmptyNoLeaks},
  {"Proto.EmitReturnsSequentialPc", testprotoemitReturnsSequentialPc},
  {"Proto.EmitGrowthPreservesContents", testprotoemitGrowthPreservesContents},
  {"Proto.EmitSAxBoundariesOk", testprotoemitSAxBoundariesOk},
  {"Proto.EmitSAxOutOfRangeSticky", testprotoemitSAxOutOfRangeSticky},
  {"Proto.EmitOomSticky", testprotoemitOomSticky},
  {"Proto.LineRunEncoding", testprotolineRunEncoding},
  {"Proto.PatchSAxJump", testprotopatchSAxJump},
  {"Proto.PatchRejectsAbcInstruction", testprotopatchRejectsAbcInstruction},
  {"Proto.PatchRejectsBadPcAndRange", testprotopatchRejectsBadPcAndRange},
  {"Proto.AddConstPointerDedup", testprotoaddConstPointerDedup},
  {"Proto.AddConstCap65536", testprotoaddConstCap65536},
  {"Proto.AddTryBlockFieldsPreserved", testprotoaddTryBlockFieldsPreserved},
  {"Proto.BuildShrinksToExactLen", testprotobuildShrinksToExactLen},
  {"Proto.BuildZeroLengthArraysNull", testprotobuildZeroLengthArraysNull},
  {"Proto.BuildEmptyCodeFails", testprotobuildEmptyCodeFails},
  {"Proto.BuildWithStickyErrorFails", testprotobuildWithStickyErrorFails},
  {"Proto.LineAtBinarySearch", testprotolineAtBinarySearch},
  {"Proto.DumpGolden", testprotodumpGolden},
  {"Proto.BuildFreeNoLeaks", testprotobuildFreeNoLeaks}
};
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: FAIL — unresolved externals `msProtoBuild` / `msProtoFree` / `msProtoLineAt` / `msProtoDump`.

- [ ] **Step 3: Implement** — append to `src/vm/ms_proto.c` (shrink helper + the four public functions; dump format per pin 2, build semantics per pin 11):

```c
// Shrinks one array to exact length; len == 0 frees it and stores NULL.
// On OOM the original block is left untouched (still owned by the builder).
static bool msProtoShrinkArray(void** items, int len, size_t itemSize) {
  if (len == 0) {
    msFree(*items);
    *items = NULL;
    return true;
  }
  void* shrunk = msRealloc(*items, (size_t)len * itemSize);
  if (shrunk == NULL) {
    return false;
  }
  *items = shrunk;
  return true;
}

MsResult msProtoBuild(struct MsProtoBuilder* b, struct MsProto** out) {
  if (b->error != MS_OK) {
    return b->error;
  }
  if (b->proto.codeLen == 0) {
    b->error = MS_ERROR_SYNTAX;
    return MS_ERROR_SYNTAX;
  }
  if (!msProtoShrinkArray((void**)&b->proto.code, b->proto.codeLen, sizeof(uint32_t))
      || !msProtoShrinkArray((void**)&b->proto.consts, b->proto.constsLen, sizeof(struct MsObject*))
      || !msProtoShrinkArray((void**)&b->proto.tryBlocks, b->proto.tryBlocksLen, sizeof(struct MsTryBlock))
      || !msProtoShrinkArray((void**)&b->proto.lines, b->proto.linesLen, sizeof(struct MsLineEntry))) {
    b->error = MS_ERROR_OOM;
    return MS_ERROR_OOM;
  }
  struct MsProto* proto = msAlloc(sizeof(struct MsProto));
  if (proto == NULL) {
    b->error = MS_ERROR_OOM;
    return MS_ERROR_OOM;
  }
  *proto = b->proto;
  memset(b, 0, sizeof(*b));   // builder left empty; destroy is a no-op
  *out = proto;
  return MS_OK;
}

void msProtoFree(struct MsProto* proto) {
  if (proto == NULL) {
    return;
  }
  msFree(proto->code);
  msFree(proto->consts);   // pool entries belong to the object model, not freed here
  msFree(proto->tryBlocks);
  msFree(proto->lines);
  msFree(proto);
}

uint32_t msProtoLineAt(const struct MsProto* proto, int pc) {
  if (proto->linesLen == 0) {
    return 0;
  }
  // Last entry with entry.pc <= pc, clamped to the first/last run.
  int lo = 0;
  int hi = proto->linesLen - 1;
  if (pc < (int)proto->lines[lo].pc) {
    return proto->lines[lo].line;
  }
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if ((int)proto->lines[mid].pc <= pc) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return proto->lines[lo].line;
}

// Prints handlerPc/finallyPc as "-" for MS_PROTO_PC_NONE, else the number.
static void msProtoDumpPc(uint32_t pc, char* buf, size_t bufSize) {
  if (pc == MS_PROTO_PC_NONE) {
    snprintf(buf, bufSize, "-");
  } else {
    snprintf(buf, bufSize, "%u", (unsigned)pc);
  }
}

void msProtoDump(const struct MsProto* proto, FILE* out) {
  fprintf(out, "== proto ==\n");
  fprintf(out, "name=%s\n", proto->name != NULL ? "#name" : "-");
  fprintf(out, "sourceFile=%s\n", proto->sourceFile != NULL ? "#file" : "-");
  fprintf(out, "params=%d locals=%d stack=%d flags=",
      proto->paramCount, proto->localCount, proto->stackSize);
  bool anyFlag = false;
  if (proto->isAsync) {
    fprintf(out, "async");
    anyFlag = true;
  }
  if (proto->hasVarArgs) {
    fprintf(out, "%svarargs", anyFlag ? "," : "");
    anyFlag = true;
  }
  if (proto->hasKwArgs) {
    fprintf(out, "%skwargs", anyFlag ? "," : "");
    anyFlag = true;
  }
  if (!anyFlag) {
    fprintf(out, "-");
  }
  fprintf(out, "\n");
  fprintf(out, "code[%d]:\n", proto->codeLen);
  for (int pc = 0; pc < proto->codeLen; ++pc) {
    uint32_t instr = proto->code[pc];
    MsOpCode op = msOpDecodeOp(instr);
    if (msOpFormatOf(op) == MS_OPFMT_SAX) {
      fprintf(out, "%04d %s sAx=%d\n", pc, msOpName(op), msOpDecodeSAx(instr));
    } else {
      fprintf(out, "%04d %s A=%u Bx=%u\n", pc, msOpName(op),
          (unsigned)msOpDecodeA(instr), (unsigned)msOpDecodeBx(instr));
    }
  }
  fprintf(out, "consts[%d]:\n", proto->constsLen);
  for (int i = 0; i < proto->constsLen; ++i) {
    fprintf(out, "#k%d\n", i);   // placeholder until the object model lands
  }
  fprintf(out, "tryBlocks[%d]:\n", proto->tryBlocksLen);
  for (int i = 0; i < proto->tryBlocksLen; ++i) {
    const struct MsTryBlock* block = &proto->tryBlocks[i];
    char handler[16];
    char finally[16];
    msProtoDumpPc(block->handlerPc, handler, sizeof(handler));
    msProtoDumpPc(block->finallyPc, finally, sizeof(finally));
    fprintf(out, "[%u, %u) handler=%s finally=%s\n",
        (unsigned)block->pcStart, (unsigned)block->pcEnd, handler, finally);
  }
  fprintf(out, "lines[%d]:\n", proto->linesLen);
  for (int i = 0; i < proto->linesLen; ++i) {
    fprintf(out, "pc=%u line=%u\n",
        (unsigned)proto->lines[i].pc, (unsigned)proto->lines[i].line);
  }
}
```

- [ ] **Step 4: Run tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure -R test_proto`
Expected: 21/21 proto tests PASS; full suite `ctest --test-dir build --output-on-failure` green.

- [ ] **Step 5: Commit**

```bash
git add src/vm/ms_proto.c tests/c/test_proto.c
git commit -m "✨ feat(vm): add proto build, free, line lookup and dump"
```

---

## Task 8: Full suite green, strict warnings, status flip

**Files:**
- Modify: `docs/tasks/README.md`
- Modify: `docs/tasks/05-bytecode-proto.md`

- [ ] **Step 1: Full verification run**

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: the whole suite green (including `test_opcode` 6/6 and `test_proto` 21/21); zero warnings (`MSLANG_STRICT_WARNINGS` is ON: MSVC /W4 /WX — any warning fails the build). Build artifacts only in `build/`.

- [ ] **Step 2: Acceptance-criteria sweep** — re-read `docs/tasks/05-bytecode-proto.md` 验收标准 and confirm each box:

- guards `MSLANG_SRC_VM_MS_OPCODE_H_` / `MSLANG_SRC_VM_MS_PROTO_H_`, headers self-contained (each header compiles as the first include of its own TU — already exercised by the two test files), style rules (2-space, 120 col, K&R, star-to-type, no typedef'd internal structs, `//` English comments, pure ASCII).
- 4-byte instructions, both layouts, round-trips + boundaries (Task 1/2 tests), sAx signed 24-bit, `target = pc + 1 + sAx` (Task 5 test asserts the target arithmetic in a comment and by construction).
- 82 + `MS_OP_COUNT`, v0.1 = 63, metadata total order (Tasks 1–2).
- `struct MsProto` fields per spec incl. `tryBlocksLen`/`linesLen`, no GC header; 65536 cap + pointer dedup + ownership (Task 6).
- Full builder API, sticky errors, ownership transfer, `msAlloc/msRealloc/msFree` only, stats show no leaks (Tasks 4–7).
- Run-encoded line table + binary search, try-block layout with `MS_PROTO_PC_NONE` (Tasks 4, 6, 7).
- Both test files cover the spec's 测试方案 list and pass; artifacts only in `build/`.
- No TBD/TODO in the new files; task-02 interface names aligned to the real headers (`<mslang/error.h>`, not `core/ms_result.h`).

- [ ] **Step 3: Flip the status markers**

- `docs/tasks/README.md`: change the row `| ⬜ | 05 | v0.1 | [字节码格式与 MsProto](05-bytecode-proto.md) |` to `| ✅ | 05 | v0.1 | [字节码格式与 MsProto](05-bytecode-proto.md) |`.
- `docs/tasks/05-bytecode-proto.md`: header table `| v0.1 | ⬜ | ...` → `| v0.1 | ✅ | ...`; tick every 验收标准 checkbox (`- [ ]` → `- [x]`).

- [ ] **Step 4: Commit**

```bash
git add docs/tasks/README.md docs/tasks/05-bytecode-proto.md
git commit -m "📝 docs(vm): mark task 05 bytecode-proto complete"
```

---

## Self-review notes (plan author)

- **Acceptance-criteria mapping:** spec 实现步骤 1–8 map to Tasks 1–8 here; every 验收标准 box is exercised by the per-task tests plus the Task 8 sweep. The spec's 测试方案 bullets map as: encode/decode round-trips + boundaries → Task 1; metadata table + SAX formats + subset counts → Tasks 1–2; builder lifecycle → Task 3; emit pc sequence / growth / sticky / sAx range → Task 4; line run-encoding → Task 4 (+ Task 7 lineAt); patch placeholder/ABC-reject/out-of-range → Task 5; const dedup / cap / try-block fields → Task 6; build shrink / empty-code error / sticky build / double-destroy / lineAt branches / dump golden / leak-free → Task 7. The value-dedup case is explicitly excluded per the spec's own note (task 06 integration point).
- **Pinned-beyond-spec decisions** are all listed in "Spec-gap pins" — reviewers should check those against `docs/tasks/05-bytecode-proto.md` first. Notable ones beyond what was pre-decided: the exact dump text layout (pin 2), sticky-failure uniformity including patch and build (pin 5), `msProtoLineAt` clamping for pc before the first entry (pin 9), `msProtoFree(NULL)` no-op (pin 12), and the enum-numeric-value independence of tests (pin 14).
- **Known risk points for reviewers:** (a) the `void**` casts in the grow/shrink helpers — standard practice, works on MSVC/GCC/Clang, kept file-static; (b) the direct `b.proto.constsLen = 65536` write in the cap test (pin 10) — deliberate, commented in the test; (c) `msOpDecodeSAx` sign extension avoids shifting into the sign bit (C11-UB-free) — do not "simplify" it to `(raw << 8) >> 8`; (d) the dump golden test depends on the byte-exact pinned format — any format change must update pin 2 and the golden string together; (e) `msProtoEmit` grows lines BEFORE appending the instruction, so an OOM mid-emit never leaves a dangling instruction without its line entry.
