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
