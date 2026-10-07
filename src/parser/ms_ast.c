#include "parser/ms_ast.h"

#include <stdint.h>
#include <string.h>

#include "core/ms_common.h"
#include "core/ms_memory.h"

void msAstArenaInit(struct MsAstArena* arena) {
  arena->head = NULL;
  arena->totalAllocated = 0;
}

void msAstArenaDestroy(struct MsAstArena* arena) {
  struct MsAstArenaBlock* block = arena->head;
  while (block != NULL) {
    struct MsAstArenaBlock* next = block->next;
    msFree(block);
    block = next;
  }
  arena->head = NULL;
  arena->totalAllocated = 0;
}

void* msAstArenaAlloc(struct MsAstArena* arena, size_t size) {
  MS_ASSERT(size > 0);
  if (size > SIZE_MAX - (MS_AST_ARENA_ALIGN - 1)) {
    return NULL;
  }
  // Clamp zero-size requests to one align unit so no two calls alias.
  if (size == 0) {
    size = MS_AST_ARENA_ALIGN;
  }
  size_t aligned = (size + (MS_AST_ARENA_ALIGN - 1)) & ~(size_t)(MS_AST_ARENA_ALIGN - 1);
  struct MsAstArenaBlock* block = arena->head;
  if (block == NULL || block->capacity - block->used < aligned) {
    size_t capacity = aligned > MS_AST_ARENA_BLOCK_SIZE ? aligned : MS_AST_ARENA_BLOCK_SIZE;
    block = msAlloc(sizeof(struct MsAstArenaBlock) + capacity);
    if (block == NULL) {
      return NULL;
    }
    block->next = arena->head;
    block->used = 0;
    block->capacity = capacity;
    arena->head = block;
    arena->totalAllocated += capacity;
  }
  void* result = block->data + block->used;
  block->used += aligned;
  return result;
}

struct MsAst* msAstNew(struct MsAstArena* arena, MsAstKind kind, uint32_t line, uint32_t column) {
  struct MsAst* node = msAstArenaAlloc(arena, sizeof(struct MsAst));
  if (node == NULL) {
    return NULL;
  }
  memset(node, 0, sizeof(*node));
  node->kind = kind;
  node->line = line;
  node->column = column;
  return node;
}

bool msAstListPush(struct MsAstArena* arena, struct MsAstList* list, struct MsAst* node) {
  if (list->count == list->capacity) {
    size_t newCapacity = list->capacity != 0 ? list->capacity * 2 : 4;
    struct MsAst** items = msAstArenaAlloc(arena, newCapacity * sizeof(*items));
    if (items == NULL) {
      return false;
    }
    if (list->items != NULL) {
      memcpy(items, list->items, list->count * sizeof(*items));
    }
    list->items = items;
    list->capacity = newCapacity;
  }
  list->items[list->count] = node;
  ++list->count;
  return true;
}

const char* msAstKindName(MsAstKind kind) {
  static const char* const kKindNames[] = {
      "MS_AST_INVALID",
      "MS_AST_INT",
      "MS_AST_FLOAT",
      "MS_AST_STRING",
      "MS_AST_RAW_STRING",
      "MS_AST_BYTES",
      "MS_AST_BOOL",
      "MS_AST_NIL",
      "MS_AST_IDENT",
      "MS_AST_FSTRING",
      "MS_AST_FSTRING_TEXT",
      "MS_AST_FSTRING_EXPR",
      "MS_AST_LIST_LIT",
      "MS_AST_DICT_LIT",
      "MS_AST_SET_LIT",
      "MS_AST_LIST_COMP",
      "MS_AST_DICT_COMP",
      "MS_AST_SET_COMP",
      "MS_AST_UNARY",
      "MS_AST_BINARY",
      "MS_AST_COMPARE_CHAIN",
      "MS_AST_LOGICAL",
      "MS_AST_CONDITIONAL",
      "MS_AST_AWAIT",
      "MS_AST_LAMBDA",
      "MS_AST_CALL",
      "MS_AST_KW_ARG",
      "MS_AST_INDEX",
      "MS_AST_SLICE",
      "MS_AST_ATTR",
      "MS_AST_SOURCE_FILE",
      "MS_AST_BLOCK",
      "MS_AST_DECL",
      "MS_AST_ASSIGN",
      "MS_AST_AUG_ASSIGN",
      "MS_AST_EXPR_STMT",
      "MS_AST_INC_DEC",
      "MS_AST_IF",
      "MS_AST_FOR",
      "MS_AST_WHILE",
      "MS_AST_TRY",
      "MS_AST_EXCEPT_CLAUSE",
      "MS_AST_WITH",
      "MS_AST_SELECT",
      "MS_AST_SELECT_CASE",
      "MS_AST_IMPORT",
      "MS_AST_FROM_IMPORT",
      "MS_AST_RETURN",
      "MS_AST_RAISE",
      "MS_AST_DEL",
      "MS_AST_GLOBAL",
      "MS_AST_BREAK",
      "MS_AST_CONTINUE",
      "MS_AST_PASS",
      "MS_AST_FUNC_DECL",
      "MS_AST_PARAM",
      "MS_AST_CLASS_DECL",
  };
  MS_ASSERT(MS_ARRAY_LEN(kKindNames) == (size_t)MS_AST_CLASS_DECL + 1);
  MS_ASSERT(kind >= MS_AST_INVALID && kind <= MS_AST_CLASS_DECL);
  return kKindNames[kind];
}

static const char* const msCompareOpNames[] = {
    "MS_CMP_LT", "MS_CMP_LE", "MS_CMP_GT", "MS_CMP_GE", "MS_CMP_EQ",
    "MS_CMP_NE", "MS_CMP_IN", "MS_CMP_NOT_IN", "MS_CMP_IS", "MS_CMP_IS_NOT",
};

static void dumpIndent(FILE* out, int depth) {
  for (int i = 0; i < depth; ++i) {
    fputs("  ", out);
  }
}

static void dumpEscaped(FILE* out, const char* start, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    switch (start[i]) {
      case '\\': fputs("\\\\", out); break;
      case '"': fputs("\\\"", out); break;
      case '\n': fputs("\\n", out); break;
      case '\r': fputs("\\r", out); break;
      case '\t': fputs("\\t", out); break;
      default: fputc(start[i], out); break;
    }
  }
}

static void dumpQuoted(FILE* out, const char* start, size_t length) {
  fputc('"', out);
  dumpEscaped(out, start, length);
  fputc('"', out);
}

static void dumpNode(const struct MsAst* node, FILE* out, int depth);

static void dumpList(FILE* out, const struct MsAstList* list, int depth) {
  for (size_t i = 0; i < list->count; ++i) {
    dumpNode(list->items[i], out, depth);
  }
}

static void dumpNode(const struct MsAst* node, FILE* out, int depth) {
  dumpIndent(out, depth);
  fputs(msAstKindName(node->kind), out);
  switch (node->kind) {
    case MS_AST_INT:
    case MS_AST_FLOAT:
    case MS_AST_STRING:
    case MS_AST_RAW_STRING:
    case MS_AST_BYTES:
    case MS_AST_IDENT:
    case MS_AST_FSTRING_TEXT:
      fputc(' ', out);
      dumpQuoted(out, node->as.literal.start, node->as.literal.length);
      fputc('\n', out);
      return;
    case MS_AST_BOOL:
      fputs(node->as.boolLit.value ? " true" : " false", out);
      fputc('\n', out);
      return;
    case MS_AST_NIL:
    case MS_AST_BREAK:
    case MS_AST_CONTINUE:
    case MS_AST_PASS:
    case MS_AST_INVALID:
      fputc('\n', out);
      return;
    case MS_AST_UNARY:
      fprintf(out, " op=%s", msTokenTypeName(node->as.unary.op));
      fputc('\n', out);
      dumpNode(node->as.unary.operand, out, depth + 1);
      return;
    case MS_AST_BINARY:
    case MS_AST_LOGICAL:
      fprintf(out, " op=%s", msTokenTypeName(node->as.binary.op));
      fputc('\n', out);
      dumpNode(node->as.binary.left, out, depth + 1);
      dumpNode(node->as.binary.right, out, depth + 1);
      return;
    case MS_AST_COMPARE_CHAIN:
      fputs(" ops=", out);
      for (size_t i = 0; i < node->as.compareChain.opCount; ++i) {
        if (i > 0) {
          fputc(',', out);
        }
        fputs(msCompareOpNames[node->as.compareChain.ops[i]], out);
      }
      fputc('\n', out);
      dumpList(out, &node->as.compareChain.operands, depth + 1);
      return;
    case MS_AST_CONDITIONAL:
      fputc('\n', out);
      dumpNode(node->as.conditional.value, out, depth + 1);
      dumpNode(node->as.conditional.cond, out, depth + 1);
      dumpNode(node->as.conditional.elseValue, out, depth + 1);
      return;
    case MS_AST_AWAIT:
      fputc('\n', out);
      dumpNode(node->as.awaitExpr.operand, out, depth + 1);
      return;
    case MS_AST_CALL:
      fputc('\n', out);
      dumpNode(node->as.call.callee, out, depth + 1);
      dumpList(out, &node->as.call.args, depth + 1);
      return;
    case MS_AST_KW_ARG:
      fputc(' ', out);
      dumpQuoted(out, node->as.kwArg.start, node->as.kwArg.length);
      fputc('\n', out);
      dumpNode(node->as.kwArg.value, out, depth + 1);
      return;
    case MS_AST_INDEX:
      fputc('\n', out);
      dumpNode(node->as.index.target, out, depth + 1);
      dumpNode(node->as.index.index, out, depth + 1);
      return;
    case MS_AST_SLICE: {
      fputs(" parts=", out);
      bool first = true;
      if (node->as.slice.start != NULL) {
        fputs("start", out);
        first = false;
      }
      if (node->as.slice.stop != NULL) {
        fputs(first ? "stop" : ",stop", out);
        first = false;
      }
      if (node->as.slice.step != NULL) {
        fputs(first ? "step" : ",step", out);
      }
      fputc('\n', out);
      dumpNode(node->as.slice.target, out, depth + 1);
      if (node->as.slice.start != NULL) {
        dumpNode(node->as.slice.start, out, depth + 1);
      }
      if (node->as.slice.stop != NULL) {
        dumpNode(node->as.slice.stop, out, depth + 1);
      }
      if (node->as.slice.step != NULL) {
        dumpNode(node->as.slice.step, out, depth + 1);
      }
      return;
    }
    case MS_AST_ATTR:
      fputc(' ', out);
      dumpQuoted(out, node->as.attr.start, node->as.attr.length);
      fputc('\n', out);
      dumpNode(node->as.attr.target, out, depth + 1);
      return;
    case MS_AST_LIST_LIT:
    case MS_AST_SET_LIT:
    case MS_AST_DICT_LIT:
    case MS_AST_SELECT:
    case MS_AST_SOURCE_FILE:
    case MS_AST_BLOCK:
      fputc('\n', out);
      dumpList(out, &node->as.block, depth + 1);
      return;
    case MS_AST_LIST_COMP:
    case MS_AST_SET_COMP:
      fputc('\n', out);
      dumpNode(node->as.comprehension.elem, out, depth + 1);
      dumpList(out, &node->as.comprehension.clauses, depth + 1);
      return;
    case MS_AST_DICT_COMP:
      fputc('\n', out);
      dumpNode(node->as.comprehension.key, out, depth + 1);
      dumpNode(node->as.comprehension.elem, out, depth + 1);
      dumpList(out, &node->as.comprehension.clauses, depth + 1);
      return;
    case MS_AST_FSTRING:
      fputc('\n', out);
      dumpList(out, &node->as.fstring.parts, depth + 1);
      return;
    case MS_AST_FSTRING_EXPR:
      if (node->as.fstringExpr.hasFmt) {
        fputs(" fmt=", out);
        dumpQuoted(out, node->as.fstringExpr.fmt, node->as.fstringExpr.fmtLen);
      }
      fputc('\n', out);
      dumpNode(node->as.fstringExpr.expr, out, depth + 1);
      return;
    case MS_AST_DECL:
    case MS_AST_ASSIGN:
      fputc('\n', out);
      dumpList(out, &node->as.decl.targets, depth + 1);
      dumpList(out, &node->as.decl.values, depth + 1);
      return;
    case MS_AST_AUG_ASSIGN:
    case MS_AST_INC_DEC:
      fprintf(out, " op=%s", msTokenTypeName(node->as.augAssign.op));
      fputc('\n', out);
      dumpNode(node->as.augAssign.target, out, depth + 1);
      if (node->as.augAssign.value != NULL) {
        dumpNode(node->as.augAssign.value, out, depth + 1);
      }
      return;
    case MS_AST_EXPR_STMT:
      fputc('\n', out);
      dumpNode(node->as.exprStmt.expr, out, depth + 1);
      return;
    case MS_AST_IF:
      fputc('\n', out);
      dumpNode(node->as.ifStmt.cond, out, depth + 1);
      dumpNode(node->as.ifStmt.thenBlock, out, depth + 1);
      if (node->as.ifStmt.elseBranch != NULL) {
        dumpNode(node->as.ifStmt.elseBranch, out, depth + 1);
      }
      return;
    case MS_AST_FOR:
      switch (node->as.forStmt.forKind) {
        case MS_FOR_BARE: fputs(" kind=MS_FOR_BARE", out); break;
        case MS_FOR_CLAUSE: fputs(" kind=MS_FOR_CLAUSE", out); break;
        case MS_FOR_IN: fputs(" kind=MS_FOR_IN", out); break;
        default: break;  // exhaustive; no default handling
      }
      fputc('\n', out);
      if (node->as.forStmt.forKind == MS_FOR_CLAUSE) {
        if (node->as.forStmt.init != NULL) {
          dumpNode(node->as.forStmt.init, out, depth + 1);
        }
        if (node->as.forStmt.cond != NULL) {
          dumpNode(node->as.forStmt.cond, out, depth + 1);
        }
        if (node->as.forStmt.post != NULL) {
          dumpNode(node->as.forStmt.post, out, depth + 1);
        }
      } else if (node->as.forStmt.forKind == MS_FOR_IN) {
        dumpList(out, &node->as.forStmt.targets, depth + 1);
        dumpNode(node->as.forStmt.iterable, out, depth + 1);
      }
      dumpNode(node->as.forStmt.body, out, depth + 1);
      return;
    case MS_AST_WHILE:
      fputc('\n', out);
      dumpNode(node->as.whileStmt.cond, out, depth + 1);
      dumpNode(node->as.whileStmt.body, out, depth + 1);
      return;
    case MS_AST_TRY:
      fputc('\n', out);
      dumpNode(node->as.tryStmt.body, out, depth + 1);
      dumpList(out, &node->as.tryStmt.excepts, depth + 1);
      if (node->as.tryStmt.finallyBlock != NULL) {
        dumpNode(node->as.tryStmt.finallyBlock, out, depth + 1);
      }
      return;
    case MS_AST_EXCEPT_CLAUSE:
      if (node->as.exceptClause.name != NULL) {
        fputs(" as=", out);
        dumpQuoted(out, node->as.exceptClause.name, node->as.exceptClause.nameLen);
      }
      fputc('\n', out);
      if (node->as.exceptClause.typeExpr != NULL) {
        dumpNode(node->as.exceptClause.typeExpr, out, depth + 1);
      }
      dumpNode(node->as.exceptClause.body, out, depth + 1);
      return;
    case MS_AST_WITH:
      if (node->as.withStmt.name != NULL) {
        fputs(" as=", out);
        dumpQuoted(out, node->as.withStmt.name, node->as.withStmt.nameLen);
      }
      fputc('\n', out);
      dumpNode(node->as.withStmt.context, out, depth + 1);
      dumpNode(node->as.withStmt.body, out, depth + 1);
      return;
    case MS_AST_SELECT_CASE:
      switch (node->as.selectCase.caseKind) {
        case MS_SELECT_RECV: fputs(" kind=recv", out); break;
        case MS_SELECT_SEND: fputs(" kind=send", out); break;
        case MS_SELECT_DEFAULT: fputs(" kind=default", out); break;
        default: break;  // exhaustive; no default handling
      }
      if (node->as.selectCase.isDecl) {
        fputs(" decl", out);
      }
      fputc('\n', out);
      if (node->as.selectCase.caseKind == MS_SELECT_RECV) {
        dumpList(out, &node->as.selectCase.targets, depth + 1);
        dumpNode(node->as.selectCase.channel, out, depth + 1);
      } else if (node->as.selectCase.caseKind == MS_SELECT_SEND) {
        dumpNode(node->as.selectCase.channel, out, depth + 1);
        dumpNode(node->as.selectCase.value, out, depth + 1);
      }
      dumpNode(node->as.selectCase.body, out, depth + 1);
      return;
    case MS_AST_IMPORT:
      fputc(' ', out);
      dumpQuoted(out, node->as.importStmt.path, node->as.importStmt.pathLen);
      if (node->as.importStmt.alias != NULL) {
        fputs(" as=", out);
        dumpQuoted(out, node->as.importStmt.alias, node->as.importStmt.aliasLen);
      }
      fputc('\n', out);
      return;
    case MS_AST_FROM_IMPORT:
      fputc(' ', out);
      dumpQuoted(out, node->as.fromImport.path, node->as.fromImport.pathLen);
      fputc('\n', out);
      dumpList(out, &node->as.fromImport.names, depth + 1);
      return;
    case MS_AST_RETURN:
      fputc('\n', out);
      dumpList(out, &node->as.returnStmt.values, depth + 1);
      return;
    case MS_AST_RAISE:
      fputc('\n', out);
      if (node->as.raiseStmt.value != NULL) {
        dumpNode(node->as.raiseStmt.value, out, depth + 1);
      }
      if (node->as.raiseStmt.fromValue != NULL) {
        dumpNode(node->as.raiseStmt.fromValue, out, depth + 1);
      }
      return;
    case MS_AST_DEL:
      fputc('\n', out);
      dumpList(out, &node->as.delStmt.targets, depth + 1);
      return;
    case MS_AST_GLOBAL:
      fputc('\n', out);
      dumpList(out, &node->as.globalStmt.names, depth + 1);
      return;
    case MS_AST_FUNC_DECL:
      fputc(' ', out);
      dumpQuoted(out, node->as.func.name, node->as.func.nameLen);
      if (node->as.func.isAsync) {
        fputs(" async", out);
      }
      if (node->as.func.isStatic) {
        fputs(" static", out);
      }
      fputc('\n', out);
      dumpList(out, &node->as.func.params, depth + 1);
      dumpNode(node->as.func.body, out, depth + 1);
      return;
    case MS_AST_LAMBDA:
      fputc('\n', out);
      dumpList(out, &node->as.func.params, depth + 1);
      dumpNode(node->as.func.body, out, depth + 1);
      return;
    case MS_AST_PARAM:
      fputc(' ', out);
      dumpQuoted(out, node->as.param.name, node->as.param.nameLen);
      switch (node->as.param.paramKind) {
        case MS_PARAM_NORMAL: fputs(" kind=MS_PARAM_NORMAL", out); break;
        case MS_PARAM_VARARGS: fputs(" kind=MS_PARAM_VARARGS", out); break;
        case MS_PARAM_KWARGS: fputs(" kind=MS_PARAM_KWARGS", out); break;
        default: break;  // exhaustive; no default handling
      }
      fputc('\n', out);
      if (node->as.param.defaultValue != NULL) {
        dumpNode(node->as.param.defaultValue, out, depth + 1);
      }
      return;
    case MS_AST_CLASS_DECL:
      fputc(' ', out);
      dumpQuoted(out, node->as.classDecl.name, node->as.classDecl.nameLen);
      fputc('\n', out);
      if (node->as.classDecl.superclass != NULL) {
        dumpNode(node->as.classDecl.superclass, out, depth + 1);
      }
      dumpNode(node->as.classDecl.body, out, depth + 1);
      return;
    default:
      break;  // every MsAstKind is handled above
  }
}

void msAstDump(const struct MsAst* node, FILE* out) {
  MS_ASSERT(node != NULL);
  dumpNode(node, out, 0);
}
