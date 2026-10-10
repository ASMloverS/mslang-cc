#include "vm/ms_proto.h"

#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

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
