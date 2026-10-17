#include "compiler/ms_compiler.h"

#include <stdbool.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "object/ms_object.h"
#include "object/ms_string.h"
#include "parser/ms_ast.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"

#define MS_COMPILER_MAX_LOCALS 256
#define MS_COMPILER_MAX_SCOPE_DEPTH 128
#define MS_COMPILER_MAX_NODE_DEPTH 512

struct MsLocalSlot {
  struct MsString* name;
  int depth;
  bool isCaptured;
};

struct MsScope {
  int localBase;
  int discardSlot;
};

struct MsFuncCtx {
  struct MsFuncCtx* parent;
  struct MsProtoBuilder builder;
  const struct MsAst* funcNode;
  const struct MsAst* currentNode;
  struct MsLocalSlot locals[MS_COMPILER_MAX_LOCALS];
  int localCount;
  int localMax;
  struct MsScope scopes[MS_COMPILER_MAX_SCOPE_DEPTH];
  int scopeDepth;
  int stackDepth;
  int stackMax;
  bool isModule;
  bool isAsync;
};

static void msCompilerReport(struct MsCompiler* c, const struct MsAst* node,
    uint32_t code, const char* message) {
  if (c->sticky == MS_ERROR_OOM) {
    return;
  }
  uint32_t line = node != NULL ? node->line : 0;
  uint32_t column = node != NULL ? node->column : 0;
  bool reported = msDiagReport(c->diags, line, column, code, "%s", message);
  if ((!reported || msDiagListIsFull(c->diags)) && c->sticky == MS_OK) {
    c->sticky = MS_ERROR_SYNTAX;
  }
}

static void msCompilerFail(struct MsCompiler* c, const struct MsAst* node,
    MsResult result, uint32_t code, const char* message) {
  if (c->sticky != MS_OK) {
    return;
  }
  msCompilerReport(c, node, code, message);
  c->sticky = result;
}

static void msCompilerSyncBuilder(struct MsCompiler* c) {
  MsResult result = c->fn->builder.error;
  if (result == MS_ERROR_OOM) {
    msCompilerFail(c, c->fn->currentNode, result, 0, "out of memory");
  } else if (result != MS_OK) {
    msCompilerFail(c, c->fn->currentNode, result, 309, "proto size limit exceeded");
  }
}

static int msCompilerEmitABC(struct MsCompiler* c, MsOpCode op, uint8_t a,
    uint16_t bx, int stackDelta) {
  if (c->sticky != MS_OK) {
    return -1;
  }
  int pc = msProtoEmitABC(&c->fn->builder, op, a, bx);
  if (pc < 0) {
    msCompilerSyncBuilder(c);
    return -1;
  }
  c->fn->stackDepth += stackDelta;
  MS_ASSERT(c->fn->stackDepth >= 0);
  if (c->fn->stackDepth > c->fn->stackMax) {
    c->fn->stackMax = c->fn->stackDepth;
  }
  return pc;
}

static void msCompilerAbortFunc(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  MS_ASSERT(fn != NULL);
  c->fn = fn->parent;
  msProtoBuilderDestroy(&fn->builder);
  msFree(fn);
}

static struct MsFuncCtx* msCompilerPushFunc(struct MsCompiler* c, const struct MsAst* funcNode) {
  struct MsFuncCtx* fn = msAlloc(sizeof(*fn));
  if (fn == NULL) {
    msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
    return NULL;
  }
  memset(fn, 0, sizeof(*fn));
  fn->parent = c->fn;
  fn->funcNode = funcNode;
  fn->currentNode = funcNode;
  fn->isModule = funcNode == NULL;
  fn->scopes[0].discardSlot = -1;
  msProtoBuilderInit(&fn->builder);
  c->fn = fn;
  const char* name = "<main>";
  struct MsString* protoName = msStringFromCStr(c->heap, name);
  if (protoName == NULL) {
    msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
  } else {
    fn->builder.proto.name = protoName;
  }
  if (c->sourceFile != NULL) {
    fn->builder.proto.sourceFile = msStringFromCStr(c->heap, c->sourceFile);
    if (fn->builder.proto.sourceFile == NULL) {
      msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
    }
  }
  if (c->sticky != MS_OK) {
    msCompilerAbortFunc(c);
    return NULL;
  }
  return fn;
}

static MsResult msCompilerPopFunc(struct MsCompiler* c, struct MsProto** out) {
  struct MsFuncCtx* fn = c->fn;
  struct MsProto* proto = NULL;
  *out = NULL;
  if (c->sticky == MS_OK) {
    int codeLen = fn->builder.proto.codeLen;
    if (codeLen == 0
        || msOpDecodeOp(fn->builder.proto.code[codeLen - 1]) != MS_OP_RETURN) {
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      msCompilerEmitABC(c, MS_OP_RETURN, 0, 0, -1);
    }
    fn->builder.proto.localCount = fn->localMax;
    fn->builder.proto.stackSize = fn->stackMax;
    MsResult result = msProtoBuild(&fn->builder, &proto);
    if (result != MS_OK) {
      msCompilerSyncBuilder(c);
    }
  }
  if (c->sticky == MS_OK) {
    c->fn = fn->parent;
    msFree(fn);
    *out = proto;
    return MS_OK;
  }
  MsResult result = c->sticky;
  msCompilerAbortFunc(c);
  return result;
}

void msCompilerInit(struct MsCompiler* c, struct MsHeap* heap, struct MsDiagList* diags,
    const char* sourceFile, uint32_t flags) {
  c->heap = heap;
  c->diags = diags;
  c->sourceFile = sourceFile;
  c->flags = flags;
  c->fn = NULL;
  c->nodeDepth = 0;
  c->sticky = MS_OK;
}

void msCompilerDestroy(struct MsCompiler* c) {
  if (c == NULL) {
    return;
  }
  while (c->fn != NULL) {
    msCompilerAbortFunc(c);
  }
  c->heap = NULL;
  c->diags = NULL;
  c->sourceFile = NULL;
}

MsResult msCompilerCompile(struct MsCompiler* c, const struct MsAst* root, struct MsProto** out) {
  if (out == NULL) {
    return MS_ERROR_SYNTAX;
  }
  *out = NULL;
  if (c->fn != NULL || c->sticky != MS_OK) {
    return c->sticky != MS_OK ? c->sticky : MS_ERROR_SYNTAX;
  }
  size_t diagStart = msDiagListCount(c->diags);
  if (msCompilerPushFunc(c, NULL) == NULL) {
    return c->sticky;
  }
  if (root == NULL || root->kind != MS_AST_SOURCE_FILE) {
    msCompilerReport(c, root, 307, "expected a source-file AST");
  } else if (root->as.block.count != 0) {
    msCompilerReport(c, root->as.block.items[0], 307, "statement compilation is not implemented");
  }
  if (c->sticky == MS_OK && msDiagListCount(c->diags) == diagStart) {
    return msCompilerPopFunc(c, out);
  }
  MsResult result = c->sticky != MS_OK ? c->sticky : MS_ERROR_SYNTAX;
  msCompilerAbortFunc(c);
  return result;
}
