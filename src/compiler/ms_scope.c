#include "compiler/ms_scope.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_string.h"

void msScopeCtxInit(struct MsScopeCtx* ctx, bool isModule) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->isModule = isModule;
  ctx->blocks[0].discardSlot = -1;
}

void msScopeCtxDestroy(struct MsScopeCtx* ctx) {
  msFree(ctx->globalNames);
  memset(ctx, 0, sizeof(*ctx));
}

void msScopeBeginBlock(struct MsScopeCtx* ctx) {
  MS_ASSERT(ctx->scopeDepth + 1 < MS_SCOPE_MAX_DEPTH);
  ++ctx->scopeDepth;
  ctx->blocks[ctx->scopeDepth].localBase = ctx->localCount;
  ctx->blocks[ctx->scopeDepth].discardSlot = -1;
}

void msScopeEndBlock(struct MsScopeCtx* ctx) {
  MS_ASSERT(ctx->scopeDepth > 0);
  ctx->localCount = ctx->blocks[ctx->scopeDepth].localBase;
  ctx->blocks[ctx->scopeDepth].discardSlot = -1;
  --ctx->scopeDepth;
}

static int msScopeAllocLocal(struct MsScopeCtx* ctx, struct MsString* name) {
  if (ctx->localCount >= MS_SCOPE_MAX_LOCALS) {
    return -1;
  }
  int slot = ctx->localCount++;
  ctx->locals[slot].name = name;
  ctx->locals[slot].depth = ctx->scopeDepth;
  ctx->locals[slot].isCaptured = false;
  if (ctx->localCount > ctx->localMax) {
    ctx->localMax = ctx->localCount;
  }
  return slot;
}

int msScopeDeclareLocal(struct MsScopeCtx* ctx, struct MsString* name) {
  MS_ASSERT(name != NULL);
  return msScopeAllocLocal(ctx, name);
}

int msScopeAllocHiddenLocal(struct MsScopeCtx* ctx) {
  return msScopeAllocLocal(ctx, NULL);
}

int msScopeResolveLocal(const struct MsScopeCtx* ctx, const struct MsString* name) {
  for (int i = ctx->localCount - 1; i >= 0; --i) {
    if (ctx->locals[i].name != NULL && msStringEquals(ctx->locals[i].name, name)) {
      return i;
    }
  }
  return -1;
}

bool msScopeHasLocalAtDepth(const struct MsScopeCtx* ctx, const struct MsString* name) {
  for (int i = ctx->localCount - 1; i >= 0; --i) {
    const struct MsLocal* local = &ctx->locals[i];
    if (local->depth == ctx->scopeDepth && local->name != NULL
        && msStringEquals(local->name, name)) {
      return true;
    }
  }
  return false;
}

bool msScopeIsMarkedGlobal(const struct MsScopeCtx* ctx, const struct MsString* name) {
  for (int i = 0; i < ctx->globalCount; ++i) {
    if (msStringEquals(ctx->globalNames[i], name)) {
      return true;
    }
  }
  return false;
}

MsResult msScopeMarkGlobal(struct MsScopeCtx* ctx, struct MsString* name) {
  if (msScopeResolveLocal(ctx, name) >= 0) {
    return MS_ERROR_SYNTAX;
  }
  if (msScopeIsMarkedGlobal(ctx, name)) {
    return MS_OK;
  }
  if (ctx->globalCount == ctx->globalCap) {
    if (ctx->globalCap > INT_MAX / 2) {
      return MS_ERROR_OOM;
    }
    int newCap = ctx->globalCap == 0 ? 4 : ctx->globalCap * 2;
    if ((size_t)newCap > SIZE_MAX / sizeof(*ctx->globalNames)) {
      return MS_ERROR_OOM;
    }
    struct MsString** names = msRealloc(ctx->globalNames,
        (size_t)newCap * sizeof(*names));
    if (names == NULL) {
      return MS_ERROR_OOM;
    }
    ctx->globalNames = names;
    ctx->globalCap = newCap;
  }
  ctx->globalNames[ctx->globalCount++] = name;
  return MS_OK;
}
