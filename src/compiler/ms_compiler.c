#include "compiler/ms_compiler.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
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

static bool msCompilerEnterNode(struct MsCompiler* c, const struct MsAst* node) {
  if (c->sticky != MS_OK) {
    return false;
  }
  if (node == NULL) {
    msCompilerReport(c, NULL, 307, "missing AST node");
    return false;
  }
  ++c->nodeDepth;
  if (c->nodeDepth > MS_COMPILER_MAX_NODE_DEPTH) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 311, "AST nesting depth exceeded");
    --c->nodeDepth;
    return false;
  }
  c->fn->currentNode = node;
  msProtoSetLine(&c->fn->builder, node->line);
  return true;
}

static int msCompilerAllocHiddenLocal(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->localCount >= MS_COMPILER_MAX_LOCALS) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "too many local slots");
    return -1;
  }
  int slot = fn->localCount++;
  fn->locals[slot].name = NULL;
  fn->locals[slot].depth = fn->scopeDepth;
  fn->locals[slot].isCaptured = false;
  if (fn->localCount > fn->localMax) {
    fn->localMax = fn->localCount;
  }
  return slot;
}

static int msCompilerDiscardSlot(struct MsCompiler* c) {
  struct MsScope* scope = &c->fn->scopes[c->fn->scopeDepth];
  if (scope->discardSlot < 0) {
    scope->discardSlot = msCompilerAllocHiddenLocal(c);
  }
  return scope->discardSlot;
}

static char* msCompilerCleanNumber(struct MsCompiler* c, const struct MsAst* node) {
  size_t len = node->as.literal.length;
  char* clean = msAlloc(len + 1);
  if (clean == NULL) {
    msCompilerFail(c, node, MS_ERROR_OOM, 0, "out of memory");
    return NULL;
  }
  size_t out = 0;
  for (size_t i = 0; i < len; ++i) {
    if (node->as.literal.start[i] != '_') {
      clean[out++] = node->as.literal.start[i];
    }
  }
  clean[out] = '\0';
  return clean;
}

static bool msCompilerParseInt(struct MsCompiler* c, const struct MsAst* node,
    bool negate, int64_t* out) {
  char* clean = msCompilerCleanNumber(c, node);
  if (clean == NULL) {
    return false;
  }
  int base = 10;
  char* digits = clean;
  if (clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
    base = 16;
    digits += 2;
  } else if (clean[0] == '0' && (clean[1] == 'o' || clean[1] == 'O')) {
    base = 8;
    digits += 2;
  } else if (clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B')) {
    base = 2;
    digits += 2;
  }
  errno = 0;
  char* end = NULL;
  unsigned long long magnitude = strtoull(digits, &end, base);
  unsigned long long limit = negate ? (unsigned long long)INT64_MAX + 1ULL
                                    : (unsigned long long)INT64_MAX;
  bool ok = end != digits && end != NULL && *end == '\0' && errno != ERANGE && magnitude <= limit;
  if (ok) {
    if (negate && magnitude == (unsigned long long)INT64_MAX + 1ULL) {
      *out = INT64_MIN;
    } else if (negate) {
      *out = -(int64_t)magnitude;
    } else {
      *out = (int64_t)magnitude;
    }
  }
  msFree(clean);
  return ok;
}

static bool msCompilerParseFloat(struct MsCompiler* c, const struct MsAst* node, double* out) {
  char* clean = msCompilerCleanNumber(c, node);
  if (clean == NULL) {
    return false;
  }
  errno = 0;
  char* end = NULL;
  double value = strtod(clean, &end);
  bool ok = end != clean && end != NULL && *end == '\0' && errno != ERANGE;
  if (ok) {
    *out = value;
  }
  msFree(clean);
  return ok;
}

static void msCompilerEmitConstObject(struct MsCompiler* c, struct MsObject* value) {
  if (c->sticky != MS_OK) {
    return;
  }
  if (value == NULL) {
    msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
    return;
  }
  int index = msProtoAddConst(&c->fn->builder, value);
  if (index < 0) {
    msCompilerSyncBuilder(c);
    return;
  }
  msCompilerEmitABC(c, MS_OP_LOAD_CONST, 0, (uint16_t)index, 1);
}

static void msCompilerEmitIntLiteral(struct MsCompiler* c, const struct MsAst* node) {
  int64_t value;
  if (!msCompilerParseInt(c, node, false, &value)) {
    if (c->sticky == MS_OK) {
      msCompilerReport(c, node, 308, "integer literal exceeds int64");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    }
    return;
  }
  msCompilerEmitConstObject(c, (struct MsObject*)msIntNew(c->heap, value));
}

static void msCompilerEmitFloatLiteral(struct MsCompiler* c, const struct MsAst* node) {
  double value;
  if (!msCompilerParseFloat(c, node, &value)) {
    if (c->sticky == MS_OK) {
      msCompilerReport(c, node, 308, "floating literal is out of range");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    }
    return;
  }
  msCompilerEmitConstObject(c, (struct MsObject*)msFloatNew(c->heap, value));
}

static void msCompilerEmitStringLiteral(struct MsCompiler* c, const struct MsAst* node) {
  const char* body = node->as.literal.start + 1;
  size_t bodyLen = node->as.literal.length - 2;
  if (node->kind == MS_AST_RAW_STRING) {
    msCompilerEmitConstObject(c,
        (struct MsObject*)msStringNew(c->heap, body, bodyLen));
    return;
  }
  char* decoded = NULL;
  size_t decodedLen = 0;
  MsResult result = msLexerUnescape(body, bodyLen, &decoded, &decodedLen);
  if (result != MS_OK) {
    msCompilerFail(c, node, result, result == MS_ERROR_OOM ? 0 : 307,
        result == MS_ERROR_OOM ? "out of memory" : "string decoding failed");
    return;
  }
  struct MsString* value = msStringNew(c->heap, decoded, decodedLen);
  msFree(decoded);
  msCompilerEmitConstObject(c, (struct MsObject*)value);
}

static void msCompilerCompileStmt(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileExpr(struct MsCompiler* c, const struct MsAst* node);

static void msCompilerCompileExpr(struct MsCompiler* c, const struct MsAst* node) {
  if (!msCompilerEnterNode(c, node)) {
    return;
  }
  switch (node->kind) {
    case MS_AST_INT:
      msCompilerEmitIntLiteral(c, node);
      break;
    case MS_AST_FLOAT:
      msCompilerEmitFloatLiteral(c, node);
      break;
    case MS_AST_STRING:
    case MS_AST_RAW_STRING:
      msCompilerEmitStringLiteral(c, node);
      break;
    case MS_AST_BOOL:
      msCompilerEmitABC(c, node->as.boolLit.value ? MS_OP_LOAD_TRUE : MS_OP_LOAD_FALSE, 0, 0, 1);
      break;
    case MS_AST_NIL:
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
    default:
      msCompilerReport(c, node, 307, "expression construct is not implemented in task 07");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
  }
  --c->nodeDepth;
}

static void msCompilerCompileStmt(struct MsCompiler* c, const struct MsAst* node) {
  if (!msCompilerEnterNode(c, node)) {
    return;
  }
  switch (node->kind) {
    case MS_AST_SOURCE_FILE:
      for (size_t i = 0; i < node->as.block.count && c->sticky == MS_OK; ++i) {
        msCompilerCompileStmt(c, node->as.block.items[i]);
      }
      break;
    case MS_AST_EXPR_STMT:
      msCompilerCompileExpr(c, node->as.exprStmt.expr);
      if (c->sticky == MS_OK) {
        int slot = msCompilerDiscardSlot(c);
        if (slot >= 0) {
          msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
        }
      }
      break;
    case MS_AST_PASS:
      break;
    default:
      msCompilerReport(c, node, 307, "statement construct is not implemented in task 07");
      break;
  }
  --c->nodeDepth;
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
  } else {
    msCompilerCompileStmt(c, root);
  }
  if (c->sticky == MS_OK && msDiagListCount(c->diags) == diagStart) {
    return msCompilerPopFunc(c, out);
  }
  MsResult result = c->sticky != MS_OK ? c->sticky : MS_ERROR_SYNTAX;
  msCompilerAbortFunc(c);
  return result;
}
