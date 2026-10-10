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
