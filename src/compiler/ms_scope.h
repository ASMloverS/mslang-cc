#ifndef MSLANG_SRC_COMPILER_MS_SCOPE_H_
#define MSLANG_SRC_COMPILER_MS_SCOPE_H_

#include <stdbool.h>

#include <mslang/error.h>

struct MsString;

#define MS_SCOPE_MAX_LOCALS 256
#define MS_SCOPE_MAX_DEPTH 128

struct MsLocal {
  struct MsString* name;
  int depth;
  bool isCaptured;
};

struct MsScopeBlock {
  int localBase;
  int discardSlot;
};

struct MsScopeCtx {
  struct MsLocal locals[MS_SCOPE_MAX_LOCALS];
  int localCount;
  int localMax;
  int scopeDepth;
  struct MsScopeBlock blocks[MS_SCOPE_MAX_DEPTH];
  struct MsString** globalNames;
  int globalCount;
  int globalCap;
  bool isModule;
};

void msScopeCtxInit(struct MsScopeCtx* ctx, bool isModule);
void msScopeCtxDestroy(struct MsScopeCtx* ctx);
void msScopeBeginBlock(struct MsScopeCtx* ctx);
void msScopeEndBlock(struct MsScopeCtx* ctx);
int msScopeDeclareLocal(struct MsScopeCtx* ctx, struct MsString* name);
int msScopeAllocHiddenLocal(struct MsScopeCtx* ctx);
int msScopeResolveLocal(const struct MsScopeCtx* ctx, const struct MsString* name);
bool msScopeHasLocalAtDepth(const struct MsScopeCtx* ctx, const struct MsString* name);
MsResult msScopeMarkGlobal(struct MsScopeCtx* ctx, struct MsString* name);
bool msScopeIsMarkedGlobal(const struct MsScopeCtx* ctx, const struct MsString* name);

#endif  // MSLANG_SRC_COMPILER_MS_SCOPE_H_
