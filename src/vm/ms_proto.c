#include "vm/ms_proto.h"

#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

#define MS_PROTO_INITIAL_CAP 8
#define MS_PROTO_CONSTS_MAX 65536   // Bx-indexed pool: 2^16 entries per proto

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
