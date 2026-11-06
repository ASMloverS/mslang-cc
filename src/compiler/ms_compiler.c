#include "compiler/ms_compiler.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/ms_scope.h"
#include "core/ms_common.h"
#include "core/ms_memory.h"
#include "lexer/ms_lexer.h"
#include "object/ms_object.h"
#include "object/ms_string.h"
#include "parser/ms_ast.h"
#include "vm/ms_opcode.h"
#include "vm/ms_proto.h"
#include "vm/ms_state_internal.h"

#define MS_COMPILER_MAX_NODE_DEPTH 512

struct MsGlobalDecl {
  struct MsGlobalDecl* next;
  struct MsString* name;
};

struct MsFuncCtx {
  struct MsFuncCtx* parent;
  struct MsProtoBuilder builder;
  const struct MsAst* funcNode;
  const struct MsAst* currentNode;
  struct MsScopeCtx scope;
  struct MsGlobalDecl* pendingGlobalDecls;
  int stackDepth;
  int stackMax;
  bool isModule;
  bool isAsync;
  bool replPrint;
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

static int msCompilerCurrentPc(const struct MsCompiler* c) {
  return c->fn->builder.proto.codeLen;
}

static int msCompilerEmitJumpPlaceholder(struct MsCompiler* c, MsOpCode op) {
  if (c->sticky != MS_OK) {
    return -1;
  }
  int pc = msProtoEmitSAx(&c->fn->builder, op, 0);
  if (pc < 0) {
    msCompilerSyncBuilder(c);
    return -1;
  }
  c->fn->stackDepth += op == MS_OP_JUMP ? 0 : -1;
  MS_ASSERT(c->fn->stackDepth >= 0);
  return pc;
}

static void msCompilerPatchToCurrent(struct MsCompiler* c, int jumpPc, int expectedStackDepth) {
  if (jumpPc < 0 || c->sticky != MS_OK) {
    return;
  }
  struct MsFuncCtx* fn = c->fn;
  MS_ASSERT(fn->stackDepth == expectedStackDepth);
  MS_UNUSED(expectedStackDepth);
  int target = msCompilerCurrentPc(c);
  uint32_t* instruction = &fn->builder.proto.code[jumpPc];
  if (msOpDecodeOp(*instruction) == MS_OP_JUMP && target == jumpPc + 1) {
    *instruction = msOpEncodeABC(MS_OP_NOP, 0, 0);
    return;
  }
  MsResult result = msProtoPatchSAx(&fn->builder, jumpPc, target - (jumpPc + 1));
  if (result != MS_OK) {
    msCompilerSyncBuilder(c);
  }
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
  if (fn->scope.localCount >= MS_SCOPE_MAX_LOCALS) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "too many local slots");
    return -1;
  }
  return msScopeAllocHiddenLocal(&fn->scope);
}

static int msCompilerDiscardSlot(struct MsCompiler* c) {
  struct MsScopeBlock* scope = &c->fn->scope.blocks[c->fn->scope.scopeDepth];
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
static void msCompilerCompileIf(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileFor(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileAugAssign(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileReturn(struct MsCompiler* c, const struct MsAst* node);
static void msCompilerCompileFunction(struct MsCompiler* c, const struct MsAst* node);

static bool msCompilerIntAddOverflows(int64_t a, int64_t b) {
  return (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
}

static bool msCompilerIntSubOverflows(int64_t a, int64_t b) {
  return (b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b);
}

static bool msCompilerIntMulOverflows(int64_t a, int64_t b) {
  if (a > 0) {
    if (b > 0) {
      return a > INT64_MAX / b;
    }
    return b < INT64_MIN / a;
  }
  if (b > 0) {
    return a < INT64_MIN / b;
  }
  return a != 0 && b < INT64_MAX / a;
}

typedef enum {
  MS_CONST_INT,
  MS_CONST_FLOAT,
  MS_CONST_BOOL,
  MS_CONST_NIL
} MsConstKind;

struct MsConstValue {
  MsConstKind kind;
  int64_t intValue;
  double floatValue;
  bool boolValue;
};

static bool msCompilerConstTruthy(const struct MsConstValue* value) {
  switch (value->kind) {
    case MS_CONST_INT:
      return value->intValue != 0;
    case MS_CONST_FLOAT:
      return value->floatValue != 0.0;
    case MS_CONST_BOOL:
      return value->boolValue;
    case MS_CONST_NIL:
      return false;
    default:
      MS_UNREACHABLE();
#ifndef NDEBUG
      // Debug MS_UNREACHABLE is a plain assert: MSVC still needs a return here
      // (C4715). In release __assume(0) makes it unreachable code (C4702).
      return false;
#endif
  }
}

static bool msCompilerEvalConst(struct MsCompiler* c, const struct MsAst* node,
    int depth, struct MsConstValue* out) {
  if (node == NULL || c->sticky != MS_OK) {
    return false;
  }
  if (depth > 0 && c->nodeDepth + depth > MS_COMPILER_MAX_NODE_DEPTH) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 311, "AST nesting depth exceeded");
    return false;
  }
  if (node->kind == MS_AST_INT) {
    out->kind = MS_CONST_INT;
    return msCompilerParseInt(c, node, false, &out->intValue);
  }
  if (node->kind == MS_AST_FLOAT) {
    out->kind = MS_CONST_FLOAT;
    return msCompilerParseFloat(c, node, &out->floatValue);
  }
  if (node->kind == MS_AST_BOOL) {
    out->kind = MS_CONST_BOOL;
    out->boolValue = node->as.boolLit.value;
    return true;
  }
  if (node->kind == MS_AST_NIL) {
    out->kind = MS_CONST_NIL;
    return true;
  }
  if (node->kind == MS_AST_UNARY) {
    if (node->as.unary.op == MS_TOKEN_MINUS && node->as.unary.operand->kind == MS_AST_INT) {
      if (c->nodeDepth + depth + 1 > MS_COMPILER_MAX_NODE_DEPTH) {
        msCompilerFail(c, node->as.unary.operand, MS_ERROR_SYNTAX, 311, "AST nesting depth exceeded");
        return false;
      }
      out->kind = MS_CONST_INT;
      return msCompilerParseInt(c, node->as.unary.operand, true, &out->intValue);
    }
    struct MsConstValue operand;
    if (!msCompilerEvalConst(c, node->as.unary.operand, depth + 1, &operand)) {
      return false;
    }
    if (node->as.unary.op == MS_TOKEN_KW_NOT) {
      out->kind = MS_CONST_BOOL;
      out->boolValue = !msCompilerConstTruthy(&operand);
      return true;
    }
    if (operand.kind != MS_CONST_INT && operand.kind != MS_CONST_FLOAT) {
      return false;
    }
    if (node->as.unary.op == MS_TOKEN_PLUS) {
      *out = operand;
      if (operand.kind == MS_CONST_FLOAT) {
        out->floatValue = 0.0 + operand.floatValue;
      }
      return true;
    }
    if (node->as.unary.op == MS_TOKEN_TILDE) {
      if (operand.kind != MS_CONST_INT) {
        return false;
      }
      out->kind = MS_CONST_INT;
      out->intValue = ~operand.intValue;
      return true;
    }
    if (node->as.unary.op != MS_TOKEN_MINUS) {
      return false;
    }
    if (operand.kind == MS_CONST_FLOAT) {
      out->kind = MS_CONST_FLOAT;
      out->floatValue = -operand.floatValue;
      return true;
    }
    if (operand.intValue == INT64_MIN) {
      return false;
    }
    out->kind = MS_CONST_INT;
    out->intValue = -operand.intValue;
    return true;
  }
  if (node->kind != MS_AST_BINARY
      || (node->as.binary.op != MS_TOKEN_PLUS && node->as.binary.op != MS_TOKEN_MINUS
          && node->as.binary.op != MS_TOKEN_STAR)) {
    return false;
  }
  struct MsConstValue left;
  struct MsConstValue right;
  if (!msCompilerEvalConst(c, node->as.binary.left, depth + 1, &left)
      || !msCompilerEvalConst(c, node->as.binary.right, depth + 1, &right)) {
    return false;
  }
  if ((left.kind != MS_CONST_INT && left.kind != MS_CONST_FLOAT)
      || (right.kind != MS_CONST_INT && right.kind != MS_CONST_FLOAT)) {
    return false;
  }
  if (left.kind == MS_CONST_INT && right.kind == MS_CONST_INT) {
    bool overflow = node->as.binary.op == MS_TOKEN_PLUS
        ? msCompilerIntAddOverflows(left.intValue, right.intValue)
        : node->as.binary.op == MS_TOKEN_MINUS
            ? msCompilerIntSubOverflows(left.intValue, right.intValue)
            : msCompilerIntMulOverflows(left.intValue, right.intValue);
    if (overflow) {
      return false;
    }
    out->kind = MS_CONST_INT;
    out->intValue = node->as.binary.op == MS_TOKEN_PLUS ? left.intValue + right.intValue
        : node->as.binary.op == MS_TOKEN_MINUS ? left.intValue - right.intValue
                                               : left.intValue * right.intValue;
    return true;
  }
  double a = left.kind == MS_CONST_INT ? (double)left.intValue : left.floatValue;
  double b = right.kind == MS_CONST_INT ? (double)right.intValue : right.floatValue;
  out->kind = MS_CONST_FLOAT;
  out->floatValue = node->as.binary.op == MS_TOKEN_PLUS ? a + b
      : node->as.binary.op == MS_TOKEN_MINUS ? a - b : a * b;
  return true;
}

static void msCompilerEmitConst(struct MsCompiler* c, const struct MsConstValue* value) {
  switch (value->kind) {
    case MS_CONST_INT:
      msCompilerEmitConstObject(c, (struct MsObject*)msIntNew(c->heap, value->intValue));
      break;
    case MS_CONST_FLOAT:
      msCompilerEmitConstObject(c, (struct MsObject*)msFloatNew(c->heap, value->floatValue));
      break;
    case MS_CONST_BOOL:
      msCompilerEmitABC(c, value->boolValue ? MS_OP_LOAD_TRUE : MS_OP_LOAD_FALSE, 0, 0, 1);
      break;
    case MS_CONST_NIL:
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
    default:
      MS_UNREACHABLE();
  }
}

static void msCompilerCompileLogical(struct MsCompiler* c, const struct MsAst* node) {
  struct MsConstValue constant;
  if (msCompilerEvalConst(c, node->as.binary.left, 0, &constant)) {
    bool isAnd = node->as.binary.op == MS_TOKEN_KW_AND;
    bool truthy = msCompilerConstTruthy(&constant);
    if ((isAnd && !truthy) || (!isAnd && truthy)) {
      msCompilerEmitConst(c, &constant);
    } else {
      msCompilerCompileExpr(c, node->as.binary.right);
    }
    return;
  }
  if (c->sticky != MS_OK) {
    return;
  }
  int baseDepth = c->fn->stackDepth;
  int savedLocals = c->fn->scope.localCount;
  int slot = msCompilerAllocHiddenLocal(c);
  if (slot < 0) {
    c->fn->scope.localCount = savedLocals;
    c->fn->stackDepth = baseDepth;
    return;
  }
  msCompilerCompileExpr(c, node->as.binary.left);
  msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
  MsOpCode jumpOp = node->as.binary.op == MS_TOKEN_KW_AND ? MS_OP_JUMP_IF_FALSE : MS_OP_JUMP_IF_TRUE;
  int jump = msCompilerEmitJumpPlaceholder(c, jumpOp);
  if (c->sticky != MS_OK) {
    c->fn->scope.localCount = savedLocals;
    c->fn->stackDepth = baseDepth;
    return;
  }
  int branchDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.binary.right);
  msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  if (c->sticky != MS_OK) {
    c->fn->scope.localCount = savedLocals;
    c->fn->stackDepth = baseDepth;
    return;
  }
  msCompilerPatchToCurrent(c, jump, branchDepth);
  if (c->sticky != MS_OK) {
    c->fn->scope.localCount = savedLocals;
    c->fn->stackDepth = baseDepth;
    return;
  }
  msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
  if (c->sticky != MS_OK) {
    c->fn->scope.localCount = savedLocals;
    c->fn->stackDepth = baseDepth;
    return;
  }
  c->fn->scope.localCount = savedLocals;
}

static void msCompilerCompileConditional(struct MsCompiler* c, const struct MsAst* node) {
  struct MsConstValue condition;
  if (msCompilerEvalConst(c, node->as.conditional.cond, 0, &condition)) {
    if (msCompilerConstTruthy(&condition)) {
      msCompilerCompileExpr(c, node->as.conditional.value);
    } else {
      msCompilerCompileExpr(c, node->as.conditional.elseValue);
    }
    return;
  }
  if (c->sticky != MS_OK) {
    return;
  }
  int baseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.conditional.cond);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
    return;
  }
  int elseJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
    return;
  }
  int elseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.conditional.value);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
    return;
  }
  int endJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
    return;
  }
  int endDepth = c->fn->stackDepth;
  c->fn->stackDepth = elseDepth;
  msCompilerPatchToCurrent(c, elseJump, elseDepth);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
    return;
  }
  msCompilerCompileExpr(c, node->as.conditional.elseValue);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
    return;
  }
  MS_ASSERT(c->fn->stackDepth == endDepth);
  msCompilerPatchToCurrent(c, endJump, endDepth);
  if (c->sticky != MS_OK) {
    c->fn->stackDepth = baseDepth;
  }
}

static void msCompilerCompileComparison(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.compareChain.opCount != 1 || node->as.compareChain.operands.count != 2) {
    msCompilerReport(c, node, 307, "comparison chains belong to task 12");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  msCompilerCompileExpr(c, node->as.compareChain.operands.items[0]);
  msCompilerCompileExpr(c, node->as.compareChain.operands.items[1]);
  MsCompareOp compare = node->as.compareChain.ops[0];
  MsOpCode op = MS_OP_COUNT;
  switch (compare) {
    case MS_CMP_LT: op = MS_OP_LT; break;
    case MS_CMP_LE: op = MS_OP_LE; break;
    case MS_CMP_GT: op = MS_OP_GT; break;
    case MS_CMP_GE: op = MS_OP_GE; break;
    case MS_CMP_EQ: op = MS_OP_EQ; break;
    case MS_CMP_NE: op = MS_OP_NE; break;
    case MS_CMP_IN:
    case MS_CMP_NOT_IN: op = MS_OP_IN; break;
    case MS_CMP_IS:
    case MS_CMP_IS_NOT: op = MS_OP_IS; break;
    default: break;
  }
  if (op == MS_OP_COUNT) {
    msCompilerReport(c, node, 307, "comparison operator is not implemented");
    return;
  }
  msCompilerEmitABC(c, op, 0, 0, -1);
  if (compare == MS_CMP_NOT_IN || compare == MS_CMP_IS_NOT) {
    msCompilerEmitABC(c, MS_OP_NOT, 0, 0, 0);
  }
}

static void msCompilerCompileCall(struct MsCompiler* c, const struct MsAst* node) {
  size_t count = node->as.call.args.count;
  if (count > 255) {
    msCompilerReport(c, node, 312, "more than 255 positional arguments");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    if (node->as.call.args.items[i]->kind == MS_AST_KW_ARG) {
      msCompilerReport(c, node, 307, "keyword calls belong to task 13");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      return;
    }
  }
  msCompilerCompileExpr(c, node->as.call.callee);
  for (size_t i = 0; i < count; ++i) {
    msCompilerCompileExpr(c, node->as.call.args.items[i]);
  }
  msCompilerEmitABC(c, MS_OP_CALL, (uint8_t)count, 0, -(int)count);
}

static MsOpCode msCompilerBinaryOp(MsTokenType token) {
  switch (token) {
    case MS_TOKEN_PLUS:
    case MS_TOKEN_PLUS_EQUAL: return MS_OP_ADD;
    case MS_TOKEN_MINUS:
    case MS_TOKEN_MINUS_EQUAL: return MS_OP_SUB;
    case MS_TOKEN_STAR:
    case MS_TOKEN_STAR_EQUAL: return MS_OP_MUL;
    case MS_TOKEN_SLASH:
    case MS_TOKEN_SLASH_EQUAL: return MS_OP_DIV;
    case MS_TOKEN_KW_DIV: return MS_OP_FLOORDIV;
    case MS_TOKEN_PERCENT:
    case MS_TOKEN_PERCENT_EQUAL: return MS_OP_MOD;
    case MS_TOKEN_DOUBLE_STAR:
    case MS_TOKEN_DOUBLE_STAR_EQUAL: return MS_OP_POW;
    case MS_TOKEN_SHIFT_LEFT:
    case MS_TOKEN_SHIFT_LEFT_EQUAL: return MS_OP_SHL;
    case MS_TOKEN_SHIFT_RIGHT:
    case MS_TOKEN_SHIFT_RIGHT_EQUAL: return MS_OP_SHR;
    case MS_TOKEN_AMP:
    case MS_TOKEN_AMP_EQUAL: return MS_OP_BITAND;
    case MS_TOKEN_PIPE:
    case MS_TOKEN_PIPE_EQUAL: return MS_OP_BITOR;
    case MS_TOKEN_CARET:
    case MS_TOKEN_CARET_EQUAL: return MS_OP_BITXOR;
    default: return MS_OP_COUNT;
  }
}

static void msCompilerCompileBinary(struct MsCompiler* c, const struct MsAst* node) {
  MsOpCode op = msCompilerBinaryOp(node->as.binary.op);
  if (op == MS_OP_COUNT) {
    msCompilerReport(c, node, 307, "binary operator is not implemented in task 07");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  msCompilerCompileExpr(c, node->as.binary.left);
  msCompilerCompileExpr(c, node->as.binary.right);
  msCompilerEmitABC(c, op, 0, 0, -1);
}

static void msCompilerCompileUnary(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.unary.op == MS_TOKEN_PLUS) {
    struct MsConstValue zero = {.kind = MS_CONST_INT, .intValue = 0};
    msCompilerEmitConst(c, &zero);
    msCompilerCompileExpr(c, node->as.unary.operand);
    msCompilerEmitABC(c, MS_OP_ADD, 0, 0, -1);
    return;
  }
  msCompilerCompileExpr(c, node->as.unary.operand);
  switch (node->as.unary.op) {
    case MS_TOKEN_MINUS:
      msCompilerEmitABC(c, MS_OP_NEG, 0, 0, 0);
      break;
    case MS_TOKEN_TILDE:
      msCompilerEmitABC(c, MS_OP_INVERT, 0, 0, 0);
      break;
    case MS_TOKEN_KW_NOT:
      msCompilerEmitABC(c, MS_OP_NOT, 0, 0, 0);
      break;
    default:
      msCompilerReport(c, node, 307, "unary operator is not implemented in task 07");
      break;
  }
}

typedef enum {
  MS_RESOLVE_LOCAL,
  MS_RESOLVE_UPVALUE,
  MS_RESOLVE_GLOBAL
} MsResolveKind;

static struct MsString* msCompilerNameFromSlice(struct MsCompiler* c, const char* start, size_t len) {
  struct MsString* name = msStringNew(c->heap, start, len);
  if (name == NULL) {
    msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
  }
  return name;
}

static bool msCompilerHasPendingGlobalDecl(const struct MsFuncCtx* fn, const struct MsString* name) {
  for (struct MsGlobalDecl* decl = fn->pendingGlobalDecls; decl != NULL; decl = decl->next) {
    if (msStringEquals(decl->name, name)) {
      return true;
    }
  }
  return false;
}

static void msCompilerAddPendingGlobalDecl(struct MsCompiler* c, struct MsString* name) {
  if (msCompilerHasPendingGlobalDecl(c->fn, name)) {
    return;
  }
  struct MsGlobalDecl* decl = msAlloc(sizeof(*decl));
  if (decl == NULL) {
    msCompilerFail(c, c->fn->currentNode, MS_ERROR_OOM, 0, "out of memory");
    return;
  }
  decl->name = name;
  decl->next = c->fn->pendingGlobalDecls;
  c->fn->pendingGlobalDecls = decl;
}

static bool msCompilerConsumePendingGlobalDecl(struct MsFuncCtx* fn, const struct MsString* name) {
  struct MsGlobalDecl** link = &fn->pendingGlobalDecls;
  while (*link != NULL) {
    struct MsGlobalDecl* decl = *link;
    if (msStringEquals(decl->name, name)) {
      *link = decl->next;
      msFree(decl);
      return true;
    }
    link = &decl->next;
  }
  return false;
}

static void msCompilerFreePendingGlobalDecls(struct MsFuncCtx* fn) {
  while (fn->pendingGlobalDecls != NULL) {
    struct MsGlobalDecl* decl = fn->pendingGlobalDecls;
    fn->pendingGlobalDecls = decl->next;
    msFree(decl);
  }
}

static int msCompilerResolveLocalInFunc(const struct MsFuncCtx* fn, const struct MsString* name) {
  return msScopeResolveLocal(&fn->scope, name);
}

static MsResolveKind msCompilerResolveName(struct MsCompiler* c, const struct MsString* name, int* slot) {
  *slot = msCompilerResolveLocalInFunc(c->fn, name);
  if (*slot >= 0) {
    return MS_RESOLVE_LOCAL;
  }
  if (msScopeIsMarkedGlobal(&c->fn->scope, name)) {
    return MS_RESOLVE_GLOBAL;
  }
  for (struct MsFuncCtx* parent = c->fn->parent; parent != NULL; parent = parent->parent) {
    if (msCompilerResolveLocalInFunc(parent, name) >= 0) {
      return MS_RESOLVE_UPVALUE;
    }
  }
  return MS_RESOLVE_GLOBAL;
}

static bool msCompilerValidateStore(struct MsCompiler* c, struct MsString* name,
    const struct MsAst* target) {
  int slot = -1;
  MsResolveKind kind = msCompilerResolveName(c, name, &slot);
  if (kind != MS_RESOLVE_GLOBAL || msScopeIsMarkedGlobal(&c->fn->scope, name)) {
    return true;
  }
  if (c->state != NULL && msStateGetGlobal(c->state, name) != NULL) {
    return true;
  }
  if (c->sticky == MS_OK) {
    uint32_t line = target != NULL ? target->line : 0;
    uint32_t column = target != NULL ? target->column : 0;
    msDiagReport(c->diags, line, column, 301, "assignment to undeclared name '%s'",
        msStringData(name));
    c->sticky = MS_ERROR_SYNTAX;
  }
  return false;
}

static int msCompilerDeclareLocal(struct MsCompiler* c, struct MsString* name) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->scope.localCount >= MS_SCOPE_MAX_LOCALS) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "too many local slots");
    return -1;
  }
  return msScopeDeclareLocal(&fn->scope, name);
}

static void msCompilerPushScope(struct MsCompiler* c) {
  struct MsFuncCtx* fn = c->fn;
  if (fn->scope.scopeDepth + 1 >= MS_SCOPE_MAX_DEPTH) {
    msCompilerFail(c, fn->currentNode, MS_ERROR_SYNTAX, 306, "block nesting depth exceeded");
    return;
  }
  msScopeBeginBlock(&fn->scope);
}

static void msCompilerPopScope(struct MsCompiler* c) {
  msScopeEndBlock(&c->fn->scope);
}

static void msCompilerEmitLoadName(struct MsCompiler* c, struct MsString* name) {
  int slot = -1;
  MsResolveKind kind = msCompilerResolveName(c, name, &slot);
  if (kind == MS_RESOLVE_LOCAL) {
    msCompilerEmitABC(c, MS_OP_LOAD_LOCAL, (uint8_t)slot, 0, 1);
    return;
  }
  if (kind == MS_RESOLVE_UPVALUE) {
    msCompilerReport(c, c->fn->currentNode, 310, "upvalue capture belongs to task 14");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
  if (index < 0) {
    msCompilerSyncBuilder(c);
    return;
  }
  msCompilerEmitABC(c, MS_OP_LOAD_GLOBAL, 0, (uint16_t)index, 1);
}

static void msCompilerEmitStoreName(struct MsCompiler* c, struct MsString* name) {
  int slot = -1;
  MsResolveKind kind = msCompilerResolveName(c, name, &slot);
  if (kind == MS_RESOLVE_LOCAL) {
    msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
    return;
  }
  if (kind == MS_RESOLVE_UPVALUE) {
    msCompilerReport(c, c->fn->currentNode, 310, "upvalue capture belongs to task 14");
    int saved = c->fn->scope.localCount;
    int discard = msCompilerAllocHiddenLocal(c);
    if (discard >= 0) {
      msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)discard, 0, -1);
    }
    c->fn->scope.localCount = saved;
    return;
  }
  int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
  if (index < 0) {
    msCompilerSyncBuilder(c);
    return;
  }
  msCompilerEmitABC(c, MS_OP_STORE_GLOBAL, 0, (uint16_t)index, -1);
}

static const struct MsAst* msCompilerTargetIdent(const struct MsAst* target) {
  if (target->kind == MS_AST_UNARY && target->as.unary.op == MS_TOKEN_STAR) {
    target = target->as.unary.operand;
  }
  return target->kind == MS_AST_IDENT ? target : NULL;
}

static int msCompilerStarTarget(const struct MsAstList* targets) {
  for (size_t i = 0; i < targets->count; ++i) {
    const struct MsAst* target = targets->items[i];
    if (target->kind == MS_AST_UNARY && target->as.unary.op == MS_TOKEN_STAR) {
      return (int)i;
    }
  }
  return -1;
}

static void msCompilerCompileDeclOrAssign(struct MsCompiler* c, const struct MsAst* node) {
  const struct MsAstList* targets = &node->as.decl.targets;
  const struct MsAstList* values = &node->as.decl.values;
  int starIndex = msCompilerStarTarget(targets);
  if (starIndex >= 0 && targets->count < 2) {
    msCompilerReport(c, node, 307, "unpacking is not implemented yet");
    return;
  }
  for (size_t i = 0; i < targets->count; ++i) {
    if (msCompilerTargetIdent(targets->items[i]) == NULL) {
      msCompilerReport(c, targets->items[i], 301, "indexed/attribute assignment belongs to task 15/16");
      return;
    }
  }
  if (values->count > 1
      && ((starIndex < 0 && values->count != targets->count)
          || (starIndex >= 0 && values->count + 1 < targets->count))) {
    msDiagReport(c->diags, node->line, node->column, 303, "cannot assign %zu values to %zu targets",
        values->count, targets->count);
    c->sticky = MS_ERROR_SYNTAX;
    return;
  }
  if (values->count == 1 && targets->count > 1) {
    if (targets->count > 255) {
      msCompilerFail(c, node, MS_ERROR_SYNTAX, 306, "too many assignment targets");
      return;
    }
    msCompilerCompileExpr(c, values->items[0]);
    msCompilerEmitABC(c, MS_OP_UNPACK, (uint8_t)targets->count,
        starIndex < 0 ? MS_UNPACK_NO_STAR : (uint16_t)starIndex, (int)targets->count - 1);
    if (c->sticky == MS_OK && c->fn->stackMax < c->fn->stackDepth + 1) {
      c->fn->stackMax = c->fn->stackDepth + 1;
    }
  } else if (starIndex >= 0) {
    size_t middleCount = values->count - targets->count + 1;
    if (middleCount > 255) {
      msCompilerReport(c, node, 312, "more than 255 container items");
      return;
    }
    for (int i = 0; i < starIndex; ++i) {
      msCompilerCompileExpr(c, values->items[i]);
    }
    for (size_t i = 0; i < middleCount; ++i) {
      msCompilerCompileExpr(c, values->items[starIndex + i]);
    }
    msCompilerEmitABC(c, MS_OP_BUILD_LIST, (uint8_t)middleCount, 0, 1 - (int)middleCount);
    for (size_t i = starIndex + middleCount; i < values->count; ++i) {
      msCompilerCompileExpr(c, values->items[i]);
    }
  } else {
    for (size_t i = 0; i < values->count; ++i) {
      msCompilerCompileExpr(c, values->items[i]);
    }
  }
  if (c->sticky != MS_OK) {
    return;
  }
  bool declaredAny = false;
  if (node->kind == MS_AST_DECL) {
    for (size_t i = 0; i < targets->count; ++i) {
      const struct MsAst* target = msCompilerTargetIdent(targets->items[i]);
      struct MsString* name = msCompilerNameFromSlice(c, target->as.literal.start, target->as.literal.length);
      if (name == NULL) {
        return;
      }
      if (c->fn->isModule && c->fn->scope.scopeDepth == 0) {
        if (!msScopeIsMarkedGlobal(&c->fn->scope, name)) {
          MsResult result = msScopeMarkGlobal(&c->fn->scope, name);
          if (result != MS_OK) {
            msCompilerFail(c, node, result, 0, "out of memory");
            return;
          }
          declaredAny = true;
        } else if (msCompilerConsumePendingGlobalDecl(c->fn, name)) {
          declaredAny = true;
        }
        continue;
      }
      if (!c->fn->isModule && msScopeIsMarkedGlobal(&c->fn->scope, name)) {
        msCompilerFail(c, node, MS_ERROR_SYNTAX, 304, "name is local before global declaration");
        return;
      }
      if (!msScopeHasLocalAtDepth(&c->fn->scope, name)) {
        int slot = msCompilerDeclareLocal(c, name);
        if (slot < 0) {
          return;
        }
        declaredAny = true;
      }
    }
  }
  for (size_t i = targets->count; i > 0; --i) {
    const struct MsAst* target = msCompilerTargetIdent(targets->items[i - 1]);
    struct MsString* name = msCompilerNameFromSlice(c, target->as.literal.start, target->as.literal.length);
    if (name == NULL) {
      return;
    }
    if (node->kind == MS_AST_ASSIGN && !msCompilerValidateStore(c, name, target)) {
      return;
    }
    msCompilerEmitStoreName(c, name);
    if (c->sticky != MS_OK) {
      return;
    }
  }
  if (node->kind == MS_AST_DECL && !declaredAny) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 302, "no new variables on left side of :=");
  }
}

static void msCompilerCompileAugAssign(struct MsCompiler* c, const struct MsAst* node) {
  const struct MsAst* target = node->as.augAssign.target;
  if (target == NULL || target->kind != MS_AST_IDENT) {
    msCompilerReport(c, node, 307, "non-identifier augmented assignment belongs to task 11");
    return;
  }
  MsOpCode op = msCompilerBinaryOp(node->as.augAssign.op);
  if (op == MS_OP_COUNT) {
    msCompilerReport(c, node, 307, "augmented assignment operator is not implemented");
    return;
  }
  struct MsString* name = msCompilerNameFromSlice(c, target->as.literal.start, target->as.literal.length);
  if (name == NULL || !msCompilerValidateStore(c, name, target)) {
    return;
  }
  msCompilerEmitLoadName(c, name);
  msCompilerCompileExpr(c, node->as.augAssign.value);
  msCompilerEmitABC(c, op, 0, 0, -1);
  msCompilerEmitStoreName(c, name);
}

static void msCompilerCompileContainer(struct MsCompiler* c, const struct MsAst* node) {
  bool isDict = node->kind == MS_AST_DICT_LIT;
  size_t count = node->as.block.count;
  MS_ASSERT(!isDict || count % 2 == 0);
  size_t items = isDict ? count / 2 : count;
  if (items > 255) {
    msCompilerReport(c, node, 312, "more than 255 container items");
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    msCompilerCompileExpr(c, node->as.block.items[i]);
  }
  msCompilerEmitABC(c, isDict ? MS_OP_BUILD_DICT : MS_OP_BUILD_LIST, (uint8_t)items, 0, 1 - (int)count);
}

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
    case MS_AST_LIST_LIT:
    case MS_AST_DICT_LIT:
      msCompilerCompileContainer(c, node);
      break;
    case MS_AST_IDENT: {
      struct MsString* name = msCompilerNameFromSlice(c, node->as.literal.start, node->as.literal.length);
      if (name != NULL) {
        msCompilerEmitLoadName(c, name);
      }
      break;
    }
    case MS_AST_UNARY: {
      struct MsConstValue folded;
      if (msCompilerEvalConst(c, node, 0, &folded)) {
        msCompilerEmitConst(c, &folded);
      } else if (c->sticky == MS_OK) {
        msCompilerCompileUnary(c, node);
      }
      break;
    }
    case MS_AST_BINARY: {
      struct MsConstValue folded;
      if (msCompilerEvalConst(c, node, 0, &folded)) {
        msCompilerEmitConst(c, &folded);
      } else if (c->sticky == MS_OK) {
        msCompilerCompileBinary(c, node);
      }
      break;
    }
    case MS_AST_LOGICAL:
      msCompilerCompileLogical(c, node);
      break;
    case MS_AST_CONDITIONAL:
      msCompilerCompileConditional(c, node);
      break;
    case MS_AST_COMPARE_CHAIN:
      msCompilerCompileComparison(c, node);
      break;
    case MS_AST_CALL:
      msCompilerCompileCall(c, node);
      break;
    case MS_AST_LAMBDA:
      msCompilerCompileFunction(c, node);
      break;
    default:
      msCompilerReport(c, node, 307, "expression construct is not implemented in task 07");
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
      break;
  }
  --c->nodeDepth;
}

static void msCompilerCompileIf(struct MsCompiler* c, const struct MsAst* node) {
  int baseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.ifStmt.cond);
  int elseJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  msCompilerCompileStmt(c, node->as.ifStmt.thenBlock);
  if (node->as.ifStmt.elseBranch != NULL) {
    int endJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
    msCompilerPatchToCurrent(c, elseJump, baseDepth);
    msCompilerCompileStmt(c, node->as.ifStmt.elseBranch);
    msCompilerPatchToCurrent(c, endJump, baseDepth);
  } else {
    msCompilerPatchToCurrent(c, elseJump, baseDepth);
  }
}

static void msCompilerCompileFor(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.forStmt.forKind != MS_FOR_CLAUSE) {
    msCompilerReport(c, node, 307, "only three-part for is implemented in task 09");
    return;
  }
  msCompilerCompileStmt(c, node->as.forStmt.init);
  int loopHead = msCompilerCurrentPc(c);
  int baseDepth = c->fn->stackDepth;
  msCompilerCompileExpr(c, node->as.forStmt.cond);
  int exitJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP_IF_FALSE);
  msCompilerCompileStmt(c, node->as.forStmt.body);
  msCompilerCompileStmt(c, node->as.forStmt.post);
  int backJump = msCompilerEmitJumpPlaceholder(c, MS_OP_JUMP);
  if (backJump >= 0 && c->sticky == MS_OK) {
    MsResult result = msProtoPatchSAx(&c->fn->builder, backJump, loopHead - (backJump + 1));
    if (result != MS_OK) {
      msCompilerSyncBuilder(c);
    }
  }
  msCompilerPatchToCurrent(c, exitJump, baseDepth);
}

static void msCompilerCompileGlobal(struct MsCompiler* c, const struct MsAst* node) {
  for (size_t i = 0; i < node->as.globalStmt.names.count && c->sticky == MS_OK; ++i) {
    const struct MsAst* item = node->as.globalStmt.names.items[i];
    struct MsString* name = msCompilerNameFromSlice(c,
        item->as.literal.start, item->as.literal.length);
    if (name == NULL) {
      return;
    }
    bool isNewModuleGlobal = c->fn->isModule && c->fn->scope.scopeDepth == 0
        && !msScopeIsMarkedGlobal(&c->fn->scope, name);
    MsResult result = msScopeMarkGlobal(&c->fn->scope, name);
    if (result == MS_ERROR_SYNTAX) {
      msCompilerFail(c, item, result, 304, "name is local before global declaration");
    } else if (result != MS_OK) {
      msCompilerFail(c, item, result, 0, "out of memory");
    } else if (isNewModuleGlobal) {
      msCompilerAddPendingGlobalDecl(c, name);
    }
  }
}

static void msCompilerCompileStmt(struct MsCompiler* c, const struct MsAst* node) {
  if (!msCompilerEnterNode(c, node)) {
    return;
  }
  switch (node->kind) {
    case MS_AST_SOURCE_FILE:
      for (size_t i = 0; i < node->as.block.count && c->sticky == MS_OK; ++i) {
        const struct MsAst* statement = node->as.block.items[i];
        c->fn->replPrint = (c->flags & MS_COMPILE_REPL) != 0
            && i + 1 == node->as.block.count && statement->kind == MS_AST_EXPR_STMT;
        msCompilerCompileStmt(c, statement);
        c->fn->replPrint = false;
      }
      break;
    case MS_AST_EXPR_STMT: {
      bool print = c->fn->isModule && c->fn->replPrint;
      c->fn->replPrint = false;
      msCompilerCompileExpr(c, node->as.exprStmt.expr);
      if (print) {
        msCompilerEmitABC(c, MS_OP_PRINT_EXPR, 0, 0, -1);
      } else if (c->sticky == MS_OK) {
        int slot = msCompilerDiscardSlot(c);
        if (slot >= 0) {
          msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
        }
      }
      break;
    }
    case MS_AST_DECL:
    case MS_AST_ASSIGN:
      msCompilerCompileDeclOrAssign(c, node);
      break;
    case MS_AST_AUG_ASSIGN:
      msCompilerCompileAugAssign(c, node);
      break;
    case MS_AST_BLOCK: {
      int oldDepth = c->fn->scope.scopeDepth;
      msCompilerPushScope(c);
      if (c->fn->scope.scopeDepth != oldDepth) {
        for (size_t i = 0; i < node->as.block.count && c->sticky == MS_OK; ++i) {
          msCompilerCompileStmt(c, node->as.block.items[i]);
        }
        msCompilerPopScope(c);
      }
      break;
    }
    case MS_AST_IF:
      msCompilerCompileIf(c, node);
      break;
    case MS_AST_FOR:
      msCompilerCompileFor(c, node);
      break;
    case MS_AST_GLOBAL:
      msCompilerCompileGlobal(c, node);
      break;
    case MS_AST_PASS:
      break;
    case MS_AST_RETURN:
      msCompilerCompileReturn(c, node);
      break;
    case MS_AST_FUNC_DECL:
      msCompilerCompileFunction(c, node);
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
  msCompilerFreePendingGlobalDecls(fn);
  msScopeCtxDestroy(&fn->scope);
  msFree(fn);
}

static struct MsFuncCtx* msCompilerPushFunc(struct MsCompiler* c, const struct MsAst* funcNode) {
  struct MsFuncCtx* fn = msAlloc(sizeof(*fn));
  if (fn == NULL) {
    msCompilerFail(c, funcNode, MS_ERROR_OOM, 0, "out of memory");
    return NULL;
  }
  memset(fn, 0, sizeof(*fn));
  msScopeCtxInit(&fn->scope, funcNode == NULL);
  fn->parent = c->fn;
  fn->funcNode = funcNode;
  fn->currentNode = funcNode;
  fn->isModule = funcNode == NULL;
  msProtoBuilderInit(&fn->builder);
  c->fn = fn;
  fn->isAsync = funcNode != NULL && funcNode->as.func.isAsync;
  fn->builder.proto.isAsync = fn->isAsync;
  struct MsString* protoName;
  if (funcNode == NULL) {
    protoName = msStringFromCStr(c->heap, "<main>");
  } else if (funcNode->kind == MS_AST_LAMBDA) {
    protoName = msStringFromCStr(c->heap, "<lambda>");
  } else {
    protoName = msStringNew(c->heap, funcNode->as.func.name, funcNode->as.func.nameLen);
  }
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
    fn->builder.proto.localCount = fn->scope.localMax;
    fn->builder.proto.stackSize = fn->stackMax;
    MsResult result = msProtoBuild(&fn->builder, &proto);
    if (result != MS_OK) {
      msCompilerSyncBuilder(c);
    }
  }
  if (c->sticky == MS_OK) {
    c->fn = fn->parent;
    msCompilerFreePendingGlobalDecls(fn);
    msScopeCtxDestroy(&fn->scope);
    msFree(fn);
    *out = proto;
    return MS_OK;
  }
  MsResult result = c->sticky;
  msCompilerAbortFunc(c);
  return result;
}

static bool msCompilerValidateParams(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.func.params.count > MS_SCOPE_MAX_LOCALS) {
    msCompilerFail(c, node, MS_ERROR_SYNTAX, 306, "too many parameter slots");
    return false;
  }
  for (size_t i = 0; i < node->as.func.params.count; ++i) {
    const struct MsAst* param = node->as.func.params.items[i];
    if (param->kind != MS_AST_PARAM || param->as.param.paramKind != MS_PARAM_NORMAL
        || param->as.param.defaultValue != NULL) {
      msCompilerReport(c, param, 307, "default and variadic parameters belong to task 13");
      return false;
    }
  }
  return true;
}

static void msCompilerCompileReturn(struct MsCompiler* c, const struct MsAst* node) {
  if (node->as.returnStmt.values.count > 1) {
    msCompilerReport(c, node, 307, "multiple return values belong to task 13");
    return;
  }
  if (node->as.returnStmt.values.count == 0) {
    msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
  } else {
    msCompilerCompileExpr(c, node->as.returnStmt.values.items[0]);
  }
  msCompilerEmitABC(c, MS_OP_RETURN, 0, 0, -1);
}

static bool msCompilerBuildFunctionProto(struct MsCompiler* c, const struct MsAst* node,
    struct MsProto** out) {
  *out = NULL;
  if (node->as.func.isStatic || !msCompilerValidateParams(c, node)) {
    if (node->as.func.isStatic) {
      msCompilerReport(c, node, 307, "static methods belong to task 15");
    }
    return false;
  }
  if (msCompilerPushFunc(c, node) == NULL) {
    return false;
  }
  for (size_t i = 0; i < node->as.func.params.count; ++i) {
    const struct MsAst* param = node->as.func.params.items[i];
    struct MsString* name = msCompilerNameFromSlice(c, param->as.param.name, param->as.param.nameLen);
    if (name != NULL) {
      msCompilerDeclareLocal(c, name);
    }
  }
  c->fn->builder.proto.paramCount = (int)node->as.func.params.count;
  if (node->kind == MS_AST_LAMBDA) {
    msCompilerCompileExpr(c, node->as.func.body);
    msCompilerEmitABC(c, MS_OP_RETURN, 0, 0, -1);
  } else if (node->as.func.body != NULL && node->as.func.body->kind == MS_AST_BLOCK) {
    for (size_t i = 0; i < node->as.func.body->as.block.count && c->sticky == MS_OK; ++i) {
      msCompilerCompileStmt(c, node->as.func.body->as.block.items[i]);
    }
  } else {
    msCompilerReport(c, node, 307, "function body must be a block");
  }
  return msCompilerPopFunc(c, out) == MS_OK;
}

static void msCompilerBindFunctionName(struct MsCompiler* c, const struct MsAst* node) {
  struct MsString* name = msCompilerNameFromSlice(c, node->as.func.name, node->as.func.nameLen);
  if (name == NULL) {
    return;
  }
  if (c->fn->isModule && c->fn->scope.scopeDepth == 0) {
    MsResult result = msScopeMarkGlobal(&c->fn->scope, name);
    if (result == MS_ERROR_OOM) {
      msCompilerFail(c, node, result, 0, "out of memory");
      return;
    }
    if (result != MS_OK) {
      msCompilerFail(c, node, result, 304, "name is local before global declaration");
      return;
    }
    int index = msProtoAddConst(&c->fn->builder, (struct MsObject*)name);
    if (index >= 0) {
      msCompilerEmitABC(c, MS_OP_STORE_GLOBAL, 0, (uint16_t)index, -1);
    } else {
      msCompilerSyncBuilder(c);
    }
    return;
  }
  int slot = msCompilerDeclareLocal(c, name);
  if (slot >= 0) {
    msCompilerEmitABC(c, MS_OP_STORE_LOCAL, (uint8_t)slot, 0, -1);
  }
}

static void msCompilerCompileFunction(struct MsCompiler* c, const struct MsAst* node) {
  struct MsProto* child = NULL;
  if (!msCompilerBuildFunctionProto(c, node, &child)) {
    if (node->kind == MS_AST_LAMBDA && c->sticky == MS_OK) {
      msCompilerEmitABC(c, MS_OP_LOAD_NIL, 0, 0, 1);
    }
    return;
  }
  int index = msProtoAddProto(&c->fn->builder, child);
  if (index < 0) {
    msProtoFree(child);
    msCompilerSyncBuilder(c);
    return;
  }
  MsOpCode op = node->kind == MS_AST_LAMBDA ? MS_OP_MAKE_LAMBDA : MS_OP_MAKE_FUNCTION;
  msCompilerEmitABC(c, op, 0, (uint16_t)index, 1);
  if (node->kind == MS_AST_FUNC_DECL) {
    msCompilerBindFunctionName(c, node);
  }
}

void msCompilerInit(struct MsCompiler* c, struct MsHeap* heap, struct MsDiagList* diags,
    const char* sourceFile, uint32_t flags) {
  c->heap = heap;
  c->state = NULL;
  c->diags = diags;
  c->sourceFile = sourceFile;
  c->flags = flags;
  c->fn = NULL;
  c->nodeDepth = 0;
  c->sticky = MS_OK;
}

void msCompilerSetState(struct MsCompiler* c, struct MsState* state) {
  c->state = state;
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
